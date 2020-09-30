#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "program.h"
#include "websocket.h"
#include "b64.h"
#include "jeezson/jeezson.h"
#include "fourmat/fourmat.h"

#include "keys.in"

#define XATTR_NAME "user.tags"

#define addspaces(n) addnstr("                                          "/*31+5+6=42*/, n);

#define COLOR_DOWN 1
#define COLOR_UP   2
#define COLOR_CONN 3
#define COLOR_ERR  4
#define COLOR_EOF  5

#define CONTROL(letter) ((letter) - '@')
#define array_len(arr) (sizeof arr / sizeof *arr)

#define VIEWS "\0dfp"

#define DOWNLOAD_SYMBOLS  "?w>|.-*"
/* NOTE: order is relevant for sorting */
#define DOWNLOAD_UNKNOWN  0
#define DOWNLOAD_WAITING  1
#define DOWNLOAD_ACTIVE   2
#define DOWNLOAD_PAUSED   3
#define DOWNLOAD_COMPLETE 4
#define DOWNLOAD_REMOVED  5
#define DOWNLOAD_ERROR    6
#define DOWNLOAD_COUNT    7

/* str := "true" | "false" */
#define IS_TRUE(str) ('t' == str[0])

enum pos_how { POS_SET, POS_CUR, POS_END };

struct peer {
	uint8_t peer_id[20];

	char ip[INET6_ADDRSTRLEN];
	in_port_t port;

	bool up_choked: 1;
	bool down_choked: 1;

	uint32_t pieces_have;
	struct timespec latest_change; /* latest time when pieces_have changed */

	uint64_t peer_download_speed;

	uint64_t download_speed; /* from peer */
	uint64_t upload_speed; /* to peer */
};

struct server {
	char *current_uri;
	uint64_t download_speed;
};

enum uri_status {
	uri_status_waiting,
	uri_status_used
};

struct uri {
	enum uri_status status;
	uint32_t num_servers;
	char *uri;
	struct server *servers;
};

struct file {
	char *path;
	uint64_t total;
	uint64_t have;
	uint32_t num_uris;
	bool selected;
	struct uri *uris;
};

static char const *const NONAME = "?";

struct download {
	char *name;
	char const *display_name;
	char gid[16 + 1];

	uint8_t refcnt; /* dynamic weak refs */
	bool deleted: 1; /* one strong ref: is on the list or not */
	bool initialized: 1;
	bool requested_options: 1;
	int8_t status; /* DOWNLOAD_*; or negative if changed. */
	uint16_t queue_index;

	char *error_message;

	uint32_t num_files;
	uint32_t num_selfiles;

	uint32_t num_connections;

	uint32_t num_peers;
	uint32_t num_pieces;
	uint32_t piece_size;

	uint32_t seed_ratio;

	uint64_t total;
	uint64_t have;
	uint64_t uploaded;
	uint64_t verified;

	uint64_t download_speed;
	uint64_t upload_speed;
	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

	char *dir;

	bool follows; /* follows or belongsTo |parent| */
	struct peer *peers;
	struct file *files;

	struct download *parent;
	struct download *first_child;
	struct download *next_sibling;

	char *tags;
};

static struct download **downloads;
static size_t num_downloads;

static struct {
	uint64_t download_speed;
	uint64_t upload_speed;

	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

	/* computed locally */
	uint64_t download_speed_total;
	uint64_t upload_speed_total;
	uint64_t have_total;
	uint64_t uploaded_total;

	/* number of downloads that have download-speed-limit set */
	uint64_t num_download_limited;
	/* number of downloads that have upload-speed-limit or seed-ratio set */
	uint64_t num_upload_limited;

	/* number of downloads per statuses */
	uint32_t num_perstatus[DOWNLOAD_COUNT];
} global;

static struct {
	enum {
		act_visual = 0,
		act_add_downloads,
		act_shutdown,
		act_print_gid,
		act_pause,
		act_unpause,
		act_purge
	} kind;
	bool uses_files;
	size_t num_sel;
	char **sel;
} action;

static struct {
	char const *tsl;
	char const *fsl;
} ti;

static char const* remote_host;
static in_port_t remote_port;
static char *secret_token;

static char session_file[PATH_MAX];
static bool is_local; /* server runs on local host */

static struct timespec period;
static void(*periodic)(void);

typedef void(*rpc_handler)(struct json_node const *result, void *arg);
static struct rpc_request {
	rpc_handler handler;
	void *arg;
} rpc_requests[10];

static char view = VIEWS[1];

static int curx = 0;
static int downloads_need_reflow = 0;
static int tag_col_width = 0;
static struct download const *longest_tag;
static int do_forced = 0;
static int oldselidx, selidx;
static int oldtopidx, topidx;

static struct json_writer jw[1];

static void draw_statusline(void);
static void draw_main(void);
static void draw_files(void);
static void draw_peers(void);
static void draw_downloads(void);
static void draw_download(struct download const *d, bool draw_parents, int *y);

static void
free_peer(struct peer *p)
{
	(void)p;
}

static void
free_server(struct server *s)
{
	free(s->current_uri);
}

static void
free_uri(struct uri *u)
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
free_file(struct file *f)
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
free_peers(struct peer *peers, uint32_t num_peers)
{
	uint32_t i;

	if (NULL == peers)
		return;

	for (i = 0; i < num_peers; ++i)
		free_peer(&peers[i]);

	free(peers);
}

static void
free_files_of(struct download *d)
{
	uint32_t i;

	if (NULL == d->files)
		return;

	for (i = 0; i < d->num_files; ++i)
		free_file(&d->files[i]);

	free(d->files);
}

static void
unref_download(struct download *d);

static struct download const *
download_prev_sibling(struct download const *d)
{
	struct download const *sibling;

	/* no family at all */
	if (NULL == d->parent)
		return NULL;

	/* first child */
	sibling = d->parent->first_child;
	if (d == sibling)
		return NULL;

	while (sibling->next_sibling != d)
		sibling = sibling->next_sibling;

	return sibling;
}

static void
free_download(struct download *d)
{
	free_files_of(d);
	free_peers(d->peers, d->num_peers);
	free(d->dir);
	free(d->name);
	free(d->error_message);
	free(d->tags);
	free(d);
}

static struct download *
ref_download(struct download *d)
{
	if (NULL != d)
		++d->refcnt;
	return d;
}

static void
unref_download(struct download *d)
{
	if (NULL != d) {
		assert(d->refcnt >= 1);
		if (0 == --d->refcnt)
			free_download(d);
	}
}

static void
delete_download_at(struct download **dd)
{
	struct download *d = *dd;
	struct download *sibling, *next;

	assert(!d->deleted);

	*dd = downloads[--num_downloads];

	if (longest_tag == d) {
		tag_col_width = 0;
		longest_tag = NULL;
	}

	for (sibling = d->first_child;
	     NULL != sibling;
	     sibling = next) {
		next = sibling->next_sibling;
		sibling->parent = NULL;
		sibling->next_sibling = NULL;
	}

	/* remove download from family tree */
	if (NULL != d->parent) {
		struct download *sibling = d->parent->first_child;
		if (d == sibling) {
			d->parent->first_child = d->next_sibling;
		} else {
			while (sibling->next_sibling != d)
				sibling = sibling->next_sibling;
			sibling->next_sibling = d->next_sibling;
		}
	}

	global.have_total -= d->have;
	global.uploaded_total -= d->uploaded;
	global.download_speed_total -= d->download_speed;
	global.upload_speed_total -= d->upload_speed;
	global.num_download_limited -= !!d->download_speed_limit;
	global.num_upload_limited -= d->seed_ratio || d->upload_speed_limit;
	global.num_perstatus[abs(d->status)] -= 1;

	/* mark download as not valid; will be freed when refcnt hits zero */
	d->deleted = true;

	unref_download(d);
}

static void
clear_downloads(void)
{
	while (0 < num_downloads)
		delete_download_at(&downloads[num_downloads - 1]);
}

static void
default_handler(struct json_node const *result, void *arg)
{
	(void)result, (void)arg;
	/* just do nothing */
}

static void
load_config(void)
{
	char *str;

	(remote_host = getenv("ARIA_RPC_HOST")) ||
	(remote_host = "127.0.0.1");

	(NULL != (str = getenv("ARIA_RPC_PORT")) &&
		(errno = 0,
		 remote_port = strtoul(str, NULL, 10),
		 0 == errno)) ||
	(remote_port = 6800);

	/* “token:$$secret$$” */
	(str = getenv("ARIA_RPC_SECRET")) ||
	(str = "");

	secret_token = malloc(snprintf(NULL, 0, "token:%s", str) + 1);
	if (NULL == secret_token) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}

	sprintf(secret_token, "token:%s", str);
}

static struct download **
new_download(void)
{
	struct download *d;
	struct download **dd;

	if (NULL == (dd = realloc(downloads, (num_downloads + 1) * sizeof *downloads)))
		return NULL;
	downloads = dd;

	if (NULL == (d = calloc(1, sizeof *d)))
		return NULL;
	*(dd = &downloads[num_downloads++]) = d;

	d->refcnt = 1;
	d->display_name = NONAME;
	global.num_perstatus[DOWNLOAD_UNKNOWN] += 1;

	return dd;
}

/* return whether |d| is still valid (on the list); |pdd| will be filled with
 * location of |d| if not NULL */
static bool
upgrade_download(struct download const *d, struct download ***pdd)
{
	if (d->deleted)
		return false;

	if (NULL != pdd) {
		struct download **dd = downloads;

		for (; *dd != d; ++dd)
			;

		*pdd = dd;
	}

	return true;
}

static bool
filter_download(struct download *d, struct download **dd);

static struct download **
get_download_bygid(char const *gid)
{
	struct download *d;
	struct download **dd = downloads;
	struct download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (0 == memcmp((*dd)->gid, gid, sizeof (*dd)->gid))
			return dd;

	if (NULL == (dd = new_download()))
		return NULL;
	d = *dd;

	assert(strlen(gid) == sizeof d->gid - 1);
	memcpy(d->gid, gid, sizeof d->gid);

	/* try applying selectors as soon as possible */
	return filter_download(d, dd) ? dd : NULL;
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
error_handler(struct json_node const *error)
{
	struct json_node const *message = json_get(error, "message");

	if (act_visual == action.kind) {
		set_error_message("%s", message->val.str);
		refresh();
	} else {
		fprintf(stderr, "%s\n", message->val.str);
		exit(EXIT_FAILURE);
	}
}

static void
free_rpc(struct rpc_request *rpc)
{
	rpc->handler = NULL;
}

static void
on_downloads_change(int stickycurs);

static void
update(void);

static bool
download_insufficient(struct download *d)
{
	return d->display_name == NONAME ||
		(-DOWNLOAD_ERROR == d->status && NULL == d->error_message) ||
		(is_local && 0 == d->num_files);
}

static void
notification_handler(char const *method, struct json_node const *event)
{
	char *const gid = json_get(event, "gid")->val.str;
	struct download **dd;
	struct download *d;
	int8_t newstatus;

	static int8_t const STATUS_MAP[] = {
		[K_notification_none              ] = DOWNLOAD_UNKNOWN,
		[K_notification_DownloadStart     ] = DOWNLOAD_ACTIVE,
		[K_notification_DownloadPause     ] = DOWNLOAD_PAUSED,
		[K_notification_DownloadStop      ] = DOWNLOAD_PAUSED,
		[K_notification_DownloadComplete  ] = DOWNLOAD_COMPLETE,
		[K_notification_DownloadError     ] = DOWNLOAD_ERROR,
		[K_notification_BtDownloadComplete] = DOWNLOAD_ACTIVE
	};

	if (NULL == (dd = get_download_bygid(gid)))
		return;
	d = *dd;

	newstatus = STATUS_MAP[K_notification_parse(method + strlen("aria2.on"))];

	if (newstatus != DOWNLOAD_UNKNOWN && newstatus != d->status) {
		global.num_perstatus[abs(d->status)] -= 1;
		d->status = -newstatus;
		global.num_perstatus[newstatus] += 1;

		on_downloads_change(1);
		refresh();

		/* in a perfect world we should update only this one download */
		if (download_insufficient(d))
			update();
	}
}

void
on_ws_message(char *msg, uint64_t msglen)
{
	static struct json_node *nodes;
	static size_t num_nodes;

	struct json_node const *id;
	struct json_node const *method;

	(void)msglen;

	json_parse(msg, &nodes, &num_nodes);

	if (NULL != (id = json_get(nodes, "id"))) {
		struct json_node const *const result = json_get(nodes, "result");
		struct rpc_request *const rpc = &rpc_requests[(unsigned)id->val.num];

		if (NULL == result)
			error_handler(json_get(nodes, "error"));

		/* NOTE: this condition shall always be true, but aria2c
		 * responses with some long messages with “Parse error.” that
		 * contains id=null, so we cannot get back the handler. the
		 * best we can do is to ignore and may leak a resource inside
		 * data. */
		if (NULL != rpc->handler) {
			rpc->handler(result, rpc->arg);
			free_rpc(rpc);
		}

	} else if (NULL != (method = json_get(nodes, "method"))) {
		struct json_node const *const params = json_get(nodes, "params");

		notification_handler(method->val.str, params + 1);
	} else {
		assert(0);
	}
}

static struct rpc_request *
new_rpc(void)
{
	uint8_t i;
	struct rpc_request *rpc;

	for (i = 0; i < array_len(rpc_requests); ++i) {
		rpc = &rpc_requests[i];

		if (NULL == rpc->handler)
			goto found;
	}

	set_error_message("too much pending messages");

	return NULL;

found:
	rpc->handler = default_handler;

	json_writer_empty(jw);
	json_write_beginobj(jw);

	json_write_key(jw, "jsonrpc");
	json_write_str(jw, "2.0");

	json_write_key(jw, "id");
	json_write_int(jw, (int)(rpc - rpc_requests));

	return rpc;
}


static void
clear_rpc_requests(void)
{
	uint8_t n = array_len(rpc_requests);

	while (0 < n) {
		struct rpc_request *rpc = &rpc_requests[--n];

		if (NULL != rpc->handler) {
			rpc->handler(NULL, rpc->arg);
			free_rpc(rpc);
		}
	}
}

static void
fetch_options(struct download *d, bool user);

static void
parse_session_info(struct json_node const *result)
{
	char *tmpdir;

	(tmpdir = getenv("TMPDIR")) ||
	(tmpdir = "/tmp");

	snprintf(session_file, sizeof session_file, "%s/aria2t.%s",
			tmpdir,
			json_get(result, "sessionId")->val.str);
}

static bool
do_rpc(struct rpc_request *rpc)
{
	json_write_endobj(jw);
	if (-1 == ws_write(jw->buf, jw->len)) {
		set_error_message("write: %s",
				strerror(errno));
		free_rpc(rpc);

		return false;
	} else {
		return true;
	}
}

static void
update_download_tags(struct download *d)
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

static struct uri *
find_file_uri(struct file const *f, char const *uri)
{
	uint32_t i;

	for (i = 0; i < f->num_uris; ++i) {
		struct uri *u = &f->uris[i];

		if (0 == strcmp(u->uri, uri))
			return u;
	}

	return NULL;
}

static void
uri_addserver(struct uri *u, struct server *s)
{
	void *p;

	p = realloc(u->servers, (u->num_servers + 1) * sizeof *(u->servers));
	if (NULL == p)
		return;
	(u->servers = p)[u->num_servers++] = *s;
}

static void
parse_file_servers(struct file *f, struct json_node const *node)
{
	if (json_isempty(node))
		return;

	node = json_children(node);
	do {
		struct json_node const *field = json_children(node);
		struct uri *u = u;
		struct server s;

		do {
			if (0 == strcmp(field->key, "uri")) {
				u = find_file_uri(f, field->val.str);
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
parse_peer_id(struct peer *p, char *peer_id)
{
#define HEX2NR(ch) (uint8_t)(ch <= '9' ? ch - '0' : ch - 'A' + 10)

	char *src = peer_id;
	uint8_t *dst = p->peer_id;

	for (; dst != p->peer_id + sizeof p->peer_id; ++dst) {
		if ('%' == *src) {
			*dst = (HEX2NR(src[1]) << 4) | HEX2NR(src[2]);
			src += 3;
		} else {
			*dst = *src;
			src += 1;
		}
	}

	assert('\0' == *src);

#undef HEX2NR
}

static void
parse_peer_bitfield(struct peer *p, char *bitfield)
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
parse_peer(struct peer *p, struct json_node const *node)
{
	struct json_node const *field = json_children(node);

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
parse_peers(struct download *d, struct json_node const *node)
{
	struct peer *p;
	struct timespec now;
	uint32_t num_oldpeers;
	struct peer *oldpeers;

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
		struct peer *oldp;
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
#define NS_PER_SEC UINT64_C(1000000000)

			uint64_t pieces_change;
			uint64_t bytes_change;
			uint64_t time_ns_change;

			pieces_change =
				p->pieces_have != oldp->pieces_have
					? p->pieces_have - oldp->pieces_have
					: 1;
			bytes_change = pieces_change * d->piece_size;
			time_ns_change =
				(now.tv_sec  - oldp->latest_change.tv_sec) * NS_PER_SEC +
				 now.tv_nsec - oldp->latest_change.tv_nsec + 1/* avoid /0 */;

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
parse_servers(struct download *d, struct json_node const *node)
{
	uint32_t i;

	if (NULL == d->files)
		return;

	/* reset servers for every uri */
	for (i = 0; i < d->num_files; ++i) {
		struct file *f = &d->files[i];
		uint32_t j;

		for (j = 0; j < f->num_uris; ++j) {
			struct uri *u = &f->uris[j];
			u->num_servers = 0;
		}
	}

	if (!json_isempty(node)) {
		node = json_children(node);
		do {
			struct json_node const *field = json_children(node);
			struct json_node const *servers = servers;
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
		struct file *f = &d->files[i];
		uint32_t j;

		for (j = 0; j < f->num_uris; ++j) {
			struct uri *u = &f->uris[j];
			if (0 == u->num_servers)
				free(u->servers), u->servers = NULL;
		}
	}

}

static void
update_display_name(struct download *d)
{
	if (NULL != d->name) {
		d->display_name = d->name;
		return;
	}

	if (d->num_files > 0 && NULL != d->files) {
		struct file *f = &d->files[0];

		if (NULL != f->path) {
			d->display_name = f->path;
			return;
		}

		if (f->num_uris > 0 && NULL != f->uris) {
			uint32_t i;

			for (i = 0; i < f->num_uris; ++i) {
				if (f->uris[i].status == uri_status_used) {
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
parse_download_files(struct download *d, struct json_node const *node)
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
		struct json_node const *field = json_children(node);
		struct file file;
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
				struct json_node const *uris;
				uint32_t uriidx = 0;

				if (json_isempty(field))
					continue;

				uris = json_children(field);
				file.num_uris = json_len(field);
				file.uris = malloc(file.num_uris * sizeof *(file.uris));

				do {
					struct json_node const *field = json_children(uris);
					struct uri u;

					u.num_servers = 0;
					u.servers = NULL;
					do {
						if (0 == strcmp(field->key, "status")) {
							if (0 == strcmp(field->val.str, "used"))
								u.status = uri_status_used;
							else if (0 == strcmp(field->val.str, "waiting"))
								u.status = uri_status_waiting;
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

	update_download_tags(d);
	update_display_name(d);
}

static bool
isgid(char const *str)
{
	uint8_t i;

	for (i = 0; i < 16; ++i) {
		char c = str[i];

		if (!(('0' <= c && c <= '9') ||
		      ('a' <= c && c <= 'f') ||
		      ('A' <= c && c <= 'F')))
			return false;

	}

	return '\0' == str[16];
}

/* decide whether download |d| (:= *|dd|) should be shown. if not, remove |d|
 * and return false; otherwise return true. */
static bool
filter_download(struct download *d, struct download **dd)
{
	size_t i;

	/* no --selects means all downloads */
	if (0 == action.num_sel)
		return true;

	for (i = 0; i < action.num_sel; ++i) {
		char const *const sel = action.sel[i];

		if (isgid(sel)) {
			/* test for matching GID */
			if (0 == memcmp(sel, d->gid, sizeof d->gid))
				return true;
		} else {
			/* test for matching path prefix */
			size_t j;
			size_t const sellen = strlen(sel);

			/* missing data can be anything */
			if (0 == d->num_files || NULL == d->files)
				return true;

			for (j = 0; j < d->num_files; ++j) {
				struct file const *f = &d->files[j];

				if (NULL != f->path && 0 == strncmp(f->path, sel, sellen))
					return true;
			}
		}
	}

	/* no selectors matched */
	if (NULL == dd)
		(void)upgrade_download(d, &dd);
	delete_download_at(dd);
	return false;
}

static void
download_changed(struct download *d)
{
	update_display_name(d);
	filter_download(d, NULL);
}

static void
parse_download(struct download *d, struct json_node const *node)
{
	struct json_node const *field = json_children(node);

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
			struct json_node const *bt_info, *bt_name;

			free(d->name), d->name = NULL;

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

			if (newstatus != d->status) {
				global.num_perstatus[abs(d->status)] -= 1;
				d->status = -newstatus;
				global.num_perstatus[newstatus] += 1;
			}
		}
			break;

		case K_download_completedLength:
			global.have_total -= d->have;
			d->have = strtoull(field->val.str, NULL, 10);
			global.have_total += d->have;
			break;

		case K_download_uploadLength:
			global.uploaded_total -= d->uploaded;
			d->uploaded = strtoull(field->val.str, NULL, 10);
			global.uploaded_total += d->uploaded;
			break;

		case K_download_following:
			/* XXX: We assume that a download cannot follow and
			 * belonging to a different download... */
			d->follows = true;
			/* FALLTHROUGH */
		case K_download_belongsTo: {
			struct download **dd = get_download_bygid(field->val.str);

			if (NULL != dd) {
				struct download *p = *dd;

				d->parent = p;
				if (NULL == p->first_child) {
					p->first_child = d;
				} else {
					p = p->first_child;
					while (NULL != p->next_sibling)
						p = p->next_sibling;
					p->next_sibling = d;
				}
			}
			break;
		}

		case K_download_errorMessage:
			free(d->error_message);
			d->error_message = strdup(field->val.str);
			break;

		case K_download_downloadSpeed:
			global.download_speed_total -= d->download_speed;
			d->download_speed = strtoul(field->val.str, NULL, 10);
			global.download_speed_total += d->download_speed;
			break;

		case K_download_uploadSpeed:
			global.upload_speed_total -= d->upload_speed;
			d->upload_speed = strtoul(field->val.str, NULL, 10);
			global.upload_speed_total += d->upload_speed;
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
}

typedef void(*parse_options_cb)(char const *, char const *, void *);

static void
parse_options(struct json_node const *node, parse_options_cb cb, void *arg)
{
	if (json_isempty(node))
		return;

	node = json_children(node);
	do
		cb(node->key, node->val.str, arg);
	while (NULL != (node = json_next(node)));
}

static void
parse_global_stat(struct json_node const *node)
{
	if (json_arr != json_type(node))
		return;

	node = json_children(node);
	node = json_children(node);
	do {
		if (0 == strcmp(node->key, "downloadSpeed"))
			global.download_speed = strtoull(node->val.str, NULL, 10);
		else if (0 == strcmp(node->key, "uploadSpeed"))
			global.upload_speed = strtoull(node->val.str, NULL, 10);
	} while (NULL != (node = json_next(node)));
}

static void
parse_option(char const *option, char const *value, struct download *d)
{
	if (NULL != d) {
		static locale_t cloc = (locale_t)0;
		locale_t origloc;

		if ((locale_t)0 == cloc)
			cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);

		switch (K_option_parse(option)) {
		default:
			/* ignore */
			break;

		case K_option_max__download__limit:
			global.num_download_limited -= !!d->download_speed_limit;
			d->download_speed_limit = atol(value);
			global.num_download_limited += !!d->download_speed_limit;
			break;

		case K_option_max__upload__limit:
			global.num_upload_limited -= d->seed_ratio || d->upload_speed_limit;
			d->upload_speed_limit = atol(value);
			global.num_upload_limited += d->seed_ratio || d->upload_speed_limit;
			break;

		case K_option_seed__ratio:
			origloc = uselocale(cloc);
			global.num_upload_limited -= d->seed_ratio || d->upload_speed_limit;
			d->seed_ratio = (uint32_t)(strtof(value, NULL) * 100);
			global.num_upload_limited += d->seed_ratio || d->upload_speed_limit;
			uselocale(origloc);
			break;

		case K_option_dir:
			free(d->dir), d->dir = strdup(value);
			update_download_tags(d);
			break;
		}
	} else {
		switch (K_option_parse(option)) {
		default:
			/* ignore */
			break;

		case K_option_max__overall__download__limit:
			global.download_speed_limit = atol(value);
			break;

		case K_option_max__overall__upload__limit:
			global.upload_speed_limit = atol(value);
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
	ws_open(remote_host, remote_port);
}

struct update_arg {
	struct download *download;
	bool has_get_peers: 1;
	bool has_get_servers: 1;
	bool has_get_options: 1;
};

static void
parse_downloads(struct json_node const *result, struct update_arg *arg)
{
	bool some_insufficient = false;
	struct download *d;

	for (; NULL != result;
	       result = json_next(result),
	       unref_download(d), ++arg) {
		struct json_node const *node;

		d = arg->download;

		if (!upgrade_download(d, NULL)) {
		skip:
			if (arg->has_get_peers || arg->has_get_servers)
				result = json_next(result);
			if (arg->has_get_options)
				result = json_next(result);
			continue;
		}

		if (json_obj == json_type(result)) {
			struct download **dd;

			upgrade_download(d, &dd);
			delete_download_at(dd);
			goto skip;
		}

		node = json_children(result);
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

		download_changed(d);
	}

	if (some_insufficient)
		update();
}

static void
set_periodic_update(void)
{
	bool const any_activity =
		0 < global.download_speed ||
		0 < global.upload_speed;

	periodic = update;
	period.tv_sec = any_activity ? 1 : 3;
	period.tv_nsec = 271 * 1000000;
}

static void
update_handler(struct json_node const *result, struct update_arg *arg)
{
	if (NULL == result)
		goto out;

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

out:
	set_periodic_update();

	free(arg);
}

static int
getmainheight(void)
{
	return getmaxy(stdscr)/*window height*/ - 1/*status line*/;
}

static void
update(void)
{
	struct rpc_request *rpc;

	/* request update only if it was scheduled */
	if (update != periodic)
		return;

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = (rpc_handler)update_handler;

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw); /* {{{ */
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

	if (0 < num_downloads) {
		struct download **top;
		struct download **bot;
		struct download **end = &downloads[num_downloads];
		struct download **dd = downloads;
		struct update_arg *arg;
		uint8_t topstatus, botstatus, status;
		size_t arglen = 0;

		if (view == VIEWS[1]) {
			top = &downloads[topidx];
			bot = top + getmainheight() - 1;
			if (end <= bot)
				bot = end - 1;
		} else {
			top = &downloads[selidx];
			bot = top;
		}

		topstatus = abs((*top)->status);
		botstatus = abs((*bot)->status);

		arglen = 0;
		status = topstatus;
		do
			arglen += global.num_perstatus[status];
		while (++status <= botstatus);

		if (NULL == (arg = malloc(arglen * sizeof *arg))) {
			free_rpc(rpc);
			return;
		}
		rpc->arg = arg;

		for (; dd < end; ++dd) {
			struct download *d = *dd;
			bool visible = top <= dd && dd <= bot;
			uint8_t keyidx = 0;
			char *keys[20];

			if (abs(d->status) < topstatus)
				continue;

			if (botstatus < abs(d->status))
				break;

#define WANT(key) keys[keyidx++] = key;

			if (!d->initialized) {
				WANT("bittorrent");
				WANT("numPieces");
				WANT("pieceLength");

				WANT("following");
				WANT("belongsTo");

				d->initialized = true;
			} else {
				if ((0 == d->num_files && (NULL == d->name || (visible && DOWNLOAD_ACTIVE != d->status && is_local))) ||
				    view == 'f')
					WANT("files");
			}

			if (d->status < 0) {
				WANT("totalLength");
				WANT("completedLength");
				WANT("downloadSpeed");
				WANT("uploadLength");
				WANT("uploadSpeed");

				if (-DOWNLOAD_ERROR == d->status)
					WANT("errorMessage");
			} else if (DOWNLOAD_UNKNOWN == d->status) {
					WANT("status");
			} else if (DOWNLOAD_ACTIVE == d->status) {
				if (0 < d->download_speed ||
				    (global.download_speed != global.download_speed_total &&
				     d->have != d->total))
					WANT("downloadSpeed");

				if (0 < d->download_speed)
					WANT("completedLength");

				if (0 < d->upload_speed ||
				    global.upload_speed != global.upload_speed_total)
					WANT("uploadSpeed");

				if (0 < d->upload_speed)
					WANT("uploadLength");

				if (visible) {
					if (0 == d->upload_speed && 0 == d->download_speed) {
						WANT("verifiedLength");
						WANT("verifyIntegrityPending");
					}

					WANT("connections");
				}
			}

#undef WANT

			if (0 == keyidx)
				continue;

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

			while (0 < keyidx)
				json_write_str(jw, keys[--keyidx]);

			json_write_endarr(jw);

			json_write_endarr(jw);
			json_write_endobj(jw);

			arg->download = ref_download(d);

			arg->has_get_peers = 0;
			arg->has_get_servers = 0;
			if (visible &&
			    (DOWNLOAD_ACTIVE == abs(d->status) || d->status < 0) &&
			    (NULL != d->name
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

			if ((arg->has_get_options = (d->status < 0 || (!d->requested_options && visible)))) {
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

				d->requested_options = true;
			}

			d->status = abs(d->status);
			++arg;
		}
	} else {
		rpc->arg = NULL;
	}

	json_write_endarr(jw); /* }}} */

	json_write_endarr(jw); /* }}} */

	if (do_rpc(rpc))
		periodic = NULL;
}

static void
change_download_position(struct download *d, int32_t pos, enum pos_how how)
{
	static char const *const POS_HOW[] = {
		"POS_SET",
		"POS_CUR",
		"POS_END"
	};

	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
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

	do_rpc(rpc);
}

static void
shutdown_aria(int force)
{
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceShutdown" : "aria2.shutdown");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	json_write_endarr(jw);

	do_rpc(rpc);
}

static void
action_exit(struct json_node const *result, void *arg)
{
	(void)result, (void)arg;
	exit(EXIT_SUCCESS);
}

static void
pause_download(struct download *d, bool pause, bool force)
{
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	if (act_visual != action.kind)
		rpc->handler = action_exit;

	if (0 == action.num_sel || NULL != d) {
		json_write_key(jw, "method");

		json_write_str(jw,
			NULL != d
			? (pause ? (force ? "aria2.forcePause" : "aria2.pause") : "aria2.unpause")
			: (pause ? (force ? "aria2.forcePauseAll" : "aria2.pauseAll") : "aria2.unpauseAll"));

		json_write_key(jw, "params");
		json_write_beginarr(jw);

		/* “secret” */
		json_write_str(jw, secret_token);
		if (NULL != d) {
			/* “gid” */
			json_write_str(jw, d->gid);
		}

		json_write_endarr(jw);
	} else {
		struct download **dd = downloads;
		struct download **const end = &downloads[num_downloads];

		json_write_key(jw, "method");
		json_write_str(jw, "system.multicall");

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* 1st arg: “methods” */
		json_write_beginarr(jw);

		for (; dd < end; ++dd) {
			json_write_beginobj(jw);
			json_write_key(jw, "methodName");
			json_write_str(jw, pause ? (force ? "aria2.forcePause" : "aria2.pause") : "aria2.unpause");
			json_write_key(jw, "params");
			json_write_beginarr(jw);

			/* “secret” */
			json_write_str(jw, secret_token);
			/* “gid” */
			json_write_str(jw, (*dd)->gid);

			json_write_endarr(jw);
			json_write_endobj(jw);
		}

		json_write_endarr(jw);
		json_write_endarr(jw);
	}

	do_rpc(rpc);
}

static void
purge_download_handler(struct json_node const *result, struct download *d)
{
	if (NULL != result) {
		struct download **dd;

		if (NULL != d) {
			if (upgrade_download(d, &dd))
				delete_download_at(dd);
		} else {
			struct download **end = &downloads[num_downloads];

			for (dd = downloads; dd < end;) {
				switch (abs((*dd)->status)) {
				case DOWNLOAD_COMPLETE:
				case DOWNLOAD_ERROR:
				case DOWNLOAD_REMOVED:
					delete_download_at(dd);
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

	unref_download(d);
}

static void
purge_download(struct download *d)
{
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = (rpc_handler)purge_download_handler;
	rpc->arg = ref_download(d);

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

	do_rpc(rpc);
}

static void
draw_main(void)
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
draw_peer(struct download const *d, size_t i, int *y)
{
	struct peer const *p = &d->peers[i];
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

	if (0 < p->peer_download_speed) {
		addstr(" @ ");
		n = fmt_speed(fmtbuf, p->peer_download_speed);
		addnstr(fmtbuf, n);
	} else {
		addspaces(3 + 4);
	}
	addspaces(2);

	if (p->down_choked) {
		addstr("----");
	} else if (0 < p->download_speed) {
		attr_set(A_BOLD, COLOR_DOWN, NULL);
		n = fmt_speed(fmtbuf, p->download_speed);
		addnstr(fmtbuf, n);
		attr_set(A_NORMAL, 0, NULL);
	} else {
		addspaces(4);
	}
	addstr(" ");

	if (p->up_choked) {
		addstr("----");
	} else if (0 < p->upload_speed) {
		attr_set(A_BOLD, COLOR_UP, NULL);
		n = fmt_speed(fmtbuf, p->upload_speed);
		addnstr(fmtbuf, n);
		attr_set(A_NORMAL, 0, NULL);
	} else {
		addspaces(4);
	}

	++*y;
}

static void
draw_peers(void)
{
	if (0 < num_downloads) {
		struct download *d = downloads[selidx];
		int y = 0;

		draw_download(d, false, &y);

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
draw_file(struct download const *d, size_t i, int *y)
{
	struct file const *f = &d->files[i];
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
		struct uri const *u = &f->uris[j];
		uint32_t k;

		attr_set(A_NORMAL, 0, NULL);
		mvprintw((*y)++, 0, "      %s╴",
		         j + 1 < f->num_uris ? "├" : "└");

		attr_set(u->status == uri_status_used ? A_BOLD : A_NORMAL, 0, NULL);
		printw("%3d%s ", j + 1, u->status == uri_status_used ? "*" : " ");
		attr_set(A_NORMAL, 0, NULL);
		addstr(u->uri);
		clrtoeol();

		for (k = 0; k < u->num_servers; ++k) {
			struct server const *s = &u->servers[k];
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
draw_files(void)
{
	if (0 < num_downloads) {
		struct download *d = downloads[selidx];
		int y = 0;

		draw_download(d, false, &y);

		if (0 < d->num_files) {
			size_t i;

			for (i = 0; i < d->num_files; ++i)
				draw_file(d, i, &y);

			/* linewrap */
			move(y, 0);
		} else {
			update();
		}

	} else {
		move(0, 0);
	}
	clrtobot();
	move(0, curx);
}

static int
downloadcmp(struct download const **pthis, struct download const **pother, void *arg);

/* XXX: Cache value? */
static struct download const *
downloadtreebest(struct download const *tree)
{
	struct download const *best = tree;
	struct download const *sibling;

	for (sibling = best->first_child;
	     NULL != sibling;
	     sibling = sibling->next_sibling) {
		struct download const *treebest = downloadtreebest(sibling);
		if (downloadcmp(&best, &treebest, (void *)1) > 0)
			best = treebest;
	}

	return best;
}

static int
downloadcmp(struct download const **pthis, struct download const **pother, void *arg)
{
	int cmp;
	uint64_t x, y;
	struct download const *this = *pthis;
	struct download const *other = *pother;
	int8_t this_status, other_status;

	if (NULL == arg) {
	/* if ((this_status == DOWNLOAD_PAUSED || other_status == DOWNLOAD_PAUSED) &&
	    (this_status == DOWNLOAD_COMPLETE || other_status == DOWNLOAD_COMPLETE))
		__asm__("int3"); */
		for (;; this = this->parent) {
			struct download const *p = other;

			do {
				if (p == this->parent)
					return 1;
				else if (this == p->parent)
					return -1;
				else if (this->parent == p->parent) {
					other = p;
					goto compare_siblings;
				}
			} while (NULL != (p = p->parent));
		}

	compare_siblings:
		this = downloadtreebest(this);
		other = downloadtreebest(other);
	}

	this_status = abs(this->status);
	other_status = abs(other->status);

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

	if (DOWNLOAD_ACTIVE == this_status) {
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
draw_download(struct download const *d, bool draw_parents, int *y)
{
	char fmtbuf[5];
	int n;
	int tagwidth;
	int namex, x;

	attr_set(A_NORMAL, 0, NULL);
	mvaddnstr(*y, 0, d->gid, 6);
	addstr(" ");

	attr_set(A_BOLD, -1, NULL);
	switch (abs(d->status)) {
	case DOWNLOAD_UNKNOWN:
		addstr("? ");
		goto fill_spaces;

	case DOWNLOAD_REMOVED:
		addstr("- ");

	fill_spaces:
		attr_set(A_NORMAL, 0, NULL);
		addspaces(29 +
			(0 < global.num_download_limited ? 5 : 0) +
			(0 < global.num_upload_limited ? 6 : 0));
		break;

	case DOWNLOAD_ERROR: {
		int const usable_width =
			29 +
			(NULL == d->tags ? tag_col_width : 0) +
			(0 < global.num_download_limited ? 5 : 0) +
			(0 < global.num_upload_limited ? 6 : 0);

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
			addspaces(4);
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
		if (0 < global.num_download_limited)
			addspaces(5);

		if (0 < d->num_files) {
			if (d->num_files != d->num_selfiles) {
				n = fmt_number(fmtbuf, d->num_selfiles);
				addnstr(fmtbuf, n);

				addstr("/");
			} else {
				addspaces(5);
			}

			n = fmt_number(fmtbuf, d->num_files);
			addnstr(fmtbuf, n);

			addstr(1 == d->num_files ? " file " : " files");
		} else {
			addspaces(5 + 4 + 6);
		}
		if (0 < global.num_upload_limited)
			addspaces(6);
		break;

	case DOWNLOAD_COMPLETE:
		attr_set(A_NORMAL, 0, NULL);

		addspaces(10);

		n = fmt_space(fmtbuf, d->total);
		addnstr(fmtbuf, n);

		addstr(",      ");

		if (0 < global.num_download_limited)
			addspaces(5);

		n = fmt_number(fmtbuf, d->num_selfiles);
		addnstr(fmtbuf, n);
		addstr(1 == d->num_selfiles ? " file " : " files");
		if (0 < global.num_upload_limited)
			addspaces(6);
		break;

	case DOWNLOAD_ACTIVE:
		addstr(d->verified == 0 ? "> " : "v ");
		attr_set(A_NORMAL, 0, NULL);

		if (0 == d->verified) {
			if (0 < d->download_speed) {
				attr_set(A_BOLD, COLOR_DOWN, NULL);
				n = fmt_percent(fmtbuf, d->have, d->total);
				addnstr(fmtbuf, n);

				attr_set(A_NORMAL, 0, NULL);
				addstr(" @ ");

				attr_set(A_BOLD, COLOR_DOWN, NULL);
				n = fmt_speed(fmtbuf, d->download_speed);
				addnstr(fmtbuf, n);

				if (0 < d->download_speed_limit) {
					attr_set(A_NORMAL, 0, NULL);
					addstr("/");
					n = fmt_speed(fmtbuf, d->download_speed_limit);
					addnstr(fmtbuf, n);
				} else if (0 < global.num_download_limited)
					addspaces(5);
				addstr(" ");
			} else {
				addstr(" ");
				n = fmt_space(fmtbuf, d->total);
				addnstr(fmtbuf, n);

				addstr("[");
				attr_set(A_NORMAL, COLOR_DOWN, NULL);
				n = fmt_percent(fmtbuf, d->have, d->total);
				addnstr(fmtbuf, n);
				attr_set(A_NORMAL, 0, NULL);
				addstr("] ");

				if (0 < global.num_download_limited)
					addspaces(5);
			}
		} else if (d->verified == UINT64_MAX) {
			addstr(" ");
			n = fmt_space(fmtbuf, d->have);
			addnstr(fmtbuf, n);

			addspaces(8 + (0 < global.num_download_limited ? 5 : 0));
		} else {
			addstr(" ");
			n = fmt_space(fmtbuf, d->verified);
			addnstr(fmtbuf, n);

			addstr("[");
			n = fmt_percent(fmtbuf, d->verified, d->total);
			addnstr(fmtbuf, n);
			addstr("] ");

			if (0 < global.num_download_limited)
				addspaces(5);
		}

		attr_set(A_NORMAL, COLOR_CONN, NULL);
		if (0 < d->num_connections) {
			n = fmt_number(fmtbuf, d->num_connections);
			addnstr(fmtbuf, n);
		} else {
			addspaces(4);
		}
		attr_set(A_NORMAL, 0, NULL);

		if (0 < d->uploaded || 0 < d->seed_ratio) {
			if (0 < d->upload_speed)
				attr_set(A_BOLD, COLOR_UP, NULL);
			addstr(" ");

			n = fmt_space(fmtbuf, d->uploaded);
			addnstr(fmtbuf, n);

			if (0 < d->upload_speed) {
				attr_set(A_NORMAL, 0, NULL);
				addstr(" @ ");

				attr_set(A_BOLD, COLOR_UP, NULL);
				n = fmt_speed(fmtbuf, d->upload_speed);
				addnstr(fmtbuf, n);

				if (0 < d->upload_speed_limit) {
					attr_set(A_NORMAL, 0, NULL);
					addstr("/");
					n = fmt_speed(fmtbuf, d->upload_speed_limit);
					addnstr(fmtbuf, n);
					addstr(" ");
				} else if (0 < global.num_upload_limited)
					addspaces(6);
			} else {
				addstr("[");

				attr_set(A_NORMAL, COLOR_UP, NULL);

				n = fmt_percent(fmtbuf, d->uploaded, d->total);
				addnstr(fmtbuf, n);

				attr_set(A_NORMAL, 0, NULL);

				if (0 < d->seed_ratio) {
					attr_set(A_NORMAL, 0, NULL);
					addstr("/");
					n = fmt_percent(fmtbuf, d->seed_ratio, 100);
					addnstr(fmtbuf, n);
					addstr("]");
				} else {
					addstr("]");
					if (0 < global.num_upload_limited)
						addspaces(6);
				}
			}

			attr_set(A_NORMAL, 0, NULL);
		} else {
			addspaces(12 + (0 < global.num_upload_limited ? 6 : 0));
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
	if (0 < tag_col_width)
		printw("%*.s", tag_col_width - tagwidth, "");

skip_tags:
	addstr(" ");
	namex = x = curx = getcurx(stdscr);
	if (draw_parents && NULL != d->parent) {
		struct download const *parent;

		for (parent = d; NULL != parent->parent; parent = parent->parent)
			x += 2;
		x += 1;
		namex = x;

		if (d->follows)
			mvaddstr(*y, x -= 3, "⮡  ");
		else
			mvaddstr(*y, x -= 3, NULL != d->next_sibling ? "├─ " : "└─ ");

		for (parent = d->parent; NULL != parent; parent = parent->parent)
			mvaddstr(*y, x -= 2, NULL != d->next_sibling ? "│ " : "  ");
	}

	mvaddstr(*y, namex, d->display_name);
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
				draw_download(downloads[topidx + line], true, &line);
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
	if (0 < num_downloads) {
		char selgid[sizeof ((struct download *)0)->gid];

		if (selidx < 0)
			selidx = 0;

		if ((size_t)selidx >= num_downloads)
			selidx = num_downloads - 1;
		memcpy(selgid, downloads[selidx]->gid, sizeof selgid);

		qsort_r(downloads, num_downloads, sizeof *downloads, (int(*)(void const *, void const *, void*))downloadcmp, NULL);

		/* move selection if download moved */
		if (stickycurs && 0 != memcmp(selgid, downloads[selidx]->gid, sizeof selgid))
			selidx = get_download_bygid(selgid) - downloads;
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
	int const height = view == VIEWS[1] ? getmainheight() : 1;

	curs_set(0 < num_downloads);

	if (topidx < 0)
		topidx = 0;

	if (num_downloads <= (size_t)(topidx + height))
		topidx = (size_t)height < num_downloads ? num_downloads - (size_t)height : 0;

	if (selidx < 0)
		selidx = 0;

	if (num_downloads <= (size_t)selidx)
		selidx = (int)num_downloads - 1;

	if (selidx < 0)
		selidx = 0;

	if (oldtopidx == topidx) {
		/* selection changed */
		if (selidx < topidx)
			topidx = selidx;

		if (topidx + height <= selidx)
			topidx = selidx - (height - 1);

		if (num_downloads <= (size_t)(topidx + height))
			topidx = (size_t)height < num_downloads ? num_downloads - (size_t)height : 0;
	} else {
		/* scroll changed */
		if (selidx < topidx)
			selidx = topidx;

		if (topidx + height <= selidx)
			selidx = topidx + (height - 1);
	}

	if (oldtopidx == topidx) {
		move(view == VIEWS[1] ? selidx - topidx : 0, curx);
	} else {
		oldtopidx = topidx;
		on_scroll_changed();
		/* update now seen downloads */
		update();
	}

	if (oldselidx != selidx) {
		oldselidx = selidx;
		draw_statusline();
	}
}

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

	ndown = fmt_speed(sdown, global.download_speed_total);
	nup = fmt_speed(sup, global.upload_speed_total);
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
	uint8_t i;
	bool first = true;
	uint64_t speed;

	update_title();

	getmaxyx(stdscr, y, w);

	/* print downloads info at the left */
	--y, x = 0;

	move(y, 0);
	attr_set(A_NORMAL, 0, NULL);

	printw("%c %4d/%d",
			view,
			0 < num_downloads ? selidx + 1 : 0, num_downloads);

	for (i = DOWNLOAD_UNKNOWN; i < DOWNLOAD_COUNT; ++i) {
		if (0 == global.num_perstatus[i])
			continue;

		addstr(first ? " (" : " ");
		first = false;

		attr_set(A_BOLD, i == DOWNLOAD_ERROR ? COLOR_ERR : 0, NULL);
		addnstr(DOWNLOAD_SYMBOLS + i, 1);
		attr_set(A_NORMAL, i == DOWNLOAD_ERROR ? COLOR_ERR : 0, NULL);
		printw("%d", global.num_perstatus[i]);
		attr_set(A_NORMAL, 0, NULL);
	}

	if (!first)
		addstr(")");

	printw(" @ %s:%u%s",
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
	if (0 < global.upload_speed_limit) {
		n = fmt_speed(fmtbuf, global.upload_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	speed = 0 == action.num_sel ? global.upload_speed : global.upload_speed_total;
	if (0 < speed)
		attr_set(A_BOLD, COLOR_UP, NULL);
	n = fmt_speed(fmtbuf, speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 4, "] @ ");
	n = fmt_percent(fmtbuf, global.uploaded_total, global.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);
	mvaddstr(y, x -= 1, "[");

	n = fmt_space(fmtbuf, global.uploaded_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, " ↑ ");
	attr_set(A_NORMAL, 0, NULL);

	/* download */
	if (0 < global.download_speed_limit) {
		n = fmt_speed(fmtbuf, global.download_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	speed = 0 == action.num_sel ? global.download_speed : global.download_speed_total;
	if (0 < speed)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
	n = fmt_speed(fmtbuf, speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, " @ ");

	n = fmt_space(fmtbuf, global.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, " ↓ ");
	attr_set(A_NORMAL, 0, NULL);

	draw_cursor();
}

static void
draw_all(void)
{
	draw_main();
}

static void
print_gid(struct download const *d)
{
	fprintf(stdout, "%s\n", d->gid);
}

static void
foreach_download(void(*cb)(struct download const *))
{
	struct download **dd = downloads;
	struct download **const end = &downloads[num_downloads];

	for (dd = downloads; dd < end; ++dd)
		cb(*dd);
}

static void
update_all_handler(struct json_node const *result, void *arg)
{
	size_t downloadidx = 0;

	(void)arg;

	if (NULL == result)
		return;

	clear_downloads();

	result = json_children(result);
	parse_global_stat(result);

	result = json_next(result);
	do {
		struct json_node const *downloads_list;
		struct json_node const *node;

		if (json_arr != json_type(result)) {
			error_handler(result);
			continue;
		}

		downloads_list = json_children(result);

		if (json_isempty(downloads_list))
			continue;

		node = json_children(downloads_list);
		do {
			struct download *d;
			/* XXX: parse_download's belongsTo and followedBy may
			 * create the download before this update arrives, so
			 * we check if only we modified the list */
			struct download **dd = downloadidx == num_downloads
				? new_download()
				: get_download_bygid(json_get(node, "gid")->val.str);
			if (NULL == dd)
				continue;
			d = *dd;

			/* we did it initially but we have to mark it now since
			 * we only received download object now */
			d->initialized = true;

			parse_download(d, node);
			if (DOWNLOAD_WAITING == abs(d->status)) {
				/* FIXME: warning: it is some serious shit */
				d->queue_index = (num_downloads - 1) - downloadidx;
			}

			download_changed(d);
			++downloadidx;
		} while (NULL != (node = json_next(node)));

	} while (NULL != (result = json_next(result)));

	switch (action.kind) {
	case act_visual:
		/* no-op */
		break;

	case act_add_downloads:
	case act_shutdown:
		/* handled earlier */
		break;

	case act_print_gid:
		foreach_download(print_gid);
		exit(EXIT_SUCCESS);
		break;

	case act_pause:
	case act_unpause:
		pause_download(NULL, act_pause == action.kind, do_forced);
		break;

	case act_purge:
		purge_download(NULL);
		break;
	}

	if (act_visual == action.kind) {
		on_downloads_change(0);
		refresh();

		/* schedule updates and request one now immediately */
		set_periodic_update();
		update();
	}
}

static void
runaction_maychanged(struct download *d)
{
	if (NULL != d) {
		update_download_tags(d);
		draw_main();
	}
}

/* revive ncurses */
static void
begwin(void)
{
	/* heh. this shit gets forgotten. */
	keypad(stdscr, TRUE);

	/* *slap* */
	refresh();
}

/* Returns:
 * - <0: action did not run.
 * - =0: action executed and terminated successfully.
 * - >0: action executed but failed. */
static int
run_action(struct download *d, char const *name, ...)
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
	begwin();

	update_title();

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

	begwin();

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
fetch_options_handler(struct json_node const *result, struct download *d)
{
	if (NULL != result) {
		clear_error_message();

		if (NULL == d || upgrade_download(d, NULL)) {
			parse_options(result, (parse_options_cb)parse_option, d);

			draw_main();
			refresh();
		}
	}

	unref_download(d);
}

static void
change_option_handler(struct json_node const *result, struct download *d)
{
	if (NULL != result)
		fetch_options(d, false);

	unref_download(d);
}

/* show received download or global options to user */
static void
show_options_handler(struct json_node const *result, struct download *d)
{
	FILE *f;
	ssize_t len;
	char *line;
	size_t linesiz;
	struct rpc_request *rpc;
	int action;
	struct stat stbefore, stafter;

	if (NULL == result)
		goto out;

	clear_error_message();

	if (NULL != d && !upgrade_download(d, NULL))
		goto out;

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
	if (stafter.st_mtim.tv_sec <= stbefore.st_mtim.tv_sec)
		goto out_fclose;

	if (NULL == (rpc = new_rpc()))
		goto out_fclose;

	rpc->handler = (rpc_handler)change_option_handler;
	rpc->arg = ref_download(d);

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

	do_rpc(rpc);

out_fclose:
	fclose(f);

out:
	unref_download(d);
}

/* if user requested it, show it. otherwise just update returned values in the
 * background */
static void
fetch_options(struct download *d, bool user)
{
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = (rpc_handler)(user ? show_options_handler : fetch_options_handler);
	rpc->arg = ref_download(d);

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

	do_rpc(rpc);
}

static void
show_options(struct download *d)
{
	fetch_options(d, true);
}

/* select (last) newly added download and write back errorneous files */
static void
add_downloads_handler(struct json_node const *result, void *arg)
{
	bool ok = true;

	(void)arg;

	if (json_isempty(result))
		return;

	result = json_children(result);
	do {
		struct download **dd;

		if (json_obj == json_type(result)) {
			ok = false;
			error_handler(result);
		} else {
			struct json_node const *gid = json_children(result);

			if (json_str == json_type(gid)) {
				/* single GID returned */
				if (NULL != (dd = get_download_bygid(gid->val.str)))
					selidx = dd - downloads;
			} else {
				/* get first GID of the array */
				if (NULL != (dd = get_download_bygid(json_children(gid)->val.str)))
					selidx = dd - downloads;
			}
		}
	} while (NULL != (result = json_next(result)));

	switch (action.kind) {
	case act_add_downloads:
		exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
		break;

	case act_visual:
		on_downloads_change(1);
		refresh();
		break;

	default:
		/* no-op */
		break;
	}
}

static void
add_downloads(char cmd)
{
	struct rpc_request *rpc;
	FILE *f;
	char *line;
	size_t linesiz;
	ssize_t len;
	int action;

	if ('\0' != cmd) {
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
	} else {
		f = stdin;
	}

	if (NULL == (rpc = new_rpc()))
		goto out_fclose;

	rpc->handler = (rpc_handler)add_downloads_handler;

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

	do_rpc(rpc);

out_fclose:
	fclose(f);
}

static void
update_all(void)
{
	unsigned n;
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = update_all_handler;

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
		bool pageable;

		switch (n) {
		case 0:
			tell = "aria2.tellWaiting";
			pageable = true;
			break;

		case 1:
			tell = "aria2.tellActive";
			pageable = false;
			break;

		case 2:
			tell = "aria2.tellStopped";
			pageable = true;
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
			json_write_int(jw, 99999);
		}

		/* “keys” {{{ */
		json_write_beginarr(jw);
		json_write_str(jw, "gid");
		json_write_str(jw, "status");
		json_write_str(jw, "errorMessage");
		if (act_visual == action.kind) {
			json_write_str(jw, "totalLength");
			json_write_str(jw, "completedLength");
			json_write_str(jw, "uploadLength");
			json_write_str(jw, "verifiedLength");
			json_write_str(jw, "verifyIntegrityPending");
			json_write_str(jw, "uploadSpeed");
			json_write_str(jw, "downloadSpeed");

			json_write_str(jw, "bittorrent");
			json_write_str(jw, "numPieces");
			json_write_str(jw, "pieceLength");
			json_write_str(jw, "following");
			json_write_str(jw, "belongsTo");
		}
		if (act_visual == action.kind ? is_local : action.uses_files)
			json_write_str(jw, "files");
		json_write_endarr(jw);
		/* }}} */

		json_write_endarr(jw);
		json_write_endobj(jw);
	}
out_of_loop:

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	do_rpc(rpc);
}

static void
remove_download_handler(struct json_node const *result, struct download *d)
{
	if (NULL != result) {
		struct download **dd;

		if (upgrade_download(d, &dd)) {
			delete_download_at(dd);

			on_downloads_change(1);
			refresh();
		}
	}

	unref_download(d);
}

static void
remove_download(struct download *d, bool force)
{
	struct rpc_request *rpc;

	if (run_action(d, "D") < 0) {
		if (DOWNLOAD_ACTIVE == abs(d->status)) {
			set_error_message("refusing to delete active download");
			return;
		}
	}

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = (rpc_handler)remove_download_handler;
	rpc->arg = ref_download(d);

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceRemove" : "aria2.remove");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	do_rpc(rpc);
}

static void
switch_view(int next)
{
	if (!(view = strchr(VIEWS + 1, view)[next ? 1 : -1])) {
		view = VIEWS[next ? 1 : array_len(VIEWS) - 2];
		oldselidx = -1; /* force redraw */
	}

	update();
	draw_all();
	refresh();
}

static void
select_download(struct download const *d)
{
	struct download **dd;

	if (NULL != d && upgrade_download(d, &dd)) {
		selidx = dd - downloads;
		draw_cursor();
		refresh();
	}
}

static void
jump_prev_group(void)
{
	if (0 < num_downloads) {
		struct download **dd = &downloads[selidx];
		struct download **lim = &downloads[0];
		int8_t status = abs((*dd)->status);

		while (lim <= --dd) {
			if (status != abs((*dd)->status)) {
				status = abs((*dd)->status);

				/* go to the first download of the group */
				while (lim <= --dd && status == abs((*dd)->status))
					;

				select_download(dd[1]);
				break;
			}
		}
	}
}

static void
jump_next_group(void)
{
	if (0 < num_downloads) {
		struct download **dd = &downloads[selidx];
		struct download **lim = &downloads[num_downloads];
		int8_t status = abs((*dd)->status);

		while (++dd < lim) {
			if (status != abs((*dd)->status)) {
				select_download(*dd);
				break;
			}
		}
	}
}

static void
read_stdin(void)
{
	int ch;

	while (ERR != (ch = getch())) {
		switch (ch) {
		/*MAN(KEYS)
		 * .TP
		 * .BR j ,\  Down
		 * Move selection downwards.
		 */
		case 'j':
		case KEY_DOWN:
			++selidx;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR k ,\  Up
		 * Move selection upwards.
		 */
		case 'k':
		case KEY_UP:
			--selidx;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR h
		 * Select parent.
		 */
		case 'h':
			if (0 < num_downloads)
				select_download(downloads[selidx]->parent);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR l
		 * Select first children.
		 */
		case 'l':
			if (0 < num_downloads)
				select_download(downloads[selidx]->first_child);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR n
		 * Select next sibling.
		 */
		case 'n':
			if (0 < num_downloads)
				select_download(downloads[selidx]->next_sibling);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR N
		 * Select previous sibling.
		 */
		case 'N':
			if (0 < num_downloads)
				select_download(download_prev_sibling(downloads[selidx]));
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR g ,\  Home
		 * Select first.
		 */
		case 'g':
		case KEY_HOME:
			selidx = 0;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR G ,\  End
		 * Select last.
		 */
		case 'G':
		case KEY_END:
			selidx = INT_MAX;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Ctrl-E
		 * Scroll small downwards.
		 */
		case CONTROL('E'):
			topidx += 3;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Ctrl-Y
		 * Scroll small upwards.
		 */
		case CONTROL('Y'):
			topidx -= 3;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Ctrl-D
		 * Scroll half-screen downwards.
		 */
		case CONTROL('D'):
			topidx += getmainheight() / 2;
			selidx += getmainheight() / 2;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Ctrl-U
		 * Scroll half-screen upwards.
		 */
		case CONTROL('U'):
			topidx -= getmainheight() / 2;
			selidx -= getmainheight() / 2;
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR Ctrl-F ,\  PageDown
		 * Scroll one screen downwards.
		 */
		case KEY_NPAGE:
		case CONTROL('F'):
			topidx += getmainheight();
			selidx += getmainheight();
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR Ctrl-B ,\  PageUp
		 * Scroll one screen upwards.
		 */
		case KEY_PPAGE:
		case CONTROL('B'):
			topidx -= getmainheight();
			selidx -= getmainheight();
			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR [
		 * Select first download of the previous status group.
		 */
		case '[':
			jump_prev_group();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR ]
		 * Select first download of the next status group.
		 */
		case ']':
			jump_next_group();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B J
		 * Move selected download backward in the queue.
		 */
		case 'J':
		/*MAN(KEYS)
		 * .TP
		 * .B K
		 * Move selected download forward in the queue.
		 */
		case 'K':
			if (0 < num_downloads)
				change_download_position(downloads[selidx], ch == 'J' ? 1 : -1, POS_CUR);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR s ,\  S
		 * Start (unpause) selected/all download(s).
		 * .TP
		 * .BR p ,\  P
		 * Pause selected/all download(s).
		 */
		case 's':
		case 'p':
			if (0 < num_downloads)
				pause_download(downloads[selidx], ch == 'p', do_forced);
			do_forced = 0;
			break;
		case 'S':
		case 'P':
			pause_download(NULL, ch == 'P', do_forced);
			do_forced = 0;
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR v ,\  RIGHT
		 * Switch to next view.
		 */
		case 'v':
		case KEY_RIGHT:
			switch_view(1);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR V ,\  LEFT
		 * Switch to previous view.
		 */
		case 'V':
		case KEY_LEFT:
			switch_view(0);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR a ,\  A ,\  +
		 * Add download(s) from
		 * .IR "session file" .
		 * Open
		 * .I session file
		 * for editing unless there are associated actions. Select lastly added download.
		 * .sp
		 * For the expected format refer to aria2's
		 * .IR "Input File" .
		 * Note that URIs that end with
		 * .BR .torrent \ and\  .meta4 \ or\  .metalink
		 * and refer to a readable file on the local filesystem, are got uploaded to
		 * aria2 as a blob.
		 * .sp
		 * Insert downloads after the selection in the queue.
		 */
		case 'a':
		case 'A':
		case '+':
			add_downloads(ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR i ,\  I
		 * Write selected download/global options in a
		 * .BR = -separated
		 * key-value format into the
		 * .IR "session file" .
		 * Open
		 * .I session file
		 * for viewing unless there are associated actions.
		 * Edit the file to change options.
		 */
		case 'i':
		case 'I':
			show_options(0 < num_downloads && ch == 'i' ? downloads[selidx] : NULL);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR D ,\  Del
		 * If there is an action for \*(lqD\*(rq execute it, otherwise
		 * remove a non-active download.
		 */
		case 'D':
		case KEY_DC: /*delete*/
			if (0 < num_downloads) {
				remove_download(downloads[selidx], do_forced);
				do_forced = 0;
			}
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR x ,\  X
		 * Remove download result for selection/all downloads.
		 */
		case 'x':
			if (0 < num_downloads)
				purge_download(downloads[selidx]);
			break;
		case 'X':
			purge_download(NULL);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B q
		 * Go back to default view. On default view quit.
		 */
		case 'q':
		case CONTROL('['):
			if (view != VIEWS[1]) {
				view = VIEWS[1];
				oldselidx = -1; /* force redraw */
				draw_all();
				refresh();
				continue;
			}
			/* fallthrough */
		/*MAN(KEYS)
		 * .TP
		 * .B Z
		 * Quit program.
		 */
		case 'Z':
			exit(EXIT_SUCCESS);

		/*MAN(KEYS)
		 * .TP
		 * .B !
		 * Do next command by force. With Torrent downloads, for
		 * example, it means that it will not contant to tracker(s).
		 */
		case '!':
			do_forced = 1;
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Return
		 * If standard output is not connected to a terminal, print selected GID and exit.
		 */
		case CONTROL('M'):
			if (isatty(STDOUT_FILENO)) {
				goto default_action;
			} else {
				endwin();

				if (0 < num_downloads) {
					struct download const *d = downloads[selidx];
					printf("%s\n", d->gid);
				}

				exit(EXIT_SUCCESS);
			}
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Q
		 * Run action \*(lqQ\*(rq then shut down aria2 if action terminated with success.
		 */
		case 'Q':
			if (EXIT_FAILURE != run_action(NULL, "Q"))
				shutdown_aria(do_forced);
			do_forced = 0;
			break;

		/* undocumented */
		case CONTROL('L'):
			/* try connect if not connected */
			if (!ws_isalive())
				try_connect();
			action.kind = act_visual;
			action.num_sel = 0;
			update_all();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B Ctrl-C
		 * Quit program with failure.
		 */
		case CONTROL('C'):
			exit(EXIT_FAILURE);

		/*MAN(KEYS)
		 * .TP
		 * .R (other)
		 * Run action named like the pressed key. Uses
		 * .BR keyname (3x).
		 */
		default:
		default_action:
			run_action(NULL, "%s", keyname(ch));
			do_forced = 0;
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
remote_info_handler(struct json_node const *result, void *arg)
{
	struct json_node const *node;

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

	switch (action.kind) {
	case act_add_downloads:
		add_downloads('\0');
		break;

	case act_shutdown:
		shutdown_aria(do_forced);
		break;

	default:
		update_all();
		break;
	}
}

static void
remote_info(void)
{
	struct rpc_request *rpc;

	if (NULL == (rpc = new_rpc()))
		return;

	rpc->handler = remote_info_handler;

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

	do_rpc(rpc);
}

static void
setasync(int fd)
{
	fcntl(fd, F_SETFL, O_ASYNC | fcntl(fd, F_GETFL));
	fcntl(fd, F_SETOWN, getpid());
	fcntl(fd, F_SETSIG, SIGIO);
}

void
on_ws_open(void)
{
	setasync(ws_fileno());
	periodic = NULL;

	clear_error_message();

	oldselidx = -1;
	oldtopidx = -1;

	is_local = 0;
	remote_info();

	draw_all();
	refresh();
}

void
on_ws_close(void)
{
	if (0 == errno)
		exit(EXIT_SUCCESS);

	if (act_visual != action.kind)
		exit(EXIT_FAILURE);

	if (try_connect != periodic) {
		periodic = try_connect;
		period.tv_sec = 1;
		period.tv_nsec = 0;
	} else {
		period.tv_sec *= 2;
	}

	clear_rpc_requests();

	clear_downloads();
	free(downloads), downloads = NULL;

	cleanup_session_file();

	draw_all();
	refresh();
}

static void
init_action(void)
{
	size_t i;

	for (i = 0; i < action.num_sel; ++i) {
		if (!isgid(action.sel[i])) {
			action.uses_files = true;
			break;
		}
	}
}

int
main(int argc, char *argv[])
{
	sigset_t ss;
	int argi;

	setlocale(LC_ALL, "");

	for (argi = 1; argi < argc;) {
		char *arg = argv[argi++];
		if (0 == strcmp(arg, "--select")) {
			if (argc <= argi)
				goto show_usage;

			action.num_sel = 1;
			action.sel = &argv[argi];
			while (++argi, NULL != argv[argi] && '-' != *argv[argi])
				++action.num_sel;
		} else if (0 == strcmp(arg, "--print-gid")) {
			action.kind = act_print_gid;
		} else if (0 == strcmp(arg, "--add")) {
			action.kind = act_add_downloads;
		} else if (0 == strcmp(arg, "--shutdown")) {
			action.kind = act_shutdown;
		} else if (0 == strcmp(arg, "--pause")) {
			action.kind = act_pause;
		} else if (0 == strcmp(arg, "--unpause")) {
			action.kind = act_unpause;
		} else if (0 == strcmp(arg, "--purge")) {
			action.kind = act_purge;
		} else if (0 == strcmp(arg, "--force")) {
			do_forced = 1;
		} else {
		show_usage:
			return EXIT_FAILURE;
		}
	}

	load_config();

	init_action();

	json_writer_init(jw);

	atexit(clear_downloads);
	atexit(cleanup_session_file);

	sigemptyset(&ss);
	sigaddset(&ss, SIGIO);
	sigaddset(&ss, SIGWINCH);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGHUP);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGKILL);
	sigaddset(&ss, SIGQUIT);
	sigaddset(&ss, SIGPIPE);

	sigprocmask(SIG_BLOCK, &ss, NULL);

	if (act_visual == action.kind) {
		setasync(STDIN_FILENO);

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
		/* make getch() non-blocking */
		nodelay(stdscr, TRUE);
		/* catch special keys */
		keypad(stdscr, TRUE);
		/* 8-bit inputs */
		meta(stdscr, TRUE);

		update_terminfo();

		draw_all();
		refresh();
	}

	try_connect();

	for (;;) {
		siginfo_t si;
		int signum;

		signum = NULL != periodic
			? sigtimedwait(&ss, &si, &period)
			: sigwaitinfo(&ss, &si);

		switch (signum) {
		case -1:
			periodic();
			break;

		case SIGIO:
			if (STDIN_FILENO == si.si_fd)
				read_stdin();
			else
				ws_read();
			break;

		case SIGWINCH:
			/* this is how ncurses reinitializes its window size */
			endwin();
			begwin();

			/* and now we can redraw the whole screen with the
			 * hopefully updated screen dimensions */
			draw_all();
			refresh();
			break;

		case SIGINT:
		case SIGHUP:
		case SIGTERM:
		case SIGKILL:
		case SIGQUIT:
			exit(EXIT_SUCCESS);
			break;

		case SIGPIPE:
			/* ignore */
			break;
		}
	}
}
