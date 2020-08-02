#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/signalfd.h>
#endif
#include <stdint.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>

#include "websocket.h"
#include "jeezson/jeezson.h"

#include "program.h"
#include "format.h"
#include "b64.h"

/*
 * TODO: following downloads
 * TODO: naming_convention
 * */

#define XATTR_NAME "user.tags"

static char const *const NONAME = "?";

static int curx = 0;
static int downloads_need_reflow = 0;
static int tag_col_width = 0;
static struct aria_download const *longest_tag;

static int do_forced = 0;
static int oldselidx;
static int selidx = 0;
static int topidx = 0;

static int is_local; /* server runs on local host */

static char *select_gid;
static char *select_file;

static char session_file[PATH_MAX];
static struct pollfd fds[
#ifdef __linux__
	3
#else
	2
#endif
];

#define COLOR_DOWN 1
#define COLOR_UP   2
#define COLOR_CONN 3
#define COLOR_ERR  4
#define COLOR_EOF  5

#define CONTROL(letter) ((letter) - '@')
#define array_len(arr) (sizeof arr / sizeof *arr)

#define VIEWS "\0dfp"
static char view = VIEWS[1];

/* NOTE: order is relevant for sorting */
#define DOWNLOAD_UNKNOWN  0
#define DOWNLOAD_WAITING  1
#define DOWNLOAD_ACTIVE   2
#define DOWNLOAD_PAUSED   3
#define DOWNLOAD_COMPLETE 4
#define DOWNLOAD_REMOVED  5
#define DOWNLOAD_ERROR    6

struct aria_peer {
	char *peerid;

	char ip[INET6_ADDRSTRLEN];
	in_port_t port;

	unsigned up_choked: 1;
	unsigned down_choked: 1;

	uint32_t pieces_have;

	struct timespec peer_measured_at;
	uint32_t peer_download_speed;

	uint32_t download_speed;
	uint32_t upload_speed;
};

struct aria_server {
	char *current_uri;
	uint32_t download_speed;

};
enum aria_uri_status {
	aria_uri_status_waiting,
	aria_uri_status_used,
	aria_uri_status_count
};

struct aria_uri {
	enum aria_uri_status status;
	uint32_t num_servers;
	char *uri;
	struct aria_server *servers;
};

struct aria_file {
	char *path;
	uint64_t total;
	uint64_t have;
	unsigned selected: 1;
	uint32_t num_uris;
	struct aria_uri *uris;
};

struct aria_download {
	char *name;
	char const *display_name;
	char *tags;
	char gid[16 + 1];

	uint8_t refcnt;
	unsigned requested_bittorrent: 1;
	/* DOWNLOAD_*; or negative if changed. */
	int8_t status;
	uint16_t queue_index;

	char *error_message;

	uint32_t num_files;
	uint32_t num_selfiles;

	/* uint32_t num_seeders; */
	uint32_t num_connections;

	uint32_t num_peers;
	uint32_t num_pieces;
	uint32_t piece_size;

	uint64_t total;
	uint64_t have;
	uint64_t uploaded;
	uint64_t verified;

	uint32_t download_speed;
	uint32_t upload_speed;
	uint32_t download_speed_limit;
	uint32_t upload_speed_limit;

	char *dir;

	struct aria_peer *peers;
	struct aria_file *files;
	struct aria_download *belongs_to;
	struct aria_download *following;
};

struct aria_globalstat {
	uint64_t download_speed;
	uint64_t upload_speed;
	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

	uint64_t upload_total;
	/* uint64_t progress_total_total;
	uint64_t progress_have_total; */
	uint64_t have_total;
	uint8_t compute_total;

	unsigned optimize_concurrency: 1;
	unsigned save_session: 1;
	uint32_t max_concurrency;

	uint32_t num_active;
	uint32_t num_waiting;
	uint32_t num_stopped;
	uint32_t num_stopped_total;
};

static struct aria_globalstat globalstat;

static struct aria_download **downloads;
static size_t num_downloads;

typedef void(*rpc_handler)(struct json_node *result, void *arg);
struct rpc_request {
	rpc_handler handler;
	void *arg;
};

static char const* remote_host;
static in_port_t remote_port;
static char *secret_token;

static struct json_writer jw[1];

static struct rpc_request rpc_requests[10];

static void draw_statusline(void);
static void draw_main(void);
static void draw_files(void);
static void draw_peers(void);
static void draw_downloads(void);
static void draw_download(struct aria_download const *d, struct aria_download const *root, int *y);

static void
free_peer(struct aria_peer *p)
{
	free(p->peerid);
}

static void
free_server(struct aria_server *s)
{
	free(s->current_uri);
}

static void
free_uri(struct aria_uri *u)
{
	free(u->uri);

	if (NULL != u->servers) {
		uint32_t i;

		for (i = 0; i < u->num_servers; ++i)
			free_server(&u->servers[i]);

		free(u->servers);
	}
}

static void
free_file(struct aria_file *f)
{
	if (NULL != f->uris) {
		uint32_t i;

		for (i = 0; i < f->num_uris; ++i)
			free_uri(&f->uris[i]);

		free(f->uris);
	}

	free(f->path);
}

static void
free_peers(struct aria_peer *peers, uint32_t num_peers)
{
	uint32_t i;

	if (NULL == peers)
		return;

	for (i = 0; i < num_peers; ++i)
		free_peer(&peers[i]);
}

static void
free_files_of(struct aria_download *d)
{
	uint32_t i;

	if (NULL == d->files)
		return;

	for (i = 0; i < d->num_files; ++i)
		free_file(&d->files[i]);
}

static void
free_download(struct aria_download *d)
{
	free_files_of(d);
	free(d->files);

	free_peers(d->peers, d->num_peers);
	free(d->peers);

	free(d->dir);
	free(d->name);
	free(d->error_message);
	free(d->tags);
	free(d);
}

struct aria_download *
ref_download(struct aria_download *d)
{
	++d->refcnt;
	return d;
}

void
unref_download(struct aria_download *d)
{
	assert(d->refcnt >= 1);
	if (0 == --d->refcnt)
		free_download(d);
}

void
clear_downloads(void)
{
	while (num_downloads > 0)
		unref_download(downloads[--num_downloads]);
}

static void
default_handler(struct json_node *result, void *arg)
{
	(void)result, (void)arg;
	/* just do nothing */
}

int
load_config(void)
{
	char *str;

	(remote_host = getenv("ARIA_RPC_HOST")) ||
	(remote_host = "127.0.0.1");

	if (NULL == (str = getenv("ARIA_RPC_PORT")) ||
	    (errno = 0, remote_port = strtoul(str, NULL, 10), errno))
		remote_port = 6800;

	/* “token:$$secret$$” */
	(str = getenv("ARIA_RPC_SECRET")) ||
	(str = "");

	secret_token = malloc(snprintf(NULL, 0, "token:%s", str) + 1);
	if (NULL == secret_token) {
		perror("malloc()");
		return -1;
	}

	sprintf(secret_token, "token:%s", str);
	return 0;
}

struct aria_download *
new_download(void)
{
	void *p;
	struct aria_download *d;

	p = realloc(downloads, (num_downloads + 1) * sizeof *downloads);
	if (NULL == p)
		return NULL;
	downloads = p;

	d = calloc(1, sizeof *d);
	if (NULL == d)
		return NULL;

	d->refcnt = 1;
	d->display_name = NONAME;
	downloads[num_downloads++] = d;

	return d;
}

static struct aria_download **
find_download(struct aria_download const *d)
{
	struct aria_download **dd = downloads;
	struct aria_download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (d == *dd)
			return dd;

	return NULL;
}

static struct aria_download **
find_download_byfile(char const *filepath, struct aria_download **start)
{
	struct aria_download **dd = NULL != start ? start : downloads;
	struct aria_download **const end = &downloads[num_downloads];
	size_t const len = strlen(filepath);

	for (; dd < end; ++dd) {
		uint32_t i;
		struct aria_download const *d = *dd;

		if (!d->files)
			continue;

		for (i = 0; i < d->num_files; ++i) {
			struct aria_file const *f = &d->files[i];
			if (NULL != f->path && 0 == memcmp(f->path, filepath, len))
				return dd;
		}
	}

	return NULL;
}

static struct aria_download **
find_download_bygid(char const *gid, int create)
{
	struct aria_download *d, **dd = downloads;
	struct aria_download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (0 == memcmp((*dd)->gid, gid, sizeof (*dd)->gid))
			return dd;

	if (!create)
		return NULL;

	if (NULL == (d = new_download()))
		return NULL;

	assert(strlen(gid) == sizeof d->gid - 1);
	memcpy(d->gid, gid, sizeof d->gid);

	dd = &downloads[num_downloads - 1];
	assert(*dd == d);

	return dd;
}

static void
clear_error_message(void)
{
	free(error_message), error_message = NULL;
	draw_statusline();
}

static void
set_error_message(char const *format, ...)
{
	va_list argptr;
	char *p;
	int siz;

	va_start(argptr, format);
	siz = vsnprintf(NULL, 0, format, argptr) + 1;
	va_end(argptr);

	if (NULL == (p = realloc(error_message, siz))) {
		free(error_message);
		error_message = NULL;
	} else {
		error_message = p;

		va_start(argptr, format);
		vsprintf(error_message, format, argptr);
		va_end(argptr);
	}

	draw_statusline();
}

static void
error_handler(struct json_node *error)
{
	struct json_node const *message = json_get(error, "message");

	set_error_message("%s", message->val.str);
	refresh();
}

static void
free_rpc(struct rpc_request *req)
{
	req->handler = NULL;
}

static void
on_downloads_change(int stickycurs);

static void
on_download_status_change(struct aria_download *d)
{
	(void)d;

	on_downloads_change(1);
	refresh();
}

static void
on_notification(char const *method, struct json_node *event)
{
	char *const gid = json_get(event, "gid")->val.str;
	struct aria_download **dd = find_download_bygid(gid, 1);
	struct aria_download *d;
	int newstatus;

	if (NULL == dd)
		return;
	d = *dd;

	if (0 == strcmp(method, "aria2.onDownloadStart")) {
		newstatus = DOWNLOAD_ACTIVE;

	} else if (0 == strcmp(method, "aria2.onDownloadPause")) {
		newstatus = DOWNLOAD_PAUSED;

	} else if (0 == strcmp(method, "aria2.onDownloadStop")) {
		newstatus = DOWNLOAD_PAUSED;

	} else if (0 == strcmp(method, "aria2.onDownloadComplete")) {
		newstatus = DOWNLOAD_COMPLETE;

	} else if (0 == strcmp(method, "aria2.onDownloadError")) {
		newstatus = DOWNLOAD_ERROR;

	} else if (0 == strcmp(method, "aria2.onBtDownloadComplete")) {
		newstatus = DOWNLOAD_COMPLETE;

	} else {
		return;
	}

	if (newstatus != d->status) {
		d->status = -newstatus;
		on_download_status_change(d);
	}
}

void
on_ws_message(char *msg, uint64_t msglen)
{
	static struct json_node *nodes;
	static size_t num_nodes;

	struct json_node *id;
	struct json_node *method;

	(void)msglen;

	json_parse(msg, &nodes, &num_nodes);

	if (NULL != (id = json_get(nodes, "id"))) {
		struct json_node *const result = json_get(nodes, "result");
		struct rpc_request *const req = &rpc_requests[(unsigned)id->val.num];

		if (NULL == result)
			error_handler(json_get(nodes, "error"));

		/* NOTE: this condition shall always be true, but aria2c
		 * responses with some long messages with “Parse error.” that
		 * contains id=null, so we cannot get back the handler. the
		 * best we can do is to ignore and may leak a resource inside
		 * data. */
		if (NULL != req->handler) {
			req->handler(result, req->arg);
			free_rpc(req);
		}

	} else if (NULL != (method = json_get(nodes, "method"))) {
		struct json_node *const params = json_get(nodes, "params");

		on_notification(method->val.str, params + 1);
	} else {
		assert(0);
	}

	/* json_debug(nodes, 0); */
	/* json_debug(json_get(json_get(nodes, "result"), "version"), 0); */
}

static struct rpc_request *
new_rpc(void)
{
	uint8_t n;
	struct rpc_request *req;

	for (n = array_len(rpc_requests); n > 0;) {
		req = &rpc_requests[--n];
		if (NULL == req->handler)
			goto found;
	}

	return NULL;

found:
	req->handler = default_handler;

	json_writer_empty(jw);
	json_write_beginobj(jw);

	json_write_key(jw, "jsonrpc");
	json_write_str(jw, "2.0");

	json_write_key(jw, "id");
	json_write_int(jw, (int)(req - rpc_requests));

	return req;
}


static void
clear_rpc_requests(void)
{
	uint8_t n;

	for (n = array_len(rpc_requests); n > 0;) {
		struct rpc_request *req = &rpc_requests[--n];

		if (NULL != req->handler) {
			req->handler(NULL, req->arg);
			free_rpc(req);
		}
	}
}

static void
update_options(struct aria_download *d, int user);

static void
parse_session_info(struct json_node *result)
{
	char *tmpdir;

	(tmpdir = getenv("TMPDIR")) ||
	(tmpdir = "/tmp");

	snprintf(session_file, sizeof session_file, "%s/aria2t.%s",
			tmpdir,
			json_get(result, "sessionId")->val.str);
}

static void
do_rpc(void)
{
	json_write_endobj(jw);
	ws_write(jw->buf, jw->len);
}

static void
update_tags(struct aria_download *d)
{
	ssize_t siz;
	char *p;

	/* do not try read tags if aria2 is remote */
	if (!is_local)
		return;

	if (NULL == d->tags)
		d->tags = malloc(255 + 1);
	if (NULL == d->tags)
		goto no_tags;

	if (NULL != d->dir && NULL != d->name) {
		char pathbuf[PATH_MAX];

		snprintf(pathbuf, sizeof pathbuf, "%s/%s", d->dir, d->name);
		if (0 < (siz = getxattr(pathbuf, XATTR_NAME, d->tags, 255)))
			goto has_tags;
	}

	if (0 < d->num_files && NULL != d->files[0].path)
		if (0 < (siz = getxattr(d->files[0].path, XATTR_NAME, d->tags, 255)))
			goto has_tags;

	goto no_tags;

has_tags:
	d->tags[siz] = '\0';
	/* transform tags: , -> ' ' */
	for (p = d->tags; NULL != (p += strcspn(p, "\t\r\n")) && '\0' != *p; ++p)
		*p = ' ';

	goto changed;

no_tags:
	free(d->tags), d->tags = NULL;
	goto changed;

changed:
	/* when longest tag changes we have to reset column width; at next draw
	 * it will be recomputed */
	if (longest_tag == d) {
		longest_tag = NULL;
		tag_col_width = 0;
	}
}

static struct aria_uri *
file_finduri(struct aria_file const *f, char const*uri)
{
	uint32_t i;

	for (i = 0; i < f->num_uris; ++i) {
		struct aria_uri *u = &f->uris[i];

		if (0 == strcmp(u->uri, uri))
			return u;
	}

	return NULL;
}

static void
uri_addserver(struct aria_uri *u, struct aria_server *s)
{
	void *p;

	p = realloc(u->servers, (u->num_servers + 1) * sizeof *(u->servers));
	if (NULL == p)
		return;
	(u->servers = p)[u->num_servers++] = *s;
}

static void
parse_file_servers(struct aria_file *f, struct json_node *node)
{
	if (json_isempty(node))
		return;

	node = json_children(node);
	do {
		struct json_node *field = json_children(node);
		struct aria_uri *u = u;
		struct aria_server s;

		do {
			if (0 == strcmp(field->key, "uri")) {
				u = file_finduri(f, field->val.str);
				assert(NULL != u);
			} else if (0 == strcmp(field->key, "downloadSpeed"))
				s.download_speed = strtoul(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "currentUri"))
				s.current_uri = field->val.str;
			else
				assert(!"unknown key in getServers([])");
		} while (NULL != (field = json_next(field)));

		if (0 == strcmp(u->uri, s.current_uri))
			s.current_uri = NULL;
		if (NULL != s.current_uri)
			s.current_uri = strdup(s.current_uri);

		uri_addserver(u, &s);
	} while (NULL != (node = json_next(node)));
}

static void
parse_peer(struct aria_peer *p, struct json_node *node)
{
	struct json_node *field = json_children(node);
	do {
		if (0 == strcmp(field->key, "peerId"))
			p->peerid = strdup(field->val.str);
		else if (0 == strcmp(field->key, "ip"))
			strncpy(p->ip, field->val.str, sizeof p->ip - 1);
		else if (0 == strcmp(field->key, "port"))
			p->port = strtoul(field->val.str, NULL, 10);
		else if (0 == strcmp(field->key, "bitfield")) {
			static uint8_t const HEX_POPCOUNT[] = {
				0, 1, 1, 2, 1, 2, 2, 3,
				1, 2, 2, 3, 2, 3, 3, 4,
			};
			uint32_t pieces_have = 0;
			char *hex;

			for (hex = field->val.str; '\0' != *hex; ++hex)
				pieces_have += HEX_POPCOUNT[*hex <= '9' ? *hex - '0' : *hex - 'a' + 10];

			p->pieces_have = pieces_have;
		} else if (0 == strcmp(field->key, "amChoking"))
			p->up_choked = 0 == strcmp(field->val.str, "true");
		else if (0 == strcmp(field->key, "peerChoking"))
			p->down_choked = 0 == strcmp(field->val.str, "true");
		else if (0 == strcmp(field->key, "downloadSpeed"))
			p->download_speed = strtoul(field->val.str, NULL, 10);
		else if (0 == strcmp(field->key, "uploadSpeed"))
			p->upload_speed = strtoul(field->val.str, NULL, 10);
	} while (NULL != (field = json_next(field)));
}

static void
parse_peers(struct aria_download *d, struct json_node *node)
{
	void *p;
	uint32_t i;
	uint32_t num_oldpeers;
	struct timespec now;
	struct aria_peer *oldpeers;

	num_oldpeers = d->num_peers;
	d->num_peers = json_len(node);
	if (NULL == (p = malloc(d->num_peers * sizeof *(d->peers)))) {
		free(d->peers), d->peers = NULL;
		return;
	}
	oldpeers = d->peers;
	d->peers = p;

	if (json_isempty(node))
		goto free_oldpeers;

	clock_gettime(
#ifdef CLOCK_MONOTONIC_COARSE
		CLOCK_MONOTONIC_COARSE,
#else
		CLOCK_MONOTONIC,
#endif
		&now
	);

	i = 0;
	node = json_children(node);
	do {
		struct aria_peer *p;
		struct aria_peer *oldp;
		uint32_t j;

		parse_peer((p = &d->peers[i]), node);

		/* find new peer among previous ones to being able to compute
		 * its progress/speed change */
		for (j = 0; j < num_oldpeers; ++j) {
			oldp = &oldpeers[j];
			if (/*oldp->port == p->port &&
			    0 == strcmp(oldp->ip, p->ip) &&*/
			    0 == strcmp(oldp->peerid, p->peerid)) {
				goto found_oldpeer;
			}
		}
		/* new peer; already added to the list just initialize */
		p->peer_download_speed = 0;
		p->peer_measured_at = now;
		continue;

	found_oldpeer:
		/* compute delta */
		if (oldp->pieces_have < p->pieces_have) {
#define NS_PER_SEC UINT64_C(1000000000)
			uint64_t pieces_change = p->pieces_have - oldp->pieces_have;
			uint64_t bytes_change = pieces_change * d->piece_size;
			uint64_t ns_change =
				(now.tv_sec - oldp->peer_measured_at.tv_sec) * NS_PER_SEC +
				now.tv_nsec - oldp->peer_measured_at.tv_nsec;

			p->peer_download_speed = (bytes_change * NS_PER_SEC) / ns_change;
			p->peer_measured_at = now;
#undef NS_PER_SEC
		} else {
			/* nothing happened since then */
			p->peer_download_speed = oldp->peer_download_speed;
			p->peer_measured_at = oldp->peer_measured_at;
		}

	} while (++i, NULL != (node = json_next(node)));

free_oldpeers:
	free_peers(oldpeers, num_oldpeers);
}

static void
parse_servers(struct aria_download *d, struct json_node *node)
{
	uint32_t i;

	if (NULL == d->files)
		return;

	/* reset servers for every uri */
	for (i = 0; i < d->num_files; ++i) {
		struct aria_file *f = &d->files[i];
		uint32_t j;

		for (j = 0; j < f->num_uris; ++j) {
			struct aria_uri *u = &f->uris[j];
			u->num_servers = 0;
		}
	}

	if (!json_isempty(node)) {
		node = json_children(node);
		do {
			struct json_node *field = json_children(node);
			struct json_node *servers = servers;
			uint32_t fileidx = fileidx;

			do {
				if (0 == strcmp(field->key, "index"))
					fileidx = atoi(field->val.str) - 1;
				else if (0 == strcmp(field->key, "servers"))
					servers = field;
				else
					assert(!"unknown key in getServers()");
			} while (NULL != (field = json_next(field)));

			assert(fileidx < d->num_files);
			parse_file_servers(&d->files[fileidx], servers);
		} while (NULL != (node = json_next(node)));
	}

	/* now deallocate server lists where num_servers == 0 */
	for (i = 0; i < d->num_files; ++i) {
		struct aria_file *f = &d->files[i];
		uint32_t j;

		for (j = 0; j < f->num_uris; ++j) {
			struct aria_uri *u = &f->uris[j];
			if (0 == u->num_servers)
				free(u->servers), u->servers = NULL;
		}
	}

}

static void
update_display_name(struct aria_download *d)
{
	if (NULL != d->name) {
		d->display_name = d->name;
		return;
	}

	if (d->num_files > 0 && NULL != d->files) {
		struct aria_file *f = &d->files[0];
		if (NULL != f->path) {
			d->display_name = f->path;
			return;
		}

		if (f->num_uris > 0 && NULL != f->uris) {
			uint32_t i;
			for (i = 0; i < f->num_uris; ++i) {
				if (f->uris[i].status == aria_uri_status_used) {
					assert(f->uris[i].uri);
					d->display_name = f->uris[i].uri;
					return;
				}
			}

			assert(f->uris[0].uri);
			d->display_name = f->uris[0].uri;
			return;
		}
	}

	d->display_name = NONAME;
}

static void
parse_download_files(struct aria_download *d, struct json_node *node)
{
	void *p;

	free_files_of(d);

	d->num_files = json_len(node);
	d->num_selfiles = 0;
	if (NULL == (p = realloc(d->files, d->num_files * sizeof *(d->files)))) {
		d->num_files = 0;
		free(d->files), d->files = NULL;
		return;
	} else {
		d->files = p;
	}

	if (json_isempty(node))
		return;

	node = json_children(node);
	do {
		struct json_node *field = json_children(node);
		struct aria_file file;
		int index = -1;

		file.num_uris = 0;
		file.uris = NULL;

		do {
			if (0 == strcmp(field->key, "index"))
				index = atoi(field->val.str) - 1;
			else if (0 == strcmp(field->key, "path"))
				file.path = strlen(field->val.str) > 0 ? strdup(field->val.str) : NULL;
			else if (0 == strcmp(field->key, "length"))
				file.total = strtoull(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "completedLength"))
				file.have = strtoull(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "selected")) {
				file.selected = 0 == strcmp(field->val.str, "true");
				d->num_selfiles += file.selected;
			} else if (0 == strcmp(field->key, "uris")) {
				struct json_node *uris;
				uint32_t uriidx = 0;

				if (json_isempty(field))
					continue;

				uris = json_children(field);
				file.num_uris = json_len(field);
				file.uris = malloc(file.num_uris * sizeof *(file.uris));

				do {
					struct json_node *field = json_children(uris);
					struct aria_uri u;

					u.num_servers = 0;
					u.servers = NULL;
					do {
						if (0 == strcmp(field->key, "status")) {
							if (0 == strcmp(field->val.str, "used"))
								u.status = aria_uri_status_used;
							else if (0 == strcmp(field->val.str, "waiting"))
								u.status = aria_uri_status_waiting;
							else
								assert(0);
						} else if (0 == strcmp(field->key, "uri")) {
							assert(field->val.str);
							u.uri = strdup(field->val.str);
						}
					} while (NULL != (field = json_next(field)));
					file.uris[uriidx++] = u;
				} while (NULL != (uris = json_next(uris)));
			}
			else
				assert(!"unknown key in file");
		} while (NULL != (field = json_next(field)));

		assert(index >= 0);
		d->files[index] = file;
	} while (NULL != (node = json_next(node)));

	update_tags(d);
	update_display_name(d);
}

static void
parse_download(struct aria_download *d, struct json_node *node)
{
	struct json_node *field = json_children(node);

	/* only present if non-zero */
	d->verified = 0;

	do {
		if (0 == strcmp(field->key, "gid")) {
			assert(strlen(field->val.str) == sizeof d->gid - 1);
			memcpy(d->gid, field->val.str, sizeof d->gid);
		} else if (0 == strcmp(field->key, "files")) {
			parse_download_files(d, field);
		} else if (0 == strcmp(field->key, "numPieces")) {
			d->num_pieces = strtoul(field->val.str, NULL, 10);
		} else if (0 == strcmp(field->key, "pieceLength")) {
			d->piece_size = strtoul(field->val.str, NULL, 10);
		} else if (0 == strcmp(field->key, "bittorrent")) {
			struct json_node *bt_info, *bt_name;

			free(d->name);
			d->name = NULL;

			if (NULL != (bt_info = json_get(field, "info")) &&
			    NULL != (bt_name = json_get(bt_info, "name")))
				d->name = strdup(bt_name->val.str);
		} else if (0 == strcmp(field->key, "status")) {
#define IS(statusstr, STATUS) \
	if (0 == strcmp(field->val.str, #statusstr)) \
		d->status = d->status != DOWNLOAD_##STATUS ? -DOWNLOAD_##STATUS : d->status;
			IS(active, ACTIVE);
			IS(waiting, WAITING);
			IS(paused, PAUSED);
			IS(error, ERROR);
			IS(complete, COMPLETE);
			IS(removed, REMOVED);
#undef IS
		}
#define else_if_FIELD(local, name, type) \
else if (0 == strcmp(field->key, name)) \
	d->local = strto##type(field->val.str, NULL, 10);
		else_if_FIELD(download_speed, "downloadSpeed", ul)
		else_if_FIELD(upload_speed, "uploadSpeed", ul)
		else_if_FIELD(total, "totalLength", ull)
		else_if_FIELD(have, "completedLength", ull)
		else_if_FIELD(uploaded, "uploadLength", ull)
		/* else_if_FIELD(num_seeders, "numSeeders", ul) */
		else_if_FIELD(num_connections, "connections", ul)
		else_if_FIELD(verified, "verifiedLength", ul)
#undef else_if_FIELD
		else if (0 == strcmp(field->key, "verifyIntegrityPending")) {
			d->verified = UINT64_MAX;
		} else if (0 == strcmp(field->key, "following")) {
			struct aria_download **dd = find_download_bygid(field->val.str, 0);
			d->following = NULL != dd ? *dd : NULL;
		} else if (0 == strcmp(field->key, "belongsTo")) {
			struct aria_download **dd = find_download_bygid(field->val.str, 0);
			d->belongs_to = NULL != dd ? *dd : NULL;
		} else if (0 == strcmp(field->key, "errorMessage")) {
			free(d->error_message);
			d->error_message = strdup(field->val.str);
		}
	} while (NULL != (field = json_next(field)));

	assert(strlen(d->gid) == 16);
	update_display_name(d);
}

typedef void(*parse_options_cb)(char const *, char const *, void *);

static void
parse_options(struct json_node *node, parse_options_cb cb, void *arg)
{
	if (json_isempty(node))
		return;

	/* First kv-pair.  */
	node = json_children(node);
	do {
		assert("option value must be string" && json_str == json_type(node));
		cb(node->key, node->val.str, arg);
	} while (NULL != (node = json_next(node)));
}

static void
parse_global_stat(struct json_node *node)
{
	if (json_arr != json_type(node))
		return;

	/* Returned object.  */
	node = json_children(node);
	/* First kv-pair.  */
	node = json_children(node);
	do {
		if (0);
#define PARSE_FIELD(field, name, type) \
	else if (0 == strcmp(node->key, name)) \
		globalstat.field = strto##type(node->val.str, NULL, 10);
		PARSE_FIELD(download_speed, "downloadSpeed", ul)
		PARSE_FIELD(upload_speed, "uploadSpeed", ul)
		PARSE_FIELD(num_active, "numActive", ul)
		PARSE_FIELD(num_waiting, "numWaiting", ul)
		PARSE_FIELD(num_stopped, "numStopped", ul)
		PARSE_FIELD(num_stopped_total, "numStoppedTotal", ul)
#undef PARSE_FIELD
	} while (NULL != (node = json_next(node)));
}

static int
download_insufficient(struct aria_download *d)
{
	return d->display_name == NONAME ||
		(-DOWNLOAD_ERROR == d->status && NULL == d->error_message) ||
		(is_local && 0 == d->num_files);
}

static void
parse_option(char const *option, char const *value, struct aria_download *d)
{
	if (NULL != d) {
		if (0 == strcmp(option, "max-download-limit"))
			d->download_speed_limit = atol(value);
		else if (0 == strcmp(option, "max-upload-limit"))
			d->upload_speed_limit = atol(value);
		else if (0 == strcmp(option, "dir")) {
			free(d->dir), d->dir = strdup(value);
			update_tags(d);
		}
	} else {
		if (0 == strcmp(option, "max-concurrent-downloads"))
			globalstat.max_concurrency = atol(value);
		else if (0 == strcmp(option, "save-session"))
			globalstat.save_session = 0 == strcmp(value, "true");
		else if (0 == strcmp(option, "optimize-concurrent-downloads"))
			globalstat.optimize_concurrency = 0 == strcmp(value, "true");
		else if (0 == strcmp(option, "max-overall-download-limit"))
			globalstat.download_speed_limit = atol(value);
		else if (0 == strcmp(option, "max-overall-upload-limit"))
			globalstat.upload_speed_limit = atol(value);
		else if (0 == strcmp(option, "dir"))
			is_local = 0 == access(value, R_OK | W_OK | X_OK);
	}
}

static void
try_connect(void)
{
	ws_connect(remote_host, remote_port);
}

static void
update_delta(int all);

struct periodic_arg {
	unsigned has_get_peers: 1;
	unsigned has_get_servers: 1;
	unsigned has_get_options: 1;
};

static void
parse_downloads(struct json_node *result, struct periodic_arg *arg)
{
	int some_insufficient = 0;

	if (NULL == result)
		return;

	/* No more repsonses if no downloads. */
	for (; NULL != result; result = json_next(result), ++arg) {
		struct json_node *node;
		struct aria_download **dd;
		struct aria_download *d;

		if (json_obj == json_type(result)) {
			error_handler(result);
			goto skip;
		}

		node = json_children(result);
		if (NULL == (dd = find_download_bygid(json_get(node, "gid")->val.str, 1))) {
		skip:
			if (arg->has_get_peers || arg->has_get_servers)
				result = json_next(result);
			if (arg->has_get_options)
				result = json_next(result);
			continue;
		}

		d = *dd;

		/* these fields are not included in response if not
		 * applicable, so we do a self reference loop to
		 * indicate that we do not need to request these next
		 * time */
		if (NULL == d->belongs_to)
			d->belongs_to = d;
		if (NULL == d->following)
			d->following = d;

		parse_download(d, node);

		if (arg->has_get_peers || arg->has_get_servers) {
			result = json_next(result);
			if (json_obj == json_type(result)) {
				error_handler(result);
			} else {
				node = json_children(result);
				(arg->has_get_peers ? parse_peers : parse_servers)(d, node);
			}
		}

		if (arg->has_get_options) {
			result = json_next(result);
			if (json_obj == json_type(result)) {
				error_handler(result);
			} else {
				node = json_children(result);
				parse_options(node, (parse_options_cb)parse_option, d);
			}
		}

		some_insufficient |= download_insufficient(d);
	}

	if (some_insufficient)
		update_delta(1);
}

static void
compute_global(void)
{
	struct aria_download **dd = downloads;
	struct aria_download **const end = &downloads[num_downloads];

	globalstat.have_total = 0;
	globalstat.upload_total = 0;

	for (; dd < end; ++dd) {
		struct aria_download const *const d = *dd;
		globalstat.have_total += d->have;
		globalstat.upload_total += d->uploaded;
	}

	/* reset counter */
	globalstat.compute_total = 0;
}

static void
on_periodic(struct json_node *result, struct periodic_arg *arg)
{
	if (NULL == result)
		goto out;

	/* Result is an array. Go to the first element. */
	result = json_children(result);
	parse_global_stat(result);

	/* there is some activity so we need to recount soon */
	if (globalstat.upload_speed > 0 || globalstat.download_speed > 0)
		compute_global();

	if (globalstat.compute_total > 0)
		compute_global();

#if 0
		++globalstat.compute_total;
	/* activity stopped but there was some before. let's update. */
	else if (globalstat.compute_total > 0)
		compute_global();

#endif

	result = json_next(result);
	if (NULL != result) {
		parse_downloads(result, arg);
		on_downloads_change(1);
	} else {
		draw_statusline();
	}

	refresh();

out:
	free(arg);
}

static int
getmainheight(void)
{
	return getmaxy(stdscr)/*window height*/ - 1/*status line*/;
}

static void
update_delta(int all)
{
	struct rpc_request *req;

	if (!ws_isalive()) {
		try_connect();
		return;
	}

	if (NULL == (req = new_rpc()))
		return;

	req->handler = (rpc_handler)on_periodic;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw); /* {{{ */
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	/* update unconditionally */{
		json_write_beginobj(jw);
		json_write_key(jw, "methodName");
		json_write_str(jw, "aria2.getGlobalStat");
		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, secret_token);
		json_write_endarr(jw);
		json_write_endobj(jw);
	}
	if (num_downloads > 0) {
		struct aria_download **dd;
		int i, n;
		struct periodic_arg *arg;
		if (all && view == VIEWS[1]) {
			dd = &downloads[topidx];
			n = getmainheight();

			if ((size_t)(topidx + n) > num_downloads)
				n = num_downloads - topidx;
		} else {
			dd = &downloads[selidx];
			n = !!num_downloads;
		}

		if (NULL == (arg = malloc(n * sizeof *arg))) {
			free_rpc(req);
			return;
		}
		req->arg = arg;

		for (i = 0; i < n; ++i, ++dd, ++arg) {
			struct aria_download *d = *dd;

			if (d->status < 0)
				globalstat.compute_total = 1;

			json_write_beginobj(jw);
			json_write_key(jw, "methodName");
			json_write_str(jw, "aria2.tellStatus");
			json_write_key(jw, "params");
			json_write_beginarr(jw);
			/* “secret” */
			json_write_str(jw, secret_token);
			/* “gid” */
			json_write_str(jw, d->gid);
			/* “keys” */
			json_write_beginarr(jw);

			json_write_str(jw, "gid");

			if (DOWNLOAD_REMOVED != abs(d->status))
				json_write_str(jw, "status");

			if (NULL == d->name && !d->requested_bittorrent && d->status < 0) {
				/* if (DOWNLOAD_REMOVED != abs(d->status))
					json_write_str(jw, "seeder"); */

				json_write_str(jw, "bittorrent");
				json_write_str(jw, "numPieces");
				json_write_str(jw, "pieceLength");
				d->requested_bittorrent = 1;
			} else if (0 == d->num_files && ((NULL == d->name && d->status >= 0) || is_local)) {
				/* If “bittorrent.info.name” is empty then
				 * assign the name of the first file as name.
				 * */
				json_write_str(jw, "files");
			}

			if (view == 'f')
				if (0 == d->num_files || (d->have != d->total && DOWNLOAD_ACTIVE == abs(d->status)) || d->status < 0)
					json_write_str(jw, "files");

			if (d->status < 0 && d->total == 0) {
				json_write_str(jw, "totalLength");
				json_write_str(jw, "completedLength");
				json_write_str(jw, "uploadLength");
				json_write_str(jw, "uploadSpeed");
			}

			if (NULL == d->belongs_to)
				json_write_str(jw, "belongsTo");

			if (NULL == d->following)
				json_write_str(jw, "following");

			switch (abs(d->status)) {
			case DOWNLOAD_ERROR:
				if (d->status < 0 || NULL == d->error_message)
					json_write_str(jw, "errorMessage");
				break;

			case DOWNLOAD_ACTIVE:
				json_write_str(jw, "verifiedLength");
				json_write_str(jw, "verifyIntegrityPending");

				if (globalstat.download_speed > 0 ||
					d->download_speed > 0) {
					json_write_str(jw, "completedLength");
					json_write_str(jw, "downloadSpeed");
				}

				if (globalstat.upload_speed > 0 ||
					d->upload_speed > 0) {
					json_write_str(jw, "uploadLength");
					json_write_str(jw, "uploadSpeed");
				}

				json_write_str(jw, "connections");
				break;
			}
			d->status = abs(d->status);

			json_write_endarr(jw);

			json_write_endarr(jw);
			json_write_endobj(jw);

			arg->has_get_peers = 0;
			arg->has_get_servers = 0;
			if (NULL != d->name
			    ? (arg->has_get_peers = ('p' == view))
			    : (arg->has_get_servers = ('f' == view))) {
				json_write_beginobj(jw);
				json_write_key(jw, "methodName");
				json_write_str(jw, NULL != d->name ? "aria2.getPeers" : "aria2.getServers");
				json_write_key(jw, "params");
				json_write_beginarr(jw);
				/* “secret” */
				json_write_str(jw, secret_token);
				/* “gid” */
				json_write_str(jw, d->gid);
				json_write_endarr(jw);
				json_write_endobj(jw);
			}

			if ((arg->has_get_options = (NULL == d->dir))) {
				json_write_beginobj(jw);
				json_write_key(jw, "methodName");
				json_write_str(jw, "aria2.getOption");
				json_write_key(jw, "params");
				json_write_beginarr(jw);
				/* “secret” */
				json_write_str(jw, secret_token);
				/* “gid” */
				json_write_str(jw, d->gid);
				json_write_endarr(jw);
				json_write_endobj(jw);
			}

		}
	}

	json_write_endarr(jw); /* }}} */

	json_write_endarr(jw); /* }}} */

	do_rpc();
}

enum pos_how { POS_SET, POS_CUR, POS_END };

static void
aria_download_repos(struct aria_download *d, int32_t pos, enum pos_how how)
{
	static char const *const POS_HOW[] = {
		"POS_SET",
		"POS_CUR",
		"POS_END"
	};

	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	json_write_key(jw, "method");
	json_write_str(jw, "aria2.changePosition");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	/* “pos” */
	json_write_int(jw, pos);
	/* “how” */
	json_write_str(jw, POS_HOW[how]);
	json_write_endarr(jw);

	do_rpc();
}

static void
aria_shutdown(int force)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceShutdown" : "aria2.shutdown");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	json_write_endarr(jw);

	do_rpc();
}

static void
aria_pause_all(int pause, int force)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	json_write_key(jw, "method");
	json_write_str(jw, pause ? (force ? "aria2.forcePauseAll" : "aria2.pauseAll") : "aria2.unpauseAll");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	json_write_endarr(jw);

	do_rpc();
}

static void
remove_download_at(struct aria_download **dd)
{
	struct aria_download *d = *dd;

	*dd = downloads[--num_downloads];

	if (longest_tag == d) {
		tag_col_width = 0;
		longest_tag = NULL;
	}

	unref_download(d);
}

static void
on_remove_result(struct json_node *result, struct aria_download *d)
{
	if (NULL != result) {
		struct aria_download **dd;

		if (NULL != d) {
			if (NULL != (dd = find_download(d)))
				remove_download_at(dd);
		} else {
			struct aria_download **end = &downloads[num_downloads];
			for (dd = downloads; dd < end;) {
				switch (abs((*dd)->status)) {
				case DOWNLOAD_COMPLETE:
				case DOWNLOAD_ERROR:
				case DOWNLOAD_REMOVED:
					remove_download_at(dd);
					--end;
					break;

				default:
					++dd;
				}
			}
		}

		on_downloads_change(1);
		refresh();
	}

	if (NULL != d)
		unref_download(d);
}

static void
aria_download_pause(struct aria_download *d, int pause, int force)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	json_write_key(jw, "method");
	json_write_str(jw, pause ? (force ? "aria2.forcePause" : "aria2.pause") : "aria2.unpause");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	do_rpc();
}

static void
aria_remove_result(struct aria_download *d)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	req->handler = (rpc_handler)on_remove_result;
	req->arg = NULL != d ? ref_download(d) : NULL;

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.removeDownloadResult" : "aria2.purgeDownloadResult");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	if (NULL != d) {
		/* “gid” */
		json_write_str(jw, d->gid);
	}
	json_write_endarr(jw);

	do_rpc();
}

/* tsl [title] fsl */

static void draw_main(void)
{
	switch (view) {
	case 'd':
		draw_downloads();
		break;

	case 'f':
		draw_files();
		break;

	case 'p':
		draw_peers();
		break;
	}

	draw_statusline();
}

static void
draw_peer(struct aria_download const *d, size_t i, int *y)
{
	struct aria_peer const *p = &d->peers[i];
	char fmtbuf[5];
	int n;
	int w = getmaxx(stdscr);
	int ipw = w > (int)sizeof p->ip + 30 ? sizeof p->ip : 0;

	attr_set(A_BOLD, 0, NULL);
	mvprintw(*y, 0, "  %*.*s", ipw, ipw, p->ip);

	attr_set(A_NORMAL, 0, NULL);
	printw(":%-5u  ", p->port);

	n = fmt_percent(fmtbuf, p->pieces_have, d->num_pieces);
	addnstr(fmtbuf, n);

	if (p->peer_download_speed > 0) {
		addstr(" @ ");
		n = fmt_speed(fmtbuf, p->peer_download_speed);
		addnstr(fmtbuf, n);
	} else {
		addstr("   " "    ");
	}
	addstr("  ");

	if (p->down_choked) {
		addstr("----");
	} else if (p->download_speed > 0) {
		attr_set(A_BOLD, COLOR_DOWN, NULL);
		n = fmt_speed(fmtbuf, p->download_speed);
		addnstr(fmtbuf, n);
		attr_set(A_NORMAL, 0, NULL);
	} else {
		addstr("    ");
	}
	addstr(" ");

	if (p->up_choked) {
		addstr("----");
	} else if (p->upload_speed > 0) {
		attr_set(A_BOLD, COLOR_UP, NULL);
		n = fmt_speed(fmtbuf, p->upload_speed);
		addnstr(fmtbuf, n);
		attr_set(A_NORMAL, 0, NULL);
	} else {
		addstr("    ");
	}
	addstr("  ");
	addstr(p->peerid);
	clrtoeol();

	++*y;
}

static void
draw_peers(void)
{
	if (num_downloads > 0) {
		struct aria_download *d = downloads[selidx];
		int y = 0;

		draw_download(d, d, &y);

		if (NULL != d->peers) {
			size_t i;

			for (i = 0; i < d->num_peers; ++i)
				draw_peer(d, i, &y);

			/* linewrap */
			move(y, 0);
		}

	} else {
		move(0, 0);
	}
	clrtobot();
	/* move(0, curx); */
}

static void
draw_file(struct aria_download const *d, size_t i, int *y)
{
	struct aria_file const *f = &d->files[i];
	char szhave[6];
	char sztotal[6];
	char szpercent[6];
	uint32_t j;

	szhave[fmt_space(szhave, f->have)] = '\0';
	sztotal[fmt_space(sztotal, f->total)] = '\0';
	szpercent[fmt_percent(szpercent, f->have, f->total)] = '\0';

	attr_set(A_BOLD, 0, NULL);
	mvprintw((*y)++, 0, "  %5d: ", i + 1);

	attr_set(A_NORMAL, 0, NULL);
	printw("%s/%s[", szhave, sztotal);

	if (f->selected) {
		attr_set(A_NORMAL, COLOR_DOWN, NULL);
		addstr(szpercent);
		attr_set(A_NORMAL, 0, NULL);
	} else {
		char *p;

		for (p = szpercent; *p; ++p)
			*p = '-';

		addstr(szpercent);
	}

	printw("] %s", f->path ? f->path : f->selected ? "(not downloaded yet)" : "(none)");
	clrtoeol();

	for (j = 0; j < f->num_uris; ++j) {
		struct aria_uri const *u = &f->uris[j];
		uint32_t k;

		attr_set(A_NORMAL, 0, NULL);
		mvprintw((*y)++, 0, "      %s╴",
				j + 1 < f->num_uris ? "├" : "└");

		attr_set(u->status == aria_uri_status_used ? A_BOLD : A_NORMAL, 0, NULL);
		printw("%3d%s ",
				j + 1,
				u->status == aria_uri_status_used ? "*" : " ");
		attr_set(A_NORMAL, 0, NULL);
		addstr(u->uri);
		clrtoeol();

		for (k = 0; k < u->num_servers; ++k) {
			struct aria_server const *s = &u->servers[k];
			char szspeed[6];

			szspeed[fmt_speed(szspeed, s->download_speed)] = '\0';
			if (NULL != s->current_uri)
				mvprintw((*y)++, 0, "      %s   ↪ %s",
						j + 1 < f->num_uris ? "│" : " ",
						s->current_uri);
			mvprintw((*y)++, 0, "      %s   %s",
					j + 1 < f->num_uris ? "│" : " ",
					szspeed);
			clrtoeol();
		}
	}
}


static void
on_download_getfiles(struct json_node *result, struct aria_download *d)
{
	if (NULL != result) {
		parse_download_files(d, result);
		draw_main();
		refresh();
	}

	unref_download(d);
}

static void
aria_download_getfiles(struct aria_download *d)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	req->handler = (rpc_handler)on_download_getfiles;
	req->arg = ref_download(d);
	json_write_key(jw, "method");
	json_write_str(jw, "aria2.getFiles");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	do_rpc();
}

static void
draw_files(void)
{
	if (num_downloads > 0) {
		struct aria_download *d = downloads[selidx];
		int y = 0;

		draw_download(d, d, &y);

		if (NULL != d->files) {
			size_t i;

			for (i = 0; i < d->num_files; ++i)
				draw_file(d, i, &y);

			/* linewrap */
			move(y, 0);
		} else {
			aria_download_getfiles(d);
		}

	} else {
		move(0, 0);
	}
	clrtobot();
	move(0, curx);
}

static int
downloadcmp(struct aria_download const **pthis, struct aria_download const **pother)
{
	int cmp;
	uint64_t x, y;
	struct aria_download const *this = *pthis;
	struct aria_download const *other = *pother;
	int this_status = abs(this->status);
	int other_status = abs(other->status);

	/* cmp = this->belongs_to ==  */
	/* sort by state */
	cmp = this_status - other_status;
	if (cmp)
		return cmp;

	if (DOWNLOAD_WAITING == this_status) {
		/* sort by position in queue */
		return this->queue_index - other->queue_index;
	}

	/* prefer incomplete downloads */
	cmp = (other->have != other->total) -
		(this->have != this->total);
	if (cmp)
		return cmp;

	if (DOWNLOAD_ACTIVE == this_status) {
		/* prefer downloads that almost completed */
		/* NOTE: total can be null for example in cases if there is no
		 * information about the download. */
		x = this->total > 0
			? this->have * 10000 / this->total
			: 0;
		y = other->total > 0
			? other->have * 10000 / other->total
			: 0;
		if (x != y)
			return x > y ? -1 : 1;

		/* prefer higher upload speed */
		if (this->upload_speed != other->upload_speed)
			return this->upload_speed > other->upload_speed ? -1 : 1;
	}

	/* prefer better upload ratio */
	x = this->total > 0
		? this->uploaded * 10000 / this->total
		: 0;
	y = other->total > 0
		? other->uploaded * 10000 / other->total
		: 0;
	if (x != y)
		return x > y ? -1 : 1;

	if (this->uploaded != other->uploaded)
		return this->uploaded > other->uploaded ? -1 : 1;

	/* sort by name */
	return strcoll(this->display_name, other->display_name);
}

/* draw download d at screen line y */
static void
draw_download(struct aria_download const *d, struct aria_download const *root, int *y)
{
	char fmtbuf[5];
	int n;
	int tagwidth;

	(void)root;

	attr_set(A_NORMAL, 0, NULL);
	mvaddstr(*y, 0, d->gid);
	addstr(" ");
	{
		/* struct aria_download *child = d; */
		/* struct aria_download *next;
		for (; child != root && child && (next = child->belongs_to) != child; child = next) */
		if (d->belongs_to != d && d->belongs_to)
			addstr("  ");
	}

	attr_set(A_BOLD, -1, NULL);
	switch (abs(d->status)) {
	case DOWNLOAD_UNKNOWN:
		printw("%31s", "");
		break;

	case DOWNLOAD_REMOVED:
		addstr("- ");
		attr_set(A_NORMAL, 0, NULL);
		break;

	case DOWNLOAD_ERROR: {
		int usable_width = 29 + (NULL == d->tags ? tag_col_width : 0);
		addstr("* ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		printw("%-*.*s", usable_width, usable_width, NULL != d->error_message && (strlen(d->error_message) <= 30 || view == VIEWS[1]) ? d->error_message : "");
		attr_set(A_NORMAL, 0, NULL);

		if (NULL == d->tags)
			goto skip_tags;
	}
		break;

	case DOWNLOAD_WAITING:
		printw("%-11d", d->queue_index);

		attr_set(A_NORMAL, 0, NULL);
		if (0 < d->total) {
			n = fmt_space(fmtbuf, d->total);
			addnstr(fmtbuf, n);
		} else {
			addstr("    ");
		}
		addstr(" ");

		goto print_files;

	case DOWNLOAD_PAUSED:
		addstr("|  ");
		attr_set(A_NORMAL, 0, NULL);

		n = fmt_space(fmtbuf, d->total);
		addnstr(fmtbuf, n);

		addstr("[");

		n = fmt_percent(fmtbuf, d->have, d->total);
		addnstr(fmtbuf, n);

		addstr("], ");

	print_files:
		if (d->num_files > 0) {
			if (d->num_files != d->num_selfiles) {
				n = fmt_number(fmtbuf, d->num_selfiles);
				addnstr(fmtbuf, n);

				addstr("/");
			} else {
				addstr("     ");
			}

			n = fmt_number(fmtbuf, d->num_files);
			addnstr(fmtbuf, n);

			addstr(1 == d->num_files ? " file " : " files");
		} else {
			addstr("    " " " "    " "      ");
		}
		break;

	case DOWNLOAD_COMPLETE:
		addstr("          ");

		attr_set(A_NORMAL, 0, NULL);

		n = fmt_space(fmtbuf, d->total);
		addnstr(fmtbuf, n);

		addstr(",      ");

		n = fmt_number(fmtbuf, d->num_selfiles);
		addnstr(fmtbuf, n);
		addstr(" files");
		break;

	case DOWNLOAD_ACTIVE:
		addstr(d->verified == 0 ? "> " : "v ");
		attr_set(A_NORMAL, 0, NULL);

		if (d->verified == 0) {
			if (d->download_speed > 0) {
				attr_set(A_BOLD, COLOR_DOWN, NULL);
				n = fmt_percent(fmtbuf, d->have, d->total);
				addnstr(fmtbuf, n);

				attr_set(A_NORMAL, 0, NULL);
				addstr(" @ ");

				attr_set(A_BOLD, COLOR_DOWN, NULL);
				n = fmt_speed(fmtbuf, d->download_speed);
				addnstr(fmtbuf, n);
				addstr(" ");
			} else {
				addstr(" ");
				n = fmt_space(fmtbuf, d->total);
				addnstr(fmtbuf, n);

				addstr("[");
				attr_set(d->have == d->total ? A_NORMAL : A_BOLD, COLOR_DOWN, NULL);
				n = fmt_percent(fmtbuf, d->have, d->total);
				addnstr(fmtbuf, n);
				attr_set(A_NORMAL, 0, NULL);
				addstr("] ");
			}
		} else if (d->verified == UINT64_MAX) {
			addstr(" ");
			n = fmt_space(fmtbuf, d->have);
			addnstr(fmtbuf, n);

			addstr("       ");
		} else {
			addstr(" ");
			n = fmt_space(fmtbuf, d->verified);
			addnstr(fmtbuf, n);

			addstr("[");
			n = fmt_percent(fmtbuf, d->verified, d->have);
			addnstr(fmtbuf, n);
			addstr("] ");
		}

		attr_set(A_NORMAL, COLOR_CONN, NULL);
		if (d->num_connections > 0) {
			n = fmt_number(fmtbuf, d->num_connections);
			addnstr(fmtbuf, n);
		} else {
			addstr("    ");
		}
		attr_set(A_NORMAL, 0, NULL);

		if (d->uploaded > 0) {
			if (d->upload_speed > 0)
				attr_set(A_BOLD, COLOR_UP, NULL);
			addstr(" ");

			n = fmt_space(fmtbuf, d->uploaded);
			addnstr(fmtbuf, n);

			if (d->upload_speed > 0) {
				attr_set(A_NORMAL, 0, NULL);
				addstr(" @ ");

				attr_set(A_BOLD, COLOR_UP, NULL);
				n = fmt_speed(fmtbuf, d->upload_speed);
				addnstr(fmtbuf, n);
			} else {
				addstr("[");

				attr_set(A_NORMAL, COLOR_UP, NULL);

				n = fmt_percent(fmtbuf, d->uploaded, d->total);
				addnstr(fmtbuf, n);

				attr_set(A_NORMAL, 0, NULL);

				addstr("]");
			}

			attr_set(A_NORMAL, 0, NULL);
		} else {
			printw("%12s", "");
		}

		break;
	}

	if (NULL != d->tags) {
		int oy, ox, ny, nx, mx;

		getyx(stdscr, oy, ox);

		addstr(" ");
		addstr(d->tags);

		mx = getmaxx(stdscr);
		getyx(stdscr, ny, nx);

		tagwidth = (ny - oy) * mx + (nx - ox);
		if (tag_col_width < tagwidth) {
			longest_tag = d; /*no ref*/
			tag_col_width = tagwidth;
			downloads_need_reflow = 1;
		}
	} else {
		tagwidth = 0;
	}
	if (0 < tag_col_width) {
		printw("%*.s", tag_col_width - tagwidth, "");
	}

skip_tags:
	addstr(" ");
	curx = getcurx(stdscr);
	addstr(d->display_name);
	clrtoeol();
	++*y;

	switch (abs(d->status)) {
	case DOWNLOAD_ERROR:
		if (view != VIEWS[1] && NULL != d->error_message && strlen(d->error_message) > 30) {
			attr_set(A_BOLD, COLOR_ERR, NULL);
			mvaddstr(*y, 0, "  ");
			addstr(d->error_message);
			attr_set(A_NORMAL, 0, NULL);
			clrtoeol();
			/* error message may span multiple lines */
			*y = getcury(stdscr) + 1;
		}
		break;
	}
}

static void
draw_downloads(void)
{
	int line, height = getmainheight();

	do {
		downloads_need_reflow = 0;
		assert(0 <= topidx);
		for (line = 0; line < height;) {
			if ((size_t)(topidx + line) < num_downloads) {
				draw_download(downloads[topidx + line], NULL, &line);
			} else {
				attr_set(A_NORMAL, COLOR_EOF, NULL);
				mvaddstr(line, 0, "~");
				attr_set(A_NORMAL, 0, NULL);
				clrtoeol();
				++line;
			}
		}
	} while (downloads_need_reflow);

	draw_statusline();
}

static void
on_downloads_change(int stickycurs)
{
	/* re-sort downloads list */
	if (num_downloads > 0) {
		char selgid[sizeof ((struct aria_download *)0)->gid];

		if (selidx < 0)
			selidx = 0;

		if ((size_t)selidx >= num_downloads)
			selidx = num_downloads - 1;
		memcpy(selgid, downloads[selidx]->gid, sizeof selgid);

		qsort(downloads, num_downloads, sizeof *downloads, (int(*)(void const *, void const *))downloadcmp);

		/* move selection if download moved */
		if (stickycurs && 0 != memcmp(selgid, downloads[selidx]->gid, sizeof selgid)) {
			struct aria_download **dd = find_download_bygid(selgid, 1);

			assert("selection disappeared after sorting" && NULL != dd);
			selidx = dd - downloads;
			assert(selidx < (int)num_downloads);
		}
	}

	/* then also update them on the screen */
	draw_main();
}

static void
on_scroll_changed(void)
{
	draw_main();
}

static void
draw_cursor(void)
{
	int const h = view == VIEWS[1] ? getmainheight() : 1;
	int const oldtopidx = topidx;

	(void)curs_set(num_downloads > 0);

	if (selidx < 0)
		selidx = 0;

	if ((size_t)selidx >= num_downloads)
		selidx = (int)num_downloads - 1;

	if (selidx < 0)
		selidx = 0;

	/* scroll selection into view */
	if (selidx < topidx)
		topidx = selidx;

	if (topidx + h <= selidx)
		topidx = selidx - (h - 1);

	if ((size_t)(topidx + h) >= num_downloads)
		topidx = num_downloads > (size_t)h ? num_downloads - (size_t)h : 0;

	if (topidx == oldtopidx) {
		move(view == VIEWS[1] ? selidx - topidx : 0, curx);
	} else if (oldselidx != selidx) {
		on_scroll_changed();
		/* update now seen downloads */
		update_delta(1);
	}

	if (oldselidx != selidx) {
		oldselidx = selidx;
		draw_statusline();
	}
}

static void
draw_statusline(void)
{
	int y, x, w;
	char fmtbuf[5];
	int n;

	getmaxyx(stdscr, y, w);

	/* print downloads info at the left */
	--y, x = 0;

	move(y, 0);
	attr_set(A_NORMAL, 0, NULL);

	printw("[%c %4d/%-4d] [%4d/%4d|%4d] [%s%s%s%dc] @ %s:%d%s",
			view,
			num_downloads > 0 ? selidx + 1 : 0, num_downloads,
			globalstat.num_active,
			globalstat.num_waiting,
			globalstat.num_stopped,
			is_local ? "L" : "",
			globalstat.save_session ? "S" : "",
			globalstat.optimize_concurrency ? "O" : "",
			globalstat.max_concurrency,
			remote_host, remote_port,
			ws_isalive() ? "" : " (not connected)");

	if (NULL != error_message) {
		addstr(": ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		addstr(error_message);
		attr_set(A_NORMAL, 0, NULL);
	}

	clrtoeol();

	/* build bottom-right widgets from right-to-left */
	x = w;
	mvaddstr(y, x -= 1, " ");

	/* upload */

	if (globalstat.upload_speed_limit > 0) {
		n = fmt_speed(fmtbuf, globalstat.upload_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	if (globalstat.upload_speed > 0)
		attr_set(A_BOLD, COLOR_UP, NULL);
	n = fmt_speed(fmtbuf, globalstat.upload_speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 4, "] @ ");
	n = fmt_percent(fmtbuf, globalstat.upload_total, globalstat.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);
	mvaddstr(y, x -= 1, "[");

	n = fmt_speed(fmtbuf, globalstat.upload_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	attr_set(globalstat.upload_speed > 0 ? A_BOLD : A_NORMAL, 0, NULL);
	mvaddstr(y, x -= 3, " ↑ ");
	attr_set(A_NORMAL, 0, NULL);

	/* download */
	if (globalstat.download_speed_limit > 0) {
		n = fmt_speed(fmtbuf, globalstat.download_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	if (globalstat.download_speed > 0)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
	n = fmt_speed(fmtbuf, globalstat.download_speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, " @ ");

	n = fmt_speed(fmtbuf, globalstat.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	attr_set(globalstat.download_speed > 0 ? A_BOLD : A_NORMAL, 0, NULL);
	mvaddstr(y, x -= 3, " ↓ ");
	attr_set(A_NORMAL, 0, NULL);

	draw_cursor();
}

static void
draw_all(void)
{
	draw_main();
	draw_cursor();
}

static void
on_update_all(struct json_node *result, void *arg)
{
	int some_insufficient = 0;

	(void)arg;

	if (NULL == result)
		return;

	clear_downloads();

	/* create a fake download with that gid so user will be notified if
	 * does not exist */
	if (NULL != select_gid) {
		struct aria_download **dd;
		if (NULL != (dd = find_download_bygid(select_gid, 1)))
			strncpy((*dd)->gid, select_gid, sizeof (*dd)->gid - 1);
	}

	result = json_children(result);
	do {
		struct json_node *downloads_list;
		struct json_node *node;
		uint32_t downloadidx;

		if (json_arr != json_type(result)) {
			error_handler(result);
			continue;
		}

		downloads_list = json_children(result);

		if (json_isempty(downloads_list))
			continue;

		downloadidx = num_downloads;
		node = json_children(downloads_list);
		do {
			struct aria_download *d = new_download();
			if (NULL == d)
				continue;

			/* we did it initially but we have to mark it now since
			 * we only received download object now */
			d->requested_bittorrent = 1;

			parse_download(d, node);
			if (DOWNLOAD_WAITING == abs(d->status)) {
				/* FIXME: warning: it is some serious shit */
				d->queue_index = (num_downloads - 1) - downloadidx;
			}

			some_insufficient |= download_insufficient(d);
		} while (NULL != (node = json_next(node)));

	} while (NULL != (result = json_next(result)));

	on_downloads_change(0);

	if (NULL != select_file) {
		struct aria_download **dd;

		if (NULL != (dd = find_download_byfile(select_file, NULL))) {
			view = 'f';
			/* update view immediately */
			some_insufficient = 1;

			selidx = dd - downloads;

			draw_cursor();
		}
	}

	refresh();

	if (some_insufficient)
		update_delta(1);
}

static void
runaction_maychanged(struct aria_download *d)
{
	if (NULL != d) {
		update_tags(d);
		draw_main();
	}
}

/* Returns:
 * - <0: action did not run.
 * - =0: action executed and terminated successfully.
 * - >0: action executed but failed. */
static int
run_action(struct aria_download *d, const char *name, ...)
{
	char filename[PATH_MAX];
	char filepath[PATH_MAX];
	va_list argptr;
	pid_t pid;
	int status;

	va_start(argptr, name);
	vsnprintf(filename, sizeof filename, name, argptr);
	va_end(argptr);

	if (NULL == d && 0 < num_downloads)
		d = downloads[selidx];

	if (getenv("ARIA2T_CONFIG"))
		snprintf(filepath, sizeof filepath, "%s/actions/%s",
				getenv("ARIA2T_CONFIG"), filename);
	else if (getenv("HOME"))
		snprintf(filepath, sizeof filepath, "%s/.config/aria2t/actions/%s",
				getenv("HOME"), filename);
	else
		return -1;

	def_prog_mode();
	endwin();

	if (0 == (pid = fork())) {
		execlp(filepath, filepath,
				NULL != d ? d->gid : "",
				session_file,
				NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, &status, 0) && errno == EINTR)
		;

	refresh();

	if (WIFEXITED(status) && 127 == WEXITSTATUS(status)) {
		return -1;
	} else if (WIFEXITED(status) && EXIT_SUCCESS == WEXITSTATUS(status)) {
		runaction_maychanged(d);
		return EXIT_SUCCESS;
	} else {
		return EXIT_FAILURE;
	}
}

static char *
file_b64_enc(char const *pathname)
{
	int fd;
	unsigned char *buf;
	char *b64 = NULL;
	size_t b64len;
	struct stat st;

	if (-1 == (fd = open(pathname, O_RDONLY)))
		goto out;

	if (-1 == fstat(fd, &st))
		goto out_close;

	buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (MAP_FAILED == buf)
		goto out_close;

	b64 = b64_enc(buf, st.st_size, &b64len);

	munmap(buf, st.st_size);

out_close:
	close(fd);
out:
	return b64;
}

static char *
stripwhite(char *str, size_t *n)
{
	while (isspace(str[*n - 1]))
		--*n;
	str[*n] = '\0';

	while (isspace(str[0]))
		++str, --*n;

	return str;
}

static int
fileout(int must_edit)
{
	char *prog;
	pid_t pid;
	int status;

	if (must_edit) {
		(prog = getenv("VISUAL")) ||
		(prog = getenv("EDITOR")) ||
		(prog = "vi");
	} else {
		(prog = getenv("PAGER")) ||
		(prog = "less");
	}

	def_prog_mode();
	endwin();

	if (0 == (pid = fork())) {
		execlp(prog, prog, session_file, NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, &status, 0) && errno == EINTR)
		;

	refresh();

	return WIFEXITED(status) && EXIT_SUCCESS == WEXITSTATUS(status)
		? EXIT_SUCCESS
		: EXIT_FAILURE;
}

static void
write_option(char const *option, char const *value, FILE *file)
{
	fprintf(file, "%s\t%s\n", option, value);
}

static void
on_options(struct json_node *result, struct aria_download *d)
{
	if (NULL != result) {
		clear_error_message();
		parse_options(result, (parse_options_cb)parse_option, d);
	}

	if (NULL != d)
		unref_download(d);
}

static void
on_options_change(struct json_node *result, struct aria_download *d)
{
	if (NULL != result)
		update_options(d, 0);
}

static void
on_show_options(struct json_node *result, struct aria_download *d)
{
	FILE *f;
	int err;
	char *line;
	size_t linesiz;
	struct rpc_request *req;
	int action;
	struct stat stbefore, stafter;

	if (NULL == result)
		goto out;

	clear_error_message();

	if (NULL == (f = fopen(session_file, "w")))
		goto out;
	(void)fstat(fileno(f), &stbefore);

	parse_options(result, (parse_options_cb)write_option, f);
	parse_options(result, (parse_options_cb)parse_option, d);

	fclose(f);

	if ((action = run_action(NULL, NULL != d ? "i" : "I")) < 0)
		action = fileout(0);

	if (EXIT_SUCCESS != action)
		goto out;

	if (NULL == (f = fopen(session_file, "r")))
		goto out;
	(void)fstat(fileno(f), &stafter);

	/* not modified */
	if (stbefore.st_mtim.tv_sec == stafter.st_mtim.tv_sec)
		goto out_fclose;

	req = new_rpc();
	if (NULL == req)
		goto out_fclose;

	req->handler = (rpc_handler)on_options_change;
	req->arg = NULL != d ? ref_download(d) : NULL;

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.changeOption" : "aria2.changeGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	if (NULL != d) {
		/* “gid” */
		json_write_str(jw, d->gid);
	}
	json_write_beginobj(jw);

	line = NULL, linesiz = 0;
	while (-1 != (err = getline(&line, &linesiz, f))) {
		char *name, *value;
		size_t len;

		if (NULL == (value = strchr(line, '\t')))
			continue;
		*value = '\0';
		if (line[err - 1] == '\n')
			line[err - 1] = '\0';

		len = value - line;
		name = stripwhite(line, &len);

		len = (err - 1) - ((value + 1) - line);
		value = stripwhite(value + 1, &len);

		json_write_key(jw, name);
		json_write_str(jw, value);

	}

	json_write_endobj(jw);
	json_write_endarr(jw);

	free(line);

	do_rpc();

out_fclose:
	fclose(f);

out:
	if (NULL != d)
		unref_download(d);
}

/* if user requested it, show it. otherwise just update returned values in the
 * background */
static void
update_options(struct aria_download *d, int user)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	req->handler = (rpc_handler)(user ? on_show_options : on_options);
	req->arg = NULL != d ? ref_download(d) : NULL;

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.getOption" : "aria2.getGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	if (NULL != d) {
		/* “gid” */
		json_write_str(jw, d->gid);
	}
	json_write_endarr(jw);

	do_rpc();
	/* update_delta(NULL != d ? 0 : 1); */
}

static void
show_options(struct aria_download *d)
{
	update_options(d, 1);
}

struct downloads_add_arg {
	size_t nlines;
	char **lines;
};

/* select (last) newly added download and write back errorneous files */
static void
on_downloads_add(struct json_node *result, struct downloads_add_arg *arg)
{
	FILE *f;
	size_t n;

	if (NULL == result) {
		if (NULL != arg) {
			while (arg->nlines > 0)
				free(arg->lines[--arg->nlines]);
			free(arg);
		}
		return;
	}

	if (json_isempty(result))
		goto out;

	/* write back failed lines */
	f = NULL != arg ? fopen(session_file, "w") : NULL;

	n = 0;
	result = json_children(result);
	do {
		struct aria_download **dd;

		if (json_obj == json_type(result)) {
			error_handler(result);

			if (NULL != f)
				fputs(arg->lines[n], f);
		} else {
			struct json_node *gid = json_children(result);

			if (json_str == json_type(gid)) {
				/* single GID returned */
				if (NULL != (dd = find_download_bygid(gid->val.str, 1)))
					selidx = dd - downloads;
			} else {
				/* get first GID of the array */
				if (NULL != (dd = find_download_bygid(json_children(gid)->val.str, 1)))
					selidx = dd - downloads;
			}
		}

		if (NULL != arg)
			free(arg->lines[n]);
		++n;
	} while (NULL != (result = json_next(result)));

	if (NULL != f)
		fclose(f);

	on_downloads_change(1);
	refresh();

out:
	if (NULL != arg) {
		free(arg->lines);
		free(arg);
	}
}

static void
add_downloads(char cmd)
{
	struct rpc_request *req;
	FILE *f;
	char *line;
	size_t linesiz;
	ssize_t err;
	size_t linelen;
	int action;
	struct downloads_add_arg *arg;

	if ((action = run_action(NULL, "%c", cmd)) < 0)
		action = fileout(1);

	if (EXIT_SUCCESS != action)
		return;

	clear_error_message();

	if (NULL == (f = fopen(session_file, "r"))) {
		set_error_message("fopen(%s, \"r\"): %s",
				session_file, strerror(errno));
		return;
	}

	req = new_rpc();
	if (NULL == req)
		goto out_fclose;

	(void)(arg = malloc(sizeof *arg));

	req->handler = (rpc_handler)on_downloads_add;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	arg->lines = NULL, arg->nlines = 0;
	line = NULL, linesiz = 0;
	while (-1 != (err = getline(&line, &linesiz, f))) {
		char *uri;
		char *b64str = NULL;
		enum { kind_torrent, kind_metalink, kind_uri } kind;

		if (NULL != arg) {
			char **newlines;

			if (NULL != (newlines = realloc(arg->lines, ++arg->nlines * sizeof *arg->lines))) {
				(arg->lines = newlines)[arg->nlines - 1] = strdup(line);
			} else {
				free(arg->lines);
				free(arg);
				arg = NULL;
			}

		}

		linelen = (size_t)err;
		uri = stripwhite(line, &linelen);

#define ISSUFFIX(lit) \
	((size_t)linelen >= sizeof lit && \
		 0 == memcmp(uri + (size_t)linelen - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (0 == linelen)
			continue;

		if (ISSUFFIX(".torrent"))
			kind = kind_torrent;
		else if ((ISSUFFIX(".meta4") || ISSUFFIX(".metalink")))
			kind = kind_metalink;
		else
			kind = kind_uri;

		if (kind != kind_uri && !(b64str = file_b64_enc(uri)))
			kind = kind_uri;
#undef ISSUFFIX

		json_write_beginobj(jw);

		json_write_key(jw, "methodName");
		switch (kind) {
		case kind_torrent:
			json_write_str(jw, "aria2.addTorrent");
			break;

		case kind_metalink:
			json_write_str(jw, "aria2.addMetalink");
			break;

		case kind_uri:
			json_write_str(jw, "aria2.addUri");
			break;
		}

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, secret_token);
		/* “data” */
		switch (kind) {
		case kind_torrent:
		case kind_metalink:
			json_write_str(jw, b64str);
			free(b64str);
			break;

		case kind_uri:
			json_write_beginarr(jw);
			json_write_str(jw, uri);
			json_write_endarr(jw);
			break;
		}
		if (kind_torrent == kind) {
			/* “uris” */
			json_write_beginarr(jw);
			/* (none) */
			json_write_endarr(jw);
		}
		/* “options” */
		json_write_beginobj(jw);
		/* (none) */
		json_write_endobj(jw);
		/* “position” */
		/* insert position at specified queue index */
		/* do not care if it’s bigger, it will be added to the end */
		json_write_num(jw, selidx);

		json_write_endarr(jw);

		json_write_endobj(jw);
	}

	free(line);

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	req->arg = arg;

	do_rpc();

out_fclose:
	fclose(f);
}

static void
update_all(void)
{
	unsigned n;
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	req->handler = on_update_all;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	for (n = 0;;++n) {
		char *tell;
		int pageable;
		switch (n) {
		case 0:
			tell = "aria2.tellWaiting";
			pageable = 1;
			break;

		case 1:
			tell = "aria2.tellActive";
			pageable = 0;
			break;

		case 2:
			tell = "aria2.tellStopped";
			pageable = 1;
			break;

		default:
			goto out_of_loop;
		}

		json_write_beginobj(jw);

		json_write_key(jw, "methodName");
		json_write_str(jw, tell);

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, secret_token);

		if (pageable) {
			/* “offset” */
			json_write_int(jw, 0);

			/* “num” */
			json_write_int(jw, 9999);
		}

		/* “keys” {{{ */
		json_write_beginarr(jw);
		json_write_str(jw, "gid");
		json_write_str(jw, "status");
		json_write_str(jw, "totalLength");
		json_write_str(jw, "completedLength");
		json_write_str(jw, "uploadLength");
		json_write_str(jw, "verifiedLength");
		json_write_str(jw, "verifyIntegrityPending");
		json_write_str(jw, "errorMessage");
		json_write_str(jw, "uploadSpeed");
		json_write_str(jw, "downloadSpeed");
		/* json_write_str(jw, "seeder"); */
		json_write_str(jw, "bittorrent");
		json_write_str(jw, "numPieces");
		json_write_str(jw, "pieceLength");
		if (is_local || select_file)
			json_write_str(jw, "files");
		json_write_endarr(jw);
		/* }}} */
		json_write_endarr(jw);

		json_write_endobj(jw);
	}
out_of_loop:

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	do_rpc();
}

static void
write_file(void)
{
	FILE *f;
	size_t i;
	struct aria_download *d;

	if (num_downloads == 0)
		return;

	if (NULL == (f = fopen(session_file, "w")))
		return;

	d = downloads[selidx];

	/* BitTorren dir/name; it may be a directory for a multi or a file for
	 * a single-file Torrent */
	if (NULL != d->dir && NULL != d->name)
		fprintf(f, "file\t%s/%s\n", d->dir, d->name);

	if (NULL != d->files) {
		for (i = 0; i < d->num_files; ++i)
			fprintf(f, "file\t%s\n", d->files[i].path);
	}

	fclose(f);
}

static void
clear_file(void)
{
	truncate(session_file, 0);
}

static void
on_download_remove(struct json_node *result, struct aria_download *d)
{
	if (NULL != result) {
		struct aria_download **dd;

		write_file();
		(void)run_action(d, "D");
		clear_file();

		/* download removed */
		if (NULL != (dd = find_download(d))) {
			remove_download_at(dd);

			on_downloads_change(1);
			refresh();
		}
	}

	unref_download(d);
}

static void
aria_download_remove(struct aria_download *d, int force)
{
	struct rpc_request *req = new_rpc();
	if (NULL == req)
		return;

	req->handler = (rpc_handler)on_download_remove;
	req->arg = ref_download(d);

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceRemove" : "aria2.remove");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	do_rpc();
}

struct remove_download_arg {
	struct aria_download *d;
	int force;
};

static void
on_remove_download(struct json_node *result, struct remove_download_arg *arg)
{
	if (NULL != result) {
		parse_download_files(arg->d, result);
		aria_download_remove(arg->d, arg->force);
	}

	unref_download(arg->d);
	free(arg);
}

static void
remove_download(struct aria_download *d, int force)
{
	if (0 < d->num_files) {
		aria_download_remove(d, force);
	} else {
		/* fill download with files so action will get it */
		struct rpc_request *req = new_rpc();
		struct remove_download_arg *arg;

		if (NULL == req)
			return;

		if (NULL == (arg = malloc(sizeof *arg))) {
			free_rpc(req);
			return;
		}

		arg->d = ref_download(d);
		arg->force = force;

		req->handler = (rpc_handler)on_remove_download;
		req->arg = arg;

		json_write_key(jw, "method");
		json_write_str(jw, "aria2.getFiles");

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, secret_token);
		/* “gid” */
		json_write_str(jw, d->gid);
		json_write_endarr(jw);

		do_rpc();
	}
}

static void
stdin_read(void)
{
	int ch;

	while (ERR != (ch = getch())) {
		switch (ch) {
		case 'j':
		case KEY_DOWN:
			++selidx;
			draw_cursor();
			refresh();
			break;

		case 'J':
		case 'K':
			if (num_downloads > 0)
				aria_download_repos(downloads[selidx], ch == 'J' ? 1 : -1, POS_CUR);
			break;

		case 'k':
		case KEY_UP:
			--selidx;
			draw_cursor();
			refresh();
			break;

		case 'N':
			if (num_downloads > 0) {
				struct aria_download *d = downloads[selidx]->following;
				if (NULL != d) {
					selidx = find_download_bygid(d->gid, 1) - downloads;
					draw_cursor();
					refresh();
				}
			}
			break;

		case 'v':
			if (!(view = strchr(VIEWS + 1, view)[1])) {
				view = VIEWS[1];
				oldselidx = -1; /* force redraw */
			}

			update_delta(1);
			draw_all();
			refresh();
			break;

		case 'V':
			if (!(view = strchr(VIEWS + 1, view)[-1])) {
				view = VIEWS[array_len(VIEWS) - 2];
				oldselidx = -1; /* force redraw */
			}

			update_delta(1);
			draw_all();
			refresh();
			break;

		case CONTROL('D'):
			selidx += getmainheight() / 2;
			draw_cursor();
			refresh();
			break;

		case CONTROL('U'):
			selidx -= getmainheight() / 2;
			draw_cursor();
			refresh();
			break;

		case KEY_NPAGE:
			selidx += getmainheight();
			draw_cursor();
			refresh();
			break;

		case KEY_PPAGE:
			selidx -= getmainheight();
			draw_cursor();
			refresh();
			break;

		case 'g':
		case KEY_HOME:
			selidx = 0;
			draw_cursor();
			refresh();
			break;

		case 'G':
		case KEY_END:
			selidx = INT_MAX;
			draw_cursor();
			refresh();
			break;

		case 's':
		case 'p':
			if (num_downloads > 0)
				(void)aria_download_pause(downloads[selidx], ch == 'p', do_forced);
			do_forced = 0;
			break;

		case 'S':
		case 'P':
			(void)aria_pause_all(ch == 'P', do_forced);
			do_forced = 0;
			break;

		case 'a':
		case 'A':
		case '+':
			add_downloads(ch);
			break;

		case 'i':
		case 'I':
			show_options(num_downloads > 0 && ch == 'i' ? downloads[selidx] : NULL);
			break;

		case 'D':
		case KEY_DC: /*delete*/
			if (num_downloads > 0) {
				struct aria_download *d = downloads[selidx];

				if (DOWNLOAD_ACTIVE != abs(d->status)) {
					remove_download(d, do_forced);
					do_forced = 0;
				} else {
					set_error_message("refusing to delete active download");
				}
			}
			break;

		case '!':
			do_forced = 1;
			break;

		case 'x':
			if (num_downloads > 0)
				aria_remove_result(downloads[selidx]);
			break;

		case 'X':
			aria_remove_result(NULL);
			break;

		case 'q':
			if (view != VIEWS[1]) {
				view = VIEWS[1];
				oldselidx = -1; /* force redraw */
				draw_all();
				refresh();
				continue;
			}
			/* FALLTHROUGH */
		case 'Z':
			exit(EXIT_SUCCESS);

		case CONTROL('M'):
			if (isatty(STDOUT_FILENO)) {
				goto defaction;
			} else {
				endwin();

				if (num_downloads > 0) {
					struct aria_download const *d = downloads[selidx];
					printf("%s\n", d->gid);
				}

				exit(EXIT_SUCCESS);
			}
			break;

		case 'Q':
			if (run_action(NULL, "Q") != EXIT_FAILURE)
				aria_shutdown(do_forced);
			do_forced = 0;
			break;

		case CONTROL('L'):
			/* try connect if not connected */
			if (!ws_isalive())
				try_connect();
			update_all();
			break;

		case CONTROL('C'):
			exit(EXIT_FAILURE);

		default:
		defaction:
			/* TODO: if num_files == 0 request it first */
			write_file();
			run_action(NULL, "%s", keyname(ch));
			clear_file();

			do_forced = 0;
			refresh();
			break;

		}

	}
}

static void
fill_pairs(void)
{
	switch (NULL == getenv("NO_COLOR") ? COLORS : 0) {
	/* https://upload.wikimedia.org/wikipedia/commons/1/15/Xterm_256color_chart.svg */
	default:
	case 256:
		init_pair(COLOR_DOWN,  34,  -1);
		init_pair(COLOR_UP,    33,  -1);
		init_pair(COLOR_CONN, 133,  -1);
		init_pair(COLOR_ERR,  197,  -1);
		init_pair(COLOR_EOF,  250,  -1);
		break;
	case 8:
		init_pair(COLOR_DOWN,   2,  -1);
		init_pair(COLOR_UP,     4,  -1);
		init_pair(COLOR_CONN,   5,  -1);
		init_pair(COLOR_ERR,    1,  -1);
		init_pair(COLOR_EOF,    6,  -1);
		break;
	case 0:
		/* https://no-color.org/ */
		init_pair(COLOR_DOWN,   0,  -1);
		init_pair(COLOR_UP,     0,  -1);
		init_pair(COLOR_CONN,   0,  -1);
		init_pair(COLOR_ERR,    0,  -1);
		init_pair(COLOR_EOF,    0,  -1);
		break;
	}
}

/* múlt héten már próbáltam emailben megérdeklődni, hogy lehet-e */

static void
_endwin(void)
{
	endwin();
}

static void
cleanup_session_file(void)
{
	if ('\0' != session_file[0]) {
		unlink(session_file);
		session_file[0] = '\0';
	}
}

static void
on_remote_info(struct json_node *result, void *arg)
{
	struct json_node *node;

	(void)arg;

	if (NULL == result)
		return;

	result = json_children(result);
	node = json_children(result);
	parse_session_info(node);

	result = json_next(result);
	node = json_children(result);
	parse_options(node, (parse_options_cb)parse_option, NULL);

	/* we have is_local correctly set, so we can start requesting downloads */

	update_all();
}

static void
remote_info(void)
{
	struct rpc_request *req;

	if (NULL == (req = new_rpc()))
		return;
	req->handler = on_remote_info;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	json_write_beginobj(jw);
	json_write_key(jw, "methodName");
	json_write_str(jw, "aria2.getSessionInfo");
	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	json_write_endarr(jw);
	json_write_endobj(jw);

	json_write_beginobj(jw);
	json_write_key(jw, "methodName");
	json_write_str(jw, "aria2.getGlobalOption");
	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	json_write_endarr(jw);
	json_write_endobj(jw);

	json_write_endarr(jw);
	json_write_endarr(jw);

	do_rpc();
}

void
on_ws_open(void)
{
	fds[1].fd = ws_fileno();

	clear_error_message();

	oldselidx = -1;

	is_local = 0;
	remote_info();
}

void
on_ws_close(void)
{
	if (0 == errno)
		exit(EXIT_SUCCESS);

	fds[1].fd = -1;

	clear_rpc_requests();

	memset(&rpc_requests, 0, sizeof rpc_requests);

	clear_downloads();
	free(downloads), downloads = NULL;

	cleanup_session_file();
}

int
main(int argc, char *argv[])
{
	sigset_t sigmask;
	int argi;

	/* Needed for ncurses UTF-8. */
	setlocale(LC_CTYPE, "");

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGWINCH);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGQUIT);

	/* block signals to being able to receive it via signalfd() */
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	signal(SIGPIPE, SIG_IGN);

	for (argi = 1; argi < argc; ) {
		char *arg = argv[argi++];
		if (0 == strcmp(arg, "--select-gid")) {
			if (argc <= argi)
				goto show_usage;

			select_gid = argv[argi++];
		} else if (0 == strcmp(arg, "--select-file")) {
			if (argc <= argi)
				goto show_usage;

			select_file = argv[argi++];
		} else {
		show_usage:
			return EXIT_FAILURE;
		}
	}

	if (load_config())
		return EXIT_FAILURE;

	atexit(_endwin);
	newterm(NULL, stderr, stdin);
	start_color();
	use_default_colors();
	fill_pairs();
	/* no input buffering */
	raw();
	/* no input echo */
	noecho();
	/* do not translate '\n's */
	nonl();
	/* make `getch()` non-blocking */
	nodelay(stdscr, TRUE);
	/* catch special keys */
	keypad(stdscr, TRUE);
	/* 8-bit inputs */
	meta(stdscr, TRUE);

	json_writer_init(jw);

	/* (void)sigfillset(&sigmask); */
	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;
	fds[1].fd = -1;
	fds[1].events = POLLIN;
#if __linux__
	(void)(fds[2].fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC));
	fds[2].events = POLLIN;
#endif

	draw_all();
	refresh();

	atexit(cleanup_session_file);
	try_connect();

	for (;;) {
		int const any_activity = globalstat.download_speed + globalstat.upload_speed > 0;
		int const timeout = ws_isalive() ? (any_activity ? 1250 : 2500) : 5000;

		switch (poll(fds, array_len(fds), timeout)) {
		case -1:
			perror("poll");
			return EXIT_FAILURE;

		case 0:
			update_delta(1);
			continue;
		}

		if (fds[0].revents & POLLIN)
			stdin_read();

		if (fds[1].revents & POLLIN)
			ws_read();

#ifdef __linux__
		if (fds[2].revents & POLLIN) {
			struct signalfd_siginfo ssi;

			while (sizeof ssi == read(fds[2].fd, &ssi, sizeof ssi)) {
				switch (ssi.ssi_signo) {
				case SIGWINCH:
					endwin();
					/* First refresh is for updating changed window size. */
					refresh();
					draw_all();
					refresh();
					break;

				case SIGINT:
				case SIGHUP:
				case SIGTERM:
				case SIGQUIT:
					exit(EXIT_SUCCESS);
				}

			}
		}
#else
		/* sigtimedwait() */
#endif
	}
}
