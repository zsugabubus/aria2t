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

#include "program.h"
#include "websocket.h"
#include "b64.h"
#include "jeezson/jeezson.h"
#include "fourmat/fourmat.h"

#include "keys.in"

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

/* str := "true" | "false" */
#define IS_TRUE(str) ('t' == str[0])

struct aria_peer {
	char peer_id[20];

	char ip[INET6_ADDRSTRLEN];
	in_port_t port;

	unsigned up_choked: 1;
	unsigned down_choked: 1;

	uint32_t pieces_have;
	struct timespec latest_change; /* latest time when pieces_have changed */

	uint32_t peer_download_speed;

	uint64_t download_speed;
	uint64_t upload_speed;
};

struct aria_server {
	char *current_uri;
	uint64_t download_speed;

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

	uint32_t num_connections;

	uint32_t num_peers;
	uint32_t num_pieces;
	uint32_t piece_size;

	uint64_t total;
	uint64_t have;
	uint64_t uploaded;
	uint64_t verified;

	uint64_t download_speed;
	uint64_t upload_speed;
	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

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

	uint64_t uploaded_total;
	uint64_t have_total;

	unsigned optimize_concurrent: 1;
	unsigned save_session: 1;
	uint32_t max_concurrenct;

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
	(void)p;
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

	free(peers);
}

static void
free_files_of(struct aria_download *d)
{
	uint32_t i;

	if (NULL == d->files)
		return;

	for (i = 0; i < d->num_files; ++i)
		free_file(&d->files[i]);

	free(d->files);
}

static void
free_download(struct aria_download *d)
{
	free_files_of(d);

	free_peers(d->peers, d->num_peers);

	free(d->dir);
	free(d->name);
	free(d->error_message);
	free(d->tags);
	free(d);
}

static struct aria_download *
ref_download(struct aria_download *d)
{
	++d->refcnt;
	return d;
}

static void
unref_download(struct aria_download *d)
{
	assert(d->refcnt >= 1);
	if (0 == --d->refcnt)
		free_download(d);
}

static void
clear_downloads(void)
{
	globalstat.have_total = 0;
	globalstat.uploaded_total = 0;

	while (num_downloads > 0)
		unref_download(downloads[--num_downloads]);
}

static void
default_handler(struct json_node *result, void *arg)
{
	(void)result, (void)arg;
	/* just do nothing */
}

static int
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

static struct aria_download *
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
	struct aria_download **dd;
	struct aria_download *d;
	int8_t newstatus;

	static int8_t const STATUS_MAP[] = {
		[K_notification_none              ] = DOWNLOAD_UNKNOWN,
		[K_notification_DownloadStart     ] = DOWNLOAD_ACTIVE,
		[K_notification_DownloadPause     ] = DOWNLOAD_PAUSED,
		[K_notification_DownloadStop      ] = DOWNLOAD_PAUSED,
		[K_notification_DownloadComplete  ] = DOWNLOAD_COMPLETE,
		[K_notification_DownloadError     ] = DOWNLOAD_ERROR,
		[K_notification_BtDownloadComplete] = DOWNLOAD_COMPLETE
	};

	if (NULL == (dd = find_download_bygid(gid, 1)))
		return;
	d = *dd;

	newstatus = STATUS_MAP[K_notification_parse(method + strlen("aria2.on"))];

	if (newstatus != DOWNLOAD_UNKNOWN && newstatus != d->status) {
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
		} while (NULL != (field = json_next(field)));

		if (0 == strcmp(u->uri, s.current_uri))
			s.current_uri = NULL;
		if (NULL != s.current_uri)
			s.current_uri = strdup(s.current_uri);

		uri_addserver(u, &s);
	} while (NULL != (node = json_next(node)));
}

static void
parse_peer_id(struct aria_peer *p, char *peer_id)
{
#define HEX2NR(ch) (ch <= '9' ? ch - '0' : ch - 'A' + 10)
	char *s = peer_id, *d = p->peer_id;

	for (; d != p->peer_id + sizeof p->peer_id; ++d) {
		if ('%' == *s) {
			*d = (HEX2NR(s[1]) << 4) | HEX2NR(s[2]);
			s += 3;
		} else {
			*d = *s;
			s += 1;
		}
	}
#undef HEX2NR
	assert('\0' == *s);
}

static void
parse_peer_bitfield(struct aria_peer *p, char *bitfield)
{
	static uint8_t const HEX_POPCOUNT[] = {
		0, 1, 1, 2, 1, 2, 2, 3,
		1, 2, 2, 3, 2, 3, 3, 4,
	};
	uint32_t pieces_have = 0;
	char *hex;

	for (hex = bitfield; '\0' != *hex; ++hex)
		pieces_have += HEX_POPCOUNT[*hex <= '9' ? *hex - '0' : *hex - 'a' + 10];

	p->pieces_have = pieces_have;
}

static void
parse_peer(struct aria_peer *p, struct json_node *node)
{
	struct json_node *field = json_children(node);
	do {
		switch (K_peer_parse(field->key)) {
		case K_peer_none:
			/* ignore */
			break;

		case K_peer_peerId:
			parse_peer_id(p, field->val.str);
			break;

		case K_peer_ip:
			strncpy(p->ip, field->val.str, sizeof p->ip - 1);
			break;

		case K_peer_port:
			p->port = strtoul(field->val.str, NULL, 10);
			break;

		case K_peer_bitfield:
			parse_peer_bitfield(p, field->val.str);
			break;

		case K_peer_amChoking:
			p->up_choked = IS_TRUE(field->val.str);
			break;

		case K_peer_peerChoking:
			p->down_choked = IS_TRUE(field->val.str);
			break;

		case K_peer_downloadSpeed:
			p->download_speed = strtoull(field->val.str, NULL, 10);
			break;

		case K_peer_uploadSpeed:
			p->upload_speed = strtoull(field->val.str, NULL, 10);
			break;
		}
	} while (NULL != (field = json_next(field)));
}

static void
parse_peers(struct aria_download *d, struct json_node *node)
{
	struct aria_peer *p;
	struct timespec now;
	uint32_t num_oldpeers;
	struct aria_peer *oldpeers;

	num_oldpeers = d->num_peers;
	oldpeers = d->peers;

	d->num_peers = json_len(node);
	if (NULL == (d->peers = malloc(d->num_peers * sizeof *(d->peers))))
		goto free_oldpeers;

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

	p = d->peers;
	node = json_children(node);
	do {
		struct aria_peer *oldp;
		uint32_t j;

		parse_peer(p, node);

		/* find new peer among previous ones to being able to compute
		 * its progress/speed change */
		for (j = 0; j < num_oldpeers; ++j) {
			oldp = &oldpeers[j];
			if (0 == memcmp(p->peer_id, oldp->peer_id, sizeof p->peer_id) &&
			    p->port == oldp->port &&
			    0 == strcmp(p->ip, oldp->ip))
				goto found_oldpeer;
		}
		/* a new peer */
		p->peer_download_speed = 0;
		p->latest_change = now;
		continue;

	found_oldpeer:
		/* compute peer speed */
		/* if peer has not reached 100% we assume it downloads continously */
		if (p->pieces_have < d->num_pieces) {
			uint64_t pieces_change;
			uint64_t bytes_change;
			uint64_t time_ns_change;
#define NS_PER_SEC UINT64_C(1000000000)

			pieces_change =
				p->pieces_have != oldp->pieces_have
					? p->pieces_have - oldp->pieces_have
					: 1;
			bytes_change = pieces_change * d->piece_size;
			time_ns_change =
				(now.tv_sec  - oldp->latest_change.tv_sec) * NS_PER_SEC +
				 now.tv_nsec - oldp->latest_change.tv_nsec + 1/*avoid /0*/;

			p->peer_download_speed = (bytes_change * NS_PER_SEC) / time_ns_change;

			/* if pieces_have changed since last time we could
			 * exactly compute the speed from the difference.
			 * otherwise we derive it from the theoretical maximum
			 * speed needed for transferring one piece
			 * (p->peer_download_speed) and previous speed. */
			if (p->pieces_have != oldp->pieces_have) {
				p->latest_change = now;
			} else {
				p->latest_change = oldp->latest_change;
				if (oldp->peer_download_speed < p->peer_download_speed)
					p->peer_download_speed = oldp->peer_download_speed;
			}

#undef NS_PER_SEC
		} else {
			p->peer_download_speed = 0;
			/* NOTE: we let latest_change uninitialized; we not need that anymore */
		}
	} while (++p, NULL != (node = json_next(node)));

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
	free_files_of(d);

	d->num_files = json_len(node);
	d->num_selfiles = 0;
	if (NULL == (d->files = malloc(d->num_files * sizeof *(d->files))))
		return;

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
			switch (K_files_parse(field->key)) {
			case K_files_none:
				/* ignore */
				break;

			case K_files_index:
				index = atoi(field->val.str) - 1;
				break;

			case K_files_path:
				file.path = strlen(field->val.str) > 0 ? strdup(field->val.str) : NULL;
				break;

			case K_files_length:
				file.total = strtoull(field->val.str, NULL, 10);
				break;

			case K_files_completedLength:
				file.have = strtoull(field->val.str, NULL, 10);
				break;

			case K_files_selected:
				file.selected = IS_TRUE(field->val.str);
				d->num_selfiles += file.selected;
				break;

			case K_files_uris: {
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
				break;
			}
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
		switch (K_download_parse(field->key)) {
		case K_download_gid:
			assert(strlen(field->val.str) == sizeof d->gid - 1);
			memcpy(d->gid, field->val.str, sizeof d->gid);
			break;

		case K_download_files:
			parse_download_files(d, field);
			break;

		case K_download_numPieces:
			d->num_pieces = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_pieceLength:
			d->piece_size = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_bittorrent: {
			struct json_node *bt_info, *bt_name;

			free(d->name);
			d->name = NULL;

			if (NULL != (bt_info = json_get(field, "info")) &&
			    NULL != (bt_name = json_get(bt_info, "name")))
				d->name = strdup(bt_name->val.str);
		}
			break;

		case K_download_status: {
			static int8_t const STATUS_MAP[] = {
				[K_status_none    ] = DOWNLOAD_UNKNOWN,
				[K_status_active  ] = DOWNLOAD_ACTIVE,
				[K_status_waiting ] = DOWNLOAD_WAITING,
				[K_status_paused  ] = DOWNLOAD_PAUSED,
				[K_status_error   ] = DOWNLOAD_ERROR,
				[K_status_complete] = DOWNLOAD_COMPLETE,
				[K_status_removed ] = DOWNLOAD_REMOVED
			};
			int8_t const newstatus = STATUS_MAP[K_status_parse(field->val.str)];
			if (newstatus != d->status)
				d->status = -newstatus;
		}
			break;

		case K_download_completedLength:
			globalstat.have_total -= d->have;
			d->have = strtoull(field->val.str, NULL, 10);
			globalstat.have_total += d->have;
			break;

		case K_download_uploadLength:
			globalstat.uploaded_total -= d->uploaded;
			d->uploaded = strtoull(field->val.str, NULL, 10);
			globalstat.uploaded_total += d->uploaded;
			break;

		case K_download_following: {
			struct aria_download **dd = find_download_bygid(field->val.str, 0);
			d->following = NULL != dd ? *dd : NULL;
			break;
		}

		case K_download_belongsTo: {
			struct aria_download **dd = find_download_bygid(field->val.str, 0);
			d->belongs_to = NULL != dd ? *dd : NULL;
			break;
		}

		case K_download_errorMessage:
			free(d->error_message);
			d->error_message = strdup(field->val.str);
			break;

		case K_download_downloadSpeed:
			d->download_speed = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_uploadSpeed:
			d->upload_speed = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_totalLength:
			d->total = strtoull(field->val.str, NULL, 10);
			break;

		case K_download_connections:
			d->num_connections = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_verifiedLength:
			d->verified = strtoul(field->val.str, NULL, 10);
			break;

		case K_download_verifyIntegrityPending:
			d->verified = UINT64_MAX;
			break;

		case K_download_none:
			/* ignore */
			break;
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

	node = json_children(node);
	node = json_children(node);
	do {
		switch (K_global_parse(node->key)) {
		case K_global_none:
			/* ignore */
			break;

		case K_global_downloadSpeed:
			globalstat.download_speed = strtoull(node->val.str, NULL, 10);
			break;

		case K_global_uploadSpeed:
			globalstat.upload_speed = strtoull(node->val.str, NULL, 10);
			break;

		case K_global_numActive:
			globalstat.num_active = strtoul(node->val.str, NULL, 10);
			break;

		case K_global_numWaiting:
			globalstat.num_waiting = strtoul(node->val.str, NULL, 10);
			break;

		case K_global_numStopped:
			globalstat.num_stopped = strtoul(node->val.str, NULL, 10);
			break;

		case K_global_numStoppedTotal:
			globalstat.num_stopped_total = strtoul(node->val.str, NULL, 10);
			break;
		}
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
		switch (K_option_parse(option)) {
		default:
			/* ignore */
			break;

		case K_option_max__download__limit:
			d->download_speed_limit = atol(value);
			break;

		case K_option_max__upload__limit:
			d->upload_speed_limit = atol(value);
			break;

		case K_option_dir:
			free(d->dir), d->dir = strdup(value);
			update_tags(d);
			break;
		}
	} else {
		switch (K_option_parse(option)) {
		default:
			/* ignore */
			break;

		case K_option_max__concurrent__downloads:
			globalstat.max_concurrenct = atol(value);
			break;

		case K_option_save__session:
			globalstat.save_session = IS_TRUE(value);
			break;

		case K_option_optimize__concurrent__downloads:
			globalstat.optimize_concurrent = IS_TRUE(value);
			break;

		case K_option_max__overall__download__limit:
			globalstat.download_speed_limit = atol(value);
			break;

		case K_option_max__overall__upload__limit:
			globalstat.upload_speed_limit = atol(value);
			break;

		case K_option_dir:
			is_local = 0 == access(value, R_OK | W_OK | X_OK);
			break;
		}
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
on_periodic(struct json_node *result, struct periodic_arg *arg)
{
	if (NULL == result)
		goto free_arg;

	result = json_children(result);
	parse_global_stat(result);

	result = json_next(result);
	if (NULL != result) {
		parse_downloads(result, arg);
		on_downloads_change(1);
	} else {
		draw_statusline();
	}

	refresh();

free_arg:
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
				json_write_str(jw, "bittorrent");
				json_write_str(jw, "numPieces");
				json_write_str(jw, "pieceLength");
				d->requested_bittorrent = 1;
			} else if (0 == d->num_files && ((NULL == d->name && d->status >= 0) || is_local)) {
				/* if “bittorrent.info.name” is empty then
				 * assign the name of the first file as name */
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

				if (d->have != d->total &&
				    (globalstat.download_speed > 0 ||
				    d->download_speed > 0)) {
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
			if ((DOWNLOAD_ACTIVE == abs(d->status) || d->status < 0) && (NULL != d->name
			    ? (arg->has_get_peers = ('p' == view))
			    : (arg->has_get_servers = ('f' == view)))) {
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

	globalstat.have_total -= d->have;
	globalstat.uploaded_total -= d->uploaded;

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
	int x, w = getmaxx(stdscr);

	attr_set(A_NORMAL, 0, NULL);
	mvprintw(*y, 0, "  %s:%-5u", p->ip, p->port);
	clrtoeol();

	x = 2 + sizeof p->ip + 1 + 5;
	move(*y, x + 25 <= w ? x : w - 25);

	addstr(" ");
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
			char fmtbuf[5];
			int n;

			mvprintw((*y)++, 0, "      %s   ↓  ",
					j + 1 < f->num_uris ? "│" : " ");

			attr_set(A_BOLD, COLOR_DOWN, NULL);
			n = fmt_speed(fmtbuf, s->download_speed);
			addnstr(fmtbuf, n);
			attr_set(A_NORMAL, 0, NULL);

			if (NULL != s->current_uri)
				printw(" ↪ %s", s->current_uri);

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
	mvaddnstr(*y, 0, d->gid, 6);
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

			addstr("        ");
		} else {
			addstr(" ");
			n = fmt_space(fmtbuf, d->verified);
			addnstr(fmtbuf, n);

			addstr("[");
			n = fmt_percent(fmtbuf, d->verified, d->total);
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

static struct {
	char const *tsl;
	char const *fsl;
} ti;

static void
update_terminfo(void)
{
	if ((char *)-1 == (ti.tsl = tigetstr("tsl")))
		ti.tsl = NULL;
	if ((char *)-1 == (ti.fsl = tigetstr("fsl")))
		ti.fsl = NULL;
}

static void
update_title(void)
{
	char sdown[5], sup[5];
	int ndown, nup;

	if (NULL == ti.tsl || NULL == ti.fsl)
		return;

	ndown = fmt_speed(sdown, globalstat.download_speed);
	nup = fmt_speed(sup, globalstat.upload_speed);
	dprintf(STDERR_FILENO,
			"%s"
			"↓ %.*s ↑ %.*s @ %s:%u – aria2t"
			"%s",
			ti.tsl,
			ndown, sdown,
			nup, sup,
			remote_host, remote_port,
			ti.fsl);
}

static void
draw_statusline(void)
{
	int y, x, w;
	char fmtbuf[5];
	int n;

	update_title();

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
			globalstat.optimize_concurrent ? "O" : "",
			globalstat.max_concurrenct,
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
	n = fmt_percent(fmtbuf, globalstat.uploaded_total, globalstat.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);
	mvaddstr(y, x -= 1, "[");

	n = fmt_speed(fmtbuf, globalstat.uploaded_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

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
	parse_global_stat(result);

	result = json_next(result);
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
run_action(struct aria_download *d, char const *name, ...)
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

	if (0 == (pid = vfork())) {
		execlp(filepath, filepath,
				NULL != d ? d->gid : "",
				session_file,
				NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, &status, 0) && errno == EINTR)
		;

	/* become foreground process */
	tcsetpgrp(STDERR_FILENO, getpgrp());
	refresh();

	if (WIFEXITED(status) && 127 == WEXITSTATUS(status)) {
		return -1;
	} else if (WIFEXITED(status) && EXIT_SUCCESS == WEXITSTATUS(status)) {
		runaction_maychanged(d);
		refresh();
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
	fprintf(file, "%s=%s\n", option, value);
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
	ssize_t len;
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
	while (-1 != (len = getline(&line, &linesiz, f))) {
		char *name, *value;

		name = line;
		if (NULL == (value = strchr(name, '=')))
			continue;
		*value++ = '\0';

		if (line[len - 1] == '\n')
			line[len - 1] = '\0';

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

/* select (last) newly added download and write back errorneous files */
static void
on_downloads_add(struct json_node *result, void *arg)
{
	size_t n;

	(void)arg;

	if (json_isempty(result))
		return;

	n = 0;
	result = json_children(result);
	do {
		struct aria_download **dd;

		if (json_obj == json_type(result)) {
			error_handler(result);
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
		++n;
	} while (NULL != (result = json_next(result)));

	on_downloads_change(1);
	refresh();
}

static void
add_downloads(char cmd)
{
	struct rpc_request *req;
	FILE *f;
	char *line;
	size_t linesiz;
	ssize_t len;
	int action;

	if ((action = run_action(NULL, "%c", cmd)) < 0)
		action = fileout(1);

	if (EXIT_SUCCESS != action)
		return;

	clear_error_message();

	if (NULL == (f = fopen(session_file, "r"))) {
		set_error_message("fopen(\"%s\", \"r\"): %s",
				session_file, strerror(errno));
		return;
	}

	req = new_rpc();
	if (NULL == req)
		goto out_fclose;

	req->handler = (rpc_handler)on_downloads_add;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	line = NULL, linesiz = 0;
next:
	for (len = getline(&line, &linesiz, f); -1 != len;) {
		char *uri, *next_uri;
		char *b64str = NULL;
		enum { kind_uri, kind_torrent, kind_metalink } kind;

		if (0 < len && line[len - 1] == '\n')
			line[--len] = '\0';

		if (0 == len)
			goto next;

		uri = line;
		if (NULL != (next_uri = strchr(uri, '\t')))
			*next_uri++ = '\0';

#define ISSUFFIX(lit) \
	((size_t)len > ((sizeof lit) - 1) && \
		 0 == memcmp(uri + (size_t)len - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (ISSUFFIX(".torrent"))
			kind = kind_torrent;
		else if (ISSUFFIX(".meta4") || ISSUFFIX(".metalink"))
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
			for (;;) {
				json_write_str(jw, uri);

				if (NULL == (uri = next_uri))
					break;

				if (NULL != (next_uri = strchr(uri, '\t')))
					*next_uri++ = '\0';
			}
			json_write_endarr(jw);
			break;
		}
		if (kind_torrent == kind) {
			/* “uris” */
			json_write_beginarr(jw);
			while (NULL != next_uri) {
				uri = next_uri;
				if (NULL != (next_uri = strchr(uri, '\t')))
					*next_uri++ = '\0';

				json_write_str(jw, uri);
			}
			json_write_endarr(jw);
		}
		/* “options” */
		json_write_beginobj(jw);
		while (-1 != (len = getline(&line, &linesiz, f))) {
			char *name, *value;

			if (0 == len)
				continue;

			if (line[len - 1] == '\n')
				line[--len] = '\0';

			for (name = line; isspace(*name); )
				++name;

			/* no leading spaces */
			if (name == line)
				break;

			if (NULL == (value = strchr(name, '=')))
				break;
			*value++ = '\0';

			json_write_key(jw, name);
			json_write_str(jw, value);
		}

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

	{
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
on_download_remove(struct json_node *result, struct aria_download *d)
{
	if (NULL != result) {
		struct aria_download **dd;

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

static void
remove_download(struct aria_download *d, int force)
{
	if (run_action(d, "D") < 0) {
		if (DOWNLOAD_ACTIVE != abs(d->status))
			aria_download_remove(d, force);
		else
			set_error_message("refusing to delete active download");
	}
}

static void
switch_view(int next)
{
	if (!(view = strchr(VIEWS + 1, view)[next ? 1 : -1])) {
		view = VIEWS[next ? 1 : array_len(VIEWS) - 2];
		oldselidx = -1; /* force redraw */
	}

	update_delta(1);
	draw_all();
	refresh();
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
		case KEY_RIGHT:
			switch_view(1);
			break;

		case 'V':
		case KEY_LEFT:
			switch_view(0);
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
				remove_download(downloads[selidx], do_forced);
				do_forced = 0;
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
			if (EXIT_FAILURE != run_action(NULL, "Q"))
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
			run_action(NULL, "%s", keyname(ch));

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

	setlocale(LC_ALL, "");

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

	update_terminfo();

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
					/* first refresh is for updating changed window size */
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
