#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#include "keys.c.inc"

#define addspaces(n) addnstr("                                          "/*31+5+6=42*/, n);

enum {
	COLOR_DOWN = 1,
	COLOR_UP,
	COLOR_CONN,
	COLOR_ERR,
	COLOR_EOF,
};

#define CONTROL(letter) ((letter) - '@')
#define ARRAY_SIZE(x) (sizeof x / sizeof *x)

#define VIEWS "\0dfp"

/* str := "true" | "false" */
#define IS_TRUE(str) ('t' == str[0])

#define PEER_ADDRESS_WIDTH (int)(2 + sizeof(((Peer *)0)->ip) + 1 + 5)
#define PEER_INFO_WIDTH 24

static char const *tags_xattr;

typedef struct {
	uint8_t peer_id[20];

	char ip[INET6_ADDRSTRLEN];
	in_port_t port;

	bool up_choked: 1;
	bool down_choked: 1;

	uint32_t pieces_have;
	struct timespec latest_change; /* Latest time when pieces_have changed. */

	uint64_t peer_download_speed;

	uint64_t download_speed; /* ...from peer. */
	uint64_t upload_speed; /* ...to peer. */

	uint8_t *progress;
} Peer;

typedef struct {
	char *current_uri;
	uint64_t download_speed;
} Server;

enum URIStatus {
	URI_STATUS_WAITING,
	URI_STATUS_USED
};

typedef struct {
	enum URIStatus status;
	uint32_t num_servers;
	char *uri;
	Server *servers;
} URI;

typedef struct {
	char *path;
	uint64_t total;
	uint64_t have;
	uint32_t num_uris;
	bool selected;
	URI *uris;
} File;

static char const NONAME[] = "?";

/* NOTE: Order is relevant for sorting. */
enum DownloadType {
	D_UNKNOWN,
	D_WAITING,
	D_ACTIVE,
	D_PAUSED,
	D_COMPLETE,
	D_REMOVED,
	D_ERROR,
	D_NB,
};

static char const DOWNLOAD_SYMBOLS[D_NB] = "?w>|.-*";

typedef struct Download Download;
struct Download {
	char *name;
	char const *display_name;
	char gid[16 + 1];

	uint8_t refcnt; /* dynamic weak refs */
	bool deleted: 1; /* one strong ref: is on the list or not */
	bool initialized: 1;
	bool requested_options: 1;
	int8_t status; /* D_*; or negative if changed. */
	uint16_t queue_index;

	char *error_msg;

	uint32_t num_files, num_files_selected;

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

	bool follows; /* Follows or belongsTo |parent|. */
	Peer *peers;
	File *files;
	uint8_t *progress;

	Download *parent;
	Download *first_child;
	Download *next_sibling;

	char *tags;
};

static Download **downloads;
static size_t num_downloads;

typedef struct {
	size_t count;
	Download *downloads[];
} Selection;

static struct {
	uint64_t download_speed;
	uint64_t upload_speed;

	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

	/* Computed locally. */
	uint64_t download_speed_total;
	uint64_t upload_speed_total;
	uint64_t have_total;
	uint64_t uploaded_total;

	/* Number of downloads that have download-speed-limit set. */
	uint64_t num_download_limited;
	/* Number of downloads that have upload-speed-limit or seed-ratio set. */
	uint64_t num_upload_limited;

	/* Number of downloads per status. */
	uint32_t num_perstatus[D_NB];

	char external_ip[INET6_ADDRSTRLEN];
} global;

static struct {
	char const *tsl;
	char const *fsl;
} ti;

static bool select_need_files;
static size_t num_selects;
static char const **selects;

static char const* remote_host;
static in_port_t remote_port;
static char *secret_token;

static char session_file[PATH_MAX];
static bool is_local; /* Server runs on local host? */

struct pollfd pfds[2];
static struct timespec interval;
static void(*on_timeout)(void);

typedef void(*RPCHandler)(JSONNode const *result, void *arg);
typedef struct {
	RPCHandler handler;
	void *arg;
} RPCRequest;
static RPCRequest rpc_requests[10];

static char view = VIEWS[1];

static int curx = 0;
static bool downloads_need_reflow;
static int tag_col_width = 0;
static Download const *longest_tag;
static bool do_forced = false;

static int oldidx;

/* Logical state. */
static int selidx, topidx;

/* Visual state. */
static struct {
	int selidx, topidx;
} tui;

static int oldidx;

static JSONWriter jw[1];

static void draw_statusline(void);
static void draw_main(void);
static void draw_files(void);
static void draw_peers(void);
static void draw_downloads(void);
static void draw_download(Download const *d, bool draw_parents, int *y);

static void
free_peer(Peer *p)
{
	free(p->progress);
	(void)p;
}

static void
free_server(Server *s)
{
	free(s->current_uri);
}

static void
free_uri(URI *u)
{
	free(u->uri);

	if (u->servers) {
		for (uint32_t i = 0; i < u->num_servers; ++i)
			free_server(&u->servers[i]);

		free(u->servers);
	}
}

static void
free_file(File *f)
{
	if (f->uris) {
		for (uint32_t i = 0; i < f->num_uris; ++i)
			free_uri(&f->uris[i]);

		free(f->uris);
	}

	free(f->path);
}

static void
free_peers(Peer *peers, uint32_t num_peers)
{
	if (!peers)
		return;

	for (uint32_t i = 0; i < num_peers; ++i)
		free_peer(&peers[i]);

	free(peers);
}

static void
free_files_of(Download *d)
{
	if (!d->files)
		return;

	for (uint32_t i = 0; i < d->num_files; ++i)
		free_file(&d->files[i]);

	free(d->files);
}

static void
unref_download(Download *d);

static Download const *
download_prev_sibling(Download const *d)
{
	/* no family at all */
	if (!d->parent)
		return NULL;

	/* first child */
	Download const *sibling = d->parent->first_child;
	if (d == sibling)
		return NULL;

	while (sibling->next_sibling != d)
		sibling = sibling->next_sibling;

	return sibling;
}

static void
free_download(Download *d)
{
	free_files_of(d);
	free_peers(d->peers, d->num_peers);
	free(d->dir);
	free(d->name);
	free(d->error_msg);
	free(d->progress);
	free(d->tags);
	free(d);
}

static Download *
ref_download(Download *d)
{
	if (d)
		++d->refcnt;
	return d;
}

static void
unref_download(Download *d)
{
	if (!d)
		return;

	assert(d->refcnt >= 1);
	if (!--d->refcnt)
		free_download(d);
}

static void
delete_download_at(Download **dd)
{
	Download *d = *dd;

	assert(!d->deleted);

	*dd = downloads[--num_downloads];

	if (longest_tag == d) {
		tag_col_width = 0;
		longest_tag = NULL;
	}

	for (Download *next, *sibling = d->first_child;
	     sibling;
	     sibling = next)
	{
		next = sibling->next_sibling;
		sibling->parent = NULL;
		sibling->next_sibling = NULL;
	}

	/* remove download from family tree */
	if (d->parent) {
		Download *sibling = d->parent->first_child;
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
handle_default(JSONNode const *result, void *arg)
{
	(void)result, (void)arg;
}

static void
init_config(void)
{
	char *str;

	(tags_xattr = getenv("ARIA2T_TAGS_XATTR")) ||
	(tags_xattr = "user.tags");

	(remote_host = getenv("ARIA_RPC_HOST")) ||
	(remote_host = "127.0.0.1");

	((str = getenv("ARIA_RPC_PORT")) &&
		(errno = 0,
		 remote_port = strtoul(str, NULL, 10),
		 !errno)) ||
	(remote_port = 6800);

	/* "token:$$secret$$" */
	(str = getenv("ARIA_RPC_SECRET")) ||
	(str = "");

	secret_token = malloc(snprintf(NULL, 0, "token:%s", str) + 1);
	if (!secret_token)
		abort();

	sprintf(secret_token, "token:%s", str);
}

static Download **
new_download(void)
{
	Download *d;
	Download **dd;

	if (!(dd = realloc(downloads, (num_downloads + 1) * sizeof *downloads)))
		return NULL;
	downloads = dd;

	if (!(d = calloc(1, sizeof *d)))
		return NULL;
	*(dd = &downloads[num_downloads++]) = d;

	d->refcnt = 1;
	d->display_name = NONAME;
	global.num_perstatus[D_UNKNOWN] += 1;

	return dd;
}

/* return whether |d| is still valid (on the list); |pdd| will be filled with
 * location of |d| if not NULL */
static bool
upgrade_download(Download const *d, Download ***pdd)
{
	if (d->deleted)
		return false;

	if (pdd) {
		Download **dd = downloads;

		for (; *dd != d; ++dd);

		*pdd = dd;
	}

	return true;
}

static bool
filter_download(Download *d, Download **dd);

static bool
islxdigit(char c)
{
	return ('0' <= c && c <= '9') ||
	       ('a' <= c && c <= 'f');
}

static Download *
find_download_by_gid(char const *gid, char const **endptr)
{
	char const *p = gid;

	if (!islxdigit(*p++))
		return NULL;

	uint8_t n = 0;
	while (++n < 16 && islxdigit(*p++));

	if (endptr)
		*endptr = p;

	for (size_t i = 0; i < num_downloads; ++i) {
		Download *d = downloads[i];
		if (!memcmp(d->gid, gid, n))
			return d;
	}

	return NULL;
}

static Download **
get_download_by_gid(char const *gid)
{
	Download *d;
	Download **dd = downloads;
	Download **end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (!memcmp((*dd)->gid, gid, sizeof (*dd)->gid))
			return dd;

	/* No new downloads are accepted. */
	if (SIZE_MAX == num_selects)
		return NULL;

	if (!(dd = new_download()))
		return NULL;
	d = *dd;

	assert(strlen(gid) == sizeof d->gid - 1);
	memcpy(d->gid, gid, sizeof d->gid);

	/* Try applying selectors as soon as possible. */
	return filter_download(d, dd) ? dd : NULL;
}

static void
clear_error(void)
{
	show_error(NULL);
}

static void
handle_error(JSONNode const *error)
{
	JSONNode const *message = json_get(error, "message");

	show_error("%s", message->str);
	draw_statusline();
	refresh();
}

static void
free_rpc(RPCRequest *rpc)
{
	rpc->handler = NULL;
}

static void
begin_rpc_method(char const *method, Download *d)
{
	json_write_key(jw, "method");
	json_write_str(jw, method);

	json_write_key(jw, "params");
	json_write_beginarr(jw);

	/* "secret" */
	json_write_str(jw, secret_token);
	if (d)
		/* "gid" */
		json_write_str(jw, d->gid);
}


static void
end_rpc_method(void)
{
	json_write_endarr(jw);
}

static void
generate_rpc_method(char const *method)
{
	begin_rpc_method(method, NULL);
	end_rpc_method();
}

static void
begin_rpc_multicall(void)
{
	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* "methods" */
	json_write_beginarr(jw);
}

static void
end_rpc_multicall(void)
{
	json_write_endarr(jw);
	json_write_endarr(jw);
}

/* TODO: Maybe these function pairs could be made into a macro. That makes
 * generate_*s unnecessary. */
static void
begin_rpc_multicall_method(char const *method, Download const *d)
{
	json_write_beginobj(jw);
	json_write_key(jw, "methodName");
	json_write_str(jw, method);
	json_write_key(jw, "params");
	json_write_beginarr(jw);

	/* "secret" */
	json_write_str(jw, secret_token);
	/* "gid" */
	if (d)
		json_write_str(jw, d->gid);
}

static void
end_rpc_multicall_method(void)
{
	json_write_endarr(jw);
	json_write_endobj(jw);
}

static void
generate_rpc_multicall_method(char const *method, Download const *d)
{
	begin_rpc_multicall_method(method, d);
	end_rpc_multicall_method();
}

static void
on_downloads_change(bool stickycurs);

static void
update_downloads(void);

static bool
download_insufficient(Download *d)
{
	return d->display_name == NONAME ||
	       -D_WAITING == d->status ||
	       -D_ERROR == d->status ||
	       (is_local && !d->num_files);
}

static void
queue_changed(void)
{
	Download **dd = downloads;
	Download **end = &downloads[num_downloads];

	for (; dd < end; ++dd) {
		Download *d = *dd;

		if (D_WAITING == abs(d->status))
			d->status = -D_WAITING;
	}
}

static void
handle_notification(char const *method, JSONNode const *event)
{
	static int8_t const STATUS_MAP[] = {
		[K_notification_none              ] = D_UNKNOWN,
		[K_notification_DownloadStart     ] = D_ACTIVE,
		[K_notification_DownloadPause     ] = D_PAUSED,
		[K_notification_DownloadStop      ] = D_PAUSED,
		[K_notification_DownloadComplete  ] = D_COMPLETE,
		[K_notification_DownloadError     ] = D_ERROR,
		[K_notification_BtDownloadComplete] = D_ACTIVE
	};

	char const *gid = json_get(event, "gid")->str;
	Download **dd;
	Download *d;

	if (!(dd = get_download_by_gid(gid)))
		return;
	d = *dd;

	int8_t new_status = STATUS_MAP[K_notification_parse(method + strlen("aria2.on"))];

	if (new_status != D_UNKNOWN && new_status != d->status) {
		if (D_WAITING == abs(d->status))
			queue_changed();

		--global.num_perstatus[abs(d->status)];
		d->status = -new_status;
		++global.num_perstatus[new_status];

		on_downloads_change(true);
		refresh();

		/* in a perfect world we should update only this one download */
		if (download_insufficient(d))
			update_downloads();
	}
}

void
on_ws_message(char *msg, uint64_t msg_size)
{
	static JSONNode *nodes;
	static size_t num_nodes;

	JSONNode const *id;
	JSONNode const *method;

	(void)msg_size;

	json_parse(msg, &nodes, &num_nodes, 0);

	if ((id = json_get(nodes, "id"))) {
		JSONNode const *result = json_get(nodes, "result");
		RPCRequest *rpc = &rpc_requests[(unsigned)id->num];

		if (!result)
			handle_error(json_get(nodes, "error"));

		/* NOTE: this condition shall always be true, but aria2c
		 * responses with some long messages with "Parse error." that
		 * contains id=null, so we cannot get back the handler. the
		 * best we can do is to ignore and may leak a resource inside
		 * data. */
		if (rpc->handler) {
			rpc->handler(result, rpc->arg);
			free_rpc(rpc);
		}

	} else if ((method = json_get(nodes, "method"))) {
		JSONNode const *params = json_get(nodes, "params");

		handle_notification(method->str, params + 1);
	} else {
		assert(0);
	}
}

static RPCRequest *
new_rpc(void)
{
	RPCRequest *rpc;

	for (uint8_t i = 0; i < ARRAY_SIZE(rpc_requests); ++i) {
		rpc = &rpc_requests[i];

		if (!rpc->handler)
			goto found;
	}

	show_error("Too many pending messages");
	draw_statusline();

	return NULL;

found:
	rpc->handler = handle_default;

	json_writer_reset(jw);
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
	for (uint8_t n = ARRAY_SIZE(rpc_requests); 0 < n;) {
		RPCRequest *rpc = &rpc_requests[--n];

		if (rpc->handler) {
			rpc->handler(NULL, rpc->arg);
			free_rpc(rpc);
		}
	}
}

static void
fetch_options(Selection *s, bool all, bool user);

static void
parse_session_info(JSONNode const *result)
{
	char *tmpdir;

	(tmpdir = getenv("TMPDIR")) ||
	(tmpdir = "/tmp");

	snprintf(session_file, sizeof session_file, "%s/aria2t.%s",
			tmpdir,
			json_get(result, "sessionId")->str);
}

static bool
do_rpc(RPCRequest *rpc)
{
	json_write_endobj(jw);

	int rc = ws_send(jw->buf, jw->buf_size);
	if (rc < 0) {
		free_rpc(rpc);

		show_error("write: %s", strerror(-rc));
		draw_statusline();

		return false;
	} else {
		return true;
	}
}

static void
update_download_tags(Download *d)
{
	ssize_t size = -1;
	char tags[256];

	/* Do not try read tags if aria2 is remote. */
	if (!is_local)
		return;

	if (size < 0 && d->dir && d->name) {
		char pathbuf[PATH_MAX];

		snprintf(pathbuf, sizeof pathbuf, "%s/%s", d->dir, d->name);
		size = getxattr(pathbuf, tags_xattr, tags, sizeof tags - 1);
	}

	if (size < 0 && 0 < d->num_files && d->files[0].path)
		size = getxattr(d->files[0].path, tags_xattr, tags, sizeof tags - 1);

	free(d->tags);
	d->tags = 0 < size ? malloc(size + 1 /* NUL */) : NULL;

	if (d->tags) {
		for (ssize_t i = 0; i < size; ++i)
			if (isspace(tags[i]))
				tags[i] = ' ';

		memcpy(d->tags, tags, size);
		d->tags[size] = '\0';
	}

	/* When longest tag changes we have to reset column width; at next draw
	 * it will be recomputed. */
	if (longest_tag == d) {
		longest_tag = NULL;
		tag_col_width = 0;
	}
}

static URI *
find_file_uri(File const *f, char const *uri)
{
	for (uint32_t i = 0; i < f->num_uris; ++i) {
		URI *u = &f->uris[i];

		if (!strcmp(u->uri, uri))
			return u;
	}

	return NULL;
}

static void
uri_addserver(URI *u, Server *s)
{
	void *p;

	if (!(p = realloc(u->servers, (u->num_servers + 1) * sizeof *(u->servers))))
		return;
	(u->servers = p)[u->num_servers++] = *s;
}

static void
parse_file_servers(File *f, JSONNode const *node)
{
	for (node = json_first(node); node; node = json_next(node)) {
		JSONNode const *field = json_children(node);
		URI *u = u;
		Server s;

		do {
			if (!strcmp(field->key, "uri")) {
				u = find_file_uri(f, field->str);
				assert(u);
			} else if (!strcmp(field->key, "downloadSpeed"))
				s.download_speed = strtoul(field->str, NULL, 10);
			else if (!strcmp(field->key, "currentUri"))
				s.current_uri = (char *)field->str;
		} while ((field = json_next(field)));

		if (!strcmp(u->uri, s.current_uri))
			s.current_uri = NULL;
		if (s.current_uri)
			s.current_uri = strdup(s.current_uri);

		uri_addserver(u, &s);
	}
}

static void
parse_peer_id(Peer *p, char const *peer_id)
{
#define HEX2NR(ch) (uint8_t)(ch <= '9' ? ch - '0' : ch - 'A' + 10)

	char const *src = peer_id;
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

	assert(!*src && "Failed to parse peer_id");

#undef HEX2NR
}

static void
draw_progress(Download const *d, uint8_t const *progress, uint64_t offset, uint64_t end_offset)
{
	static uint8_t const
	SYMBOL_MAP[UINT8_MAX + 1] =
	{
#define POPCNT8(x) (((((x) * 0x0002000400080010ULL) & 0x1111111111111111ULL) * 0x1111111111111111ULL) >> 60)
#define B(x) (1 << x)|
#define S1(x) ((B(1)B(2)B(3)0 >> POPCNT8(x)) & 1 ? 10 + 0 : (B(7)0 >> POPCNT8(x)) & 1 ? 10 + 2 : 10 + 1),
#define S2(x)   S1(x)   S1(x + 1)
#define S4(x)   S2(x)   S2(x + 2)
#define S8(x)   S4(x)   S4(x + 4)
#define S16(x)  S8(x)   S8(x + 8)
#define S32(x)  S16(x)  S16(x + 8)
#define S64(x)  S32(x)  S32(x + 8)
#define S128(x) S64(x)  S64(x + 64)
#define S256(x) S128(x) S128(x + 128)

		S256(0)

		[0x00U] = 0,
		[0x01U] = 1,
		[0x03U] = 2,
		[0x07U] = 3,
		[0x0fU] = 4,
		[0x1fU] = 5,
		[0x3fU] = 6,
		[0x7fU] = 7,
		[0xffU] = 8,
		[0xf0U] = 9,
#undef B
#undef S1
#undef S2
#undef S4
#undef S8
#undef S16
#undef S32
#undef S64
#undef S128
#undef S256
#undef POPCNT8
	};

	static char const
	SYMBOLS[][4] = {
		" \0:)",          /* Empty */
		"\xe2\x96\x8f\0", /* Left 1/8 */
		"\xe2\x96\x8e\0", /* Left 2/8 */
		"\xe2\x96\x8d\0", /* Left 3/8 */
		"\xe2\x96\x8c\0", /* Left 4/8 */
		"\xe2\x96\x8b\0", /* Left 5/8 */
		"\xe2\x96\x8a\0", /* Left 6/8 */
		"\xe2\x96\x89\0", /* Left 7/8 */
		"\xe2\x96\x88\0", /* Left 8/8 */
		"\xe2\x96\x90\0", /* Right half */
		"\xe2\x96\x91\0", /* Light shade */
		"\xe2\x96\x92\0", /* Medium shade */
		"\xe2\x96\x93\0", /* Dark shade */
	};

	int width = COLS - getcurx(stdscr) - 3;

	if (/* not enough information */
	    !d->piece_size ||
	    (!progress && offset < end_offset) ||
	    /* not enough space */
	    width <= 0)
	{
		clrtoeol();
		return;
	}

	uint32_t piece_offset = offset / d->piece_size;
	uint32_t end_piece_offset = (end_offset + (d->piece_size - 1)) / d->piece_size;
	uint32_t piece_count = end_piece_offset - piece_offset;

	addstr(" [");
	size_t piece_index_from = piece_offset, piece_index_to;
	int count = 0;
	for (int i = 0; i < width; piece_index_from = piece_index_to) {
		piece_index_to = piece_offset + piece_count * ++i / width;

		++count;
		if (piece_index_from == piece_index_to && i < width)
			continue;

		if (d->num_pieces < piece_index_to)
			piece_index_to = d->num_pieces;

		uint8_t mask = UINT8_MAX;

		uint8_t last_bit = 0, bit;
		for (size_t piece_index = piece_index_from;
		     piece_index < piece_index_to;
		     ++piece_index, last_bit = bit)
		{
			bool has_piece = (progress[piece_index / CHAR_BIT] >> (CHAR_BIT - 1 - piece_index % CHAR_BIT)) & 1;
			bit = (size_t)((size_t)((piece_index + 1) - piece_index_from) * 8 / (size_t)(piece_index_to - piece_index_from));

			mask &= ~(!has_piece * (((UINT8_MAX & ~1) << bit) ^ (UINT8_MAX << last_bit)));
		}

		do
			addstr(SYMBOLS[SYMBOL_MAP[mask]]);
		while (0 < --count);
	}
	addstr("]");
}

static void
parse_progress(Download *d, uint8_t **progress, char const *bitfield)
{
#define PARSE_HEX(hex) ((hex) <= '9' ? (hex) - '0' : (hex) - 'a' + 10)
	free(*progress);

	/* number of pieces is unknown so we ignore bitfield */
	if (!d->num_pieces) {
		*progress = NULL;
		return;
	}

	uint8_t *p;
	if (!(p = *progress = malloc((d->num_pieces + CHAR_BIT * 2 - 1) / CHAR_BIT)))
		return;

	/* we may read junk but who cares */
	for (;; bitfield += 2) {
		*p++ = (PARSE_HEX(bitfield[1])) | (PARSE_HEX(bitfield[0]) << 4);
		if (!(bitfield[0] && bitfield[1]))
			break;
	}

#undef PARSE_HEX
}

static void
parse_peer_bitfield(Peer *p, char const *bitfield)
{
	static uint8_t const HEX_POPCOUNT[] = {
		0, 1, 1, 2, 1, 2, 2, 3,
		1, 2, 2, 3, 2, 3, 3, 4,
	};

	uint32_t pieces_have = 0;

	for (char const *hex = bitfield; *hex; ++hex)
		pieces_have += HEX_POPCOUNT[*hex <= '9' ? *hex - '0' : *hex - 'a' + 10];

	p->pieces_have = pieces_have;
}

static void
parse_peer(Download *d, Peer *p, JSONNode const *node)
{
	JSONNode const *field = json_children(node);

	p->progress = NULL;

	do {
		switch (K_peer_parse(field->key)) {
		case K_peer_none:
			/* ignore */
			break;

		case K_peer_peerId:
			parse_peer_id(p, field->str);
			break;

		case K_peer_ip:
			strncpy(p->ip, field->str, sizeof p->ip - 1);
			break;

		case K_peer_port:
			p->port = strtoul(field->str, NULL, 10);
			break;

		case K_peer_bitfield:
			parse_peer_bitfield(p, field->str);
			parse_progress(d, &p->progress, field->str);
			break;

		case K_peer_amChoking:
			p->up_choked = IS_TRUE(field->str);
			break;

		case K_peer_peerChoking:
			p->down_choked = IS_TRUE(field->str);
			break;

		case K_peer_downloadSpeed:
			p->download_speed = strtoull(field->str, NULL, 10);
			break;

		case K_peer_uploadSpeed:
			p->upload_speed = strtoull(field->str, NULL, 10);
			break;
		}
	} while ((field = json_next(field)));
}

static void
parse_peers(Download *d, JSONNode const *node)
{
	uint32_t num_oldpeers = d->num_peers;
	Peer *oldpeers = d->peers;

	d->num_peers = json_length(node);
	if (!(d->peers = malloc(d->num_peers * sizeof *(d->peers))))
		goto free_oldpeers;

	if (!json_length(node))
		goto free_oldpeers;

	struct timespec now;
	clock_gettime(
#ifdef CLOCK_MONOTONIC_COARSE
		CLOCK_MONOTONIC_COARSE,
#else
		CLOCK_MONOTONIC,
#endif
		&now
	);

	Peer *p = d->peers;
	node = json_children(node);
	do {
		Peer *oldp;
		uint32_t j;

		parse_peer(d, p, node);

		/* find new peer among previous ones to being able to compute
		 * its progress/speed change */
		for (j = 0; j < num_oldpeers; ++j) {
			oldp = &oldpeers[j];
			if (!memcmp(p->peer_id, oldp->peer_id, sizeof p->peer_id) &&
			    p->port == oldp->port &&
			    !strcmp(p->ip, oldp->ip))
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

			uint64_t pieces_change =
				p->pieces_have != oldp->pieces_have
					? p->pieces_have - oldp->pieces_have
					: 1;
			uint64_t bytes_change = pieces_change * d->piece_size;
			uint64_t time_ns_change =
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
	} while (++p, (node = json_next(node)));

free_oldpeers:
	free_peers(oldpeers, num_oldpeers);
}

static void
parse_servers(Download *d, JSONNode const *node)
{
	if (!d->files)
		return;

	/* reset servers for every uri */
	for (uint32_t i = 0; i < d->num_files; ++i) {
		File *f = &d->files[i];

		for (uint32_t j = 0; j < f->num_uris; ++j) {
			URI *u = &f->uris[j];
			u->num_servers = 0;
		}
	}

	for (node = json_first(node); node; node = json_next(node)) {
		JSONNode const *field = json_children(node);
		JSONNode const *servers = servers;
		uint32_t file_index = file_index;

		do {
			if (!strcmp(field->key, "index"))
				file_index = atoi(field->str) - 1;
			else if (!strcmp(field->key, "servers"))
				servers = field;
		} while ((field = json_next(field)));

		assert(file_index < d->num_files);
		parse_file_servers(&d->files[file_index], servers);
	}

	/* now deallocate server lists where num_servers == 0 */
	for (uint32_t i = 0; i < d->num_files; ++i) {
		File *f = &d->files[i];

		for (uint32_t j = 0; j < f->num_uris; ++j) {
			URI *u = &f->uris[j];
			if (!u->num_servers)
				free(u->servers), u->servers = NULL;
		}
	}

}

static void
update_display_name(Download *d)
{
	if (d->name) {
		d->display_name = d->name;
		return;
	}

	if (d->num_files > 0 && d->files) {
		File *f = &d->files[0];

		if (f->path) {
			d->display_name = f->path;
			return;
		}

		if (f->num_uris > 0 && f->uris) {
			for (uint32_t i = 0; i < f->num_uris; ++i) {
				if (f->uris[i].status == URI_STATUS_USED) {
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
parse_download_files(Download *d, JSONNode const *node)
{
	free_files_of(d);

	d->num_files = json_length(node);
	d->num_files_selected = 0;
	if (!(d->files = malloc(d->num_files * sizeof *(d->files))))
		return;

	for (node = json_first(node); node; node = json_next(node)) {
		JSONNode const *field = json_children(node);
		File file;
		int index = -1;

		file.num_uris = 0;
		file.uris = NULL;

		do {
			switch (K_file_parse(field->key)) {
			case K_file_none:
				/* ignore */
				break;

			case K_file_index:
				index = atoi(field->str) - 1;
				break;

			case K_file_path:
				file.path = strlen(field->str) > 0 ? strdup(field->str) : NULL;
				break;

			case K_file_length:
				file.total = strtoull(field->str, NULL, 10);
				break;

			case K_file_completedLength:
				file.have = strtoull(field->str, NULL, 10);
				break;

			case K_file_selected:
				file.selected = IS_TRUE(field->str);
				d->num_files_selected += file.selected;
				break;

			case K_file_uris:
			{
				JSONNode const *uris;
				uint32_t uri_index = 0;

				file.num_uris = json_length(field);
				file.uris = malloc(file.num_uris * sizeof *(file.uris));
				if (!file.uris)
					abort();

				for (uris = json_first(field); uris; uris = json_next(uris)) {
					JSONNode const *field = json_children(uris);
					URI u;

					u.num_servers = 0;
					u.servers = NULL;
					do {
						if (!strcmp(field->key, "status")) {
							if (!strcmp(field->str, "used"))
								u.status = URI_STATUS_USED;
							else if (!strcmp(field->str, "waiting"))
								u.status = URI_STATUS_WAITING;
							else
								assert(0);
						} else if (!strcmp(field->key, "uri")) {
							assert(field->str);
							u.uri = strdup(field->str);
						}
					} while ((field = json_next(field)));
					file.uris[uri_index++] = u;
				}
			}
				break;
			}
		} while ((field = json_next(field)));

		assert(index >= 0);
		d->files[index] = file;
	}

	update_download_tags(d);
	update_display_name(d);
}

static bool
is_gid(char const *str)
{
	for (uint8_t i = 0; i < 16; ++i)
		if (!islxdigit(str[i]))
			return false;

	return !!str[16];
}

/* Decide whether download |d| (:= *|dd|) should be shown. If not, remove |d|
 * and return false, otherwise true. */
static bool
filter_download(Download *d, Download **dd)
{
	/* No --selects mean all downloads. */
	if (!num_selects)
		return true;
	else if (num_selects == SIZE_MAX)
		return true;

	for (size_t i = 0; i < num_selects; ++i) {
		char const *sel = selects[i];

		if (is_gid(sel)) {
			/* Test for matching GID. */
			if (!memcmp(sel, d->gid, sizeof d->gid))
				return true;
		} else {
			/* Test for matching path prefix. */
			size_t sel_size = strlen(sel);

			/* Missing data can be anything, do not remove yet. */
			if (!d->num_files || !d->files)
				return true;

			for (uint32_t j = 0; j < d->num_files; ++j) {
				File const *f = &d->files[j];

				if (f->path && !strncmp(f->path, sel, sel_size))
					return true;
			}
		}
	}

	if (!dd)
		(void)upgrade_download(d, &dd);
	delete_download_at(dd);
	return false;
}

static void
download_changed(Download *d)
{
	update_display_name(d);
	filter_download(d, NULL);
}

static void
parse_download(Download *d, JSONNode const *node)
{
	/* only present if non-zero */
	d->verified = 0;

	for (JSONNode const *field = json_first(node);
	     field;
	     field = json_next(field))
	{
		switch (K_download_parse(field->key)) {
		case K_download_gid:
			assert(strlen(field->str) == sizeof d->gid - 1);
			memcpy(d->gid, field->str, sizeof d->gid);
			break;

		case K_download_files:
			parse_download_files(d, field);
			break;

		case K_download_numPieces:
			d->num_pieces = strtoul(field->str, NULL, 10);
			break;

		case K_download_pieceLength:
			d->piece_size = strtoul(field->str, NULL, 10);
			break;

		case K_download_bitfield:
			if (d->have != d->total)
				parse_progress(d, &d->progress, field->str);
			else
				free(d->progress), d->progress = NULL;
			break;

		case K_download_bittorrent:
		{
			JSONNode const *bt_info, *bt_name;

			free(d->name), d->name = NULL;

			if ((bt_info = json_get(field, "info")) &&
			    (bt_name = json_get(bt_info, "name")))
				d->name = strdup(bt_name->str);
		}
			break;

		case K_download_status:
		{
			static int8_t const STATUS_MAP[] = {
				[K_status_none    ] = D_UNKNOWN,
				[K_status_active  ] = D_ACTIVE,
				[K_status_waiting ] = D_WAITING,
				[K_status_paused  ] = D_PAUSED,
				[K_status_error   ] = D_ERROR,
				[K_status_complete] = D_COMPLETE,
				[K_status_removed ] = D_REMOVED
			};

			int8_t new_status = STATUS_MAP[K_status_parse(field->str)];

			if (new_status != d->status) {
				if (D_WAITING == abs(d->status))
					queue_changed();

				--global.num_perstatus[abs(d->status)];
				d->status = -new_status;
				++global.num_perstatus[new_status];
			}
		}
			break;

		case K_download_completedLength:
			global.have_total -= d->have;
			d->have = strtoull(field->str, NULL, 10);
			global.have_total += d->have;
			break;

		case K_download_uploadLength:
			global.uploaded_total -= d->uploaded;
			d->uploaded = strtoull(field->str, NULL, 10);
			global.uploaded_total += d->uploaded;
			break;

		case K_download_following:
			/* XXX: We assume that a download cannot follow and
			 * belonging to a different download... */
			d->follows = true;
			/* FALLTHROUGH */
		case K_download_belongsTo:
		{
			Download **dd = get_download_by_gid(field->str);

			if (dd) {
				Download *p = *dd;

				d->parent = p;
				if (!p->first_child) {
					p->first_child = d;
				} else {
					p = p->first_child;
					while (p->next_sibling)
						p = p->next_sibling;
					p->next_sibling = d;
				}
			}
			break;
		}

		case K_download_errorMessage:
			free(d->error_msg);
			d->error_msg = strdup(field->str);
			break;

		case K_download_downloadSpeed:
			global.download_speed_total -= d->download_speed;
			d->download_speed = strtoul(field->str, NULL, 10);
			global.download_speed_total += d->download_speed;
			break;

		case K_download_uploadSpeed:
			global.upload_speed_total -= d->upload_speed;
			d->upload_speed = strtoul(field->str, NULL, 10);
			global.upload_speed_total += d->upload_speed;
			break;

		case K_download_totalLength:
			d->total = strtoull(field->str, NULL, 10);
			break;

		case K_download_connections:
			d->num_connections = strtoul(field->str, NULL, 10);
			break;

		case K_download_verifiedLength:
			d->verified = strtoul(field->str, NULL, 10);
			break;

		case K_download_verifyIntegrityPending:
			d->verified = UINT64_MAX;
			break;

		case K_download_none:
			/* ignore */
			break;
		}
	}
}

static void
parse_option(char const *option, char const *value, Download *d)
{
	if (d) {
		static locale_t cloc = (locale_t)0;
		locale_t origloc;

		if ((locale_t)0 == cloc)
			cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);

		switch (K_option_parse(option)) {
		default:
			/* Ignore. */
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

		case K_option_bt__external__ip:
			strncpy(global.external_ip, value, sizeof global.external_ip);
			break;

		case K_option_max__overall__download__limit:
			global.download_speed_limit = atol(value);
			break;

		case K_option_max__overall__upload__limit:
			global.upload_speed_limit = atol(value);
			break;

		case K_option_dir:
			is_local = !access(value, R_OK | W_OK | X_OK);
			break;
		}
	}
}

static void
parse_options(JSONNode const *options, void *arg)
{
	for (JSONNode const *option = json_first(options);
	     option;
	     option = json_next(option))
		parse_option(option->key, option->str, arg);
}

static void
parse_global_stat(JSONNode const *node)
{
	if (JSONT_ARRAY != json_type(node))
		return;

	node = json_children(node);
	node = json_children(node);
	do {
		if (!strcmp(node->key, "downloadSpeed"))
			global.download_speed = strtoull(node->str, NULL, 10);
		else if (!strcmp(node->key, "uploadSpeed"))
			global.upload_speed = strtoull(node->str, NULL, 10);
	} while ((node = json_next(node)));
}

static void
try_connect(void)
{
	ws_open(remote_host, remote_port);
}

typedef struct {
	Download *download;
	bool has_get_peers: 1;
	bool has_get_servers: 1;
	bool has_get_options: 1;
	bool has_get_position: 1;
} UpdateArg;

static void
parse_downloads(JSONNode const *result, UpdateArg *arg)
{
	bool some_insufficient = false;

	for (Download *d;
	     result;
	     (result = json_next(result)), unref_download(d), ++arg)
	{
		JSONNode const *node;

		d = arg->download;

		if (!upgrade_download(d, NULL)) {
		skip:
			if (arg->has_get_peers || arg->has_get_servers)
				result = json_next(result);
			if (arg->has_get_options)
				result = json_next(result);
			if (arg->has_get_position)
				result = json_next(result);
			continue;
		}

		if (JSONT_OBJECT == json_type(result)) {
			Download **dd;

			/* Checked previously. */
			(void)upgrade_download(d, &dd);
			delete_download_at(dd);
			goto skip;
		}

		node = json_children(result);
		parse_download(d, node);

		if (arg->has_get_peers || arg->has_get_servers) {
			result = json_next(result);
			if (JSONT_OBJECT == json_type(result)) {
				handle_error(result);
			} else {
				node = json_children(result);
				(arg->has_get_peers ? parse_peers : parse_servers)(d, node);
			}
		}

		if (arg->has_get_options) {
			result = json_next(result);
			if (JSONT_OBJECT == json_type(result)) {
				handle_error(result);
			} else {
				node = json_children(result);
				parse_options(node, d);
			}
		}

		if (arg->has_get_position) {
			result = json_next(result);
			if (JSONT_OBJECT == json_type(result)) {
				handle_error(result);
			} else {
				node = json_children(result);
				d->queue_index = node->num;
			}
		}

		some_insufficient |= download_insufficient(d);

		download_changed(d);
	}

	if (some_insufficient)
		update_downloads();
}

static void
arm_periodic_update_timer(void)
{
	int any_activity =
		0 < global.download_speed ||
		0 < global.upload_speed;

	on_timeout = update_downloads;
	interval = (struct timespec){
		.tv_sec = any_activity ? 1 : 3,
		.tv_nsec = 271 * 1000000,
	};
}

static void
handle_update(JSONNode const *result, UpdateArg *arg)
{
	if (!result)
		goto out;

	result = json_children(result);
	parse_global_stat(result);

	result = json_next(result);
	if (result) {
		parse_downloads(result, arg);
		on_downloads_change(true);
	} else {
		draw_statusline();
	}

	refresh();

out:
	arm_periodic_update_timer();

	free(arg);
}

static int
getmainheight(void)
{
	return LINES - 1 /* Status line. */;
}

static void
update_downloads(void)
{
	/* Request update only if it was scheduled. */
	if (update_downloads != on_timeout)
		return;

	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)handle_update;

	begin_rpc_multicall();

	generate_rpc_multicall_method("aria2.getGlobalStat", NULL);

	if (0 < num_downloads) {
		Download **top;
		Download **bot;
		Download **end = &downloads[num_downloads];
		Download **dd = downloads;
		UpdateArg *arg;

		if (VIEWS[1] == view) {
			top = &downloads[topidx];
			bot = top + getmainheight() - 1;
			if (end <= bot)
				bot = end - 1;
		} else {
			top = &downloads[selidx];
			bot = top;
		}

		if (!(arg = malloc(num_downloads * sizeof *arg))) {
			free_rpc(rpc);
			return;
		}
		rpc->arg = arg;

		for (; dd < end; ++dd) {
			Download *d = *dd;
			bool visible = top <= dd && dd <= bot;
			uint8_t num_keys = 0;
			char *keys[20];

#define WANT(key) keys[num_keys++] = key;

			if (!d->initialized) {
				WANT("bittorrent");
				WANT("numPieces");
				WANT("pieceLength");

				WANT("following");
				WANT("belongsTo");

				d->initialized = true;
			} else {
				if ((!d->num_files && (!d->name || (visible && D_ACTIVE != d->status && is_local))) ||
				    (visible && 'f' == view))
				{
					WANT("files");
					if (is_local)
						WANT("bitfield");
				} else if (visible && 'p' == view && is_local) {
					WANT("bitfield");
				}
			}

			if (d->status < 0) {
				WANT("totalLength");
				WANT("completedLength");
				WANT("downloadSpeed");
				WANT("uploadLength");
				WANT("uploadSpeed");
				if (is_local)
					WANT("bitfield");

				if (-D_ERROR == d->status)
					WANT("errorMessage");
			} else if (D_UNKNOWN == d->status) {
				WANT("status");
			} else if (D_ACTIVE == d->status) {
				if (0 < d->download_speed ||
				    (global.download_speed != global.download_speed_total &&
				     d->have != d->total))
					WANT("downloadSpeed");

				if (0 < d->download_speed) {
					WANT("completedLength");
				}

				if (0 < d->upload_speed ||
				    global.upload_speed != global.upload_speed_total)
					WANT("uploadSpeed");

				if (0 < d->upload_speed)
					WANT("uploadLength");

				if (visible) {
					if (!d->upload_speed && !d->download_speed) {
						WANT("verifiedLength");
						WANT("verifyIntegrityPending");
					}

					WANT("connections");
				}
			}

#undef WANT

			if (!num_keys)
				continue;

			begin_rpc_multicall_method("aria2.tellStatus", d);
			/* "keys" */
			json_write_beginarr(jw);
			while (0 < num_keys)
				json_write_str(jw, keys[--num_keys]);
			json_write_endarr(jw);
			end_rpc_multicall_method();

			arg->download = ref_download(d);

			arg->has_get_peers = 0;
			arg->has_get_servers = 0;
			if (visible &&
			    (D_ACTIVE == abs(d->status) || d->status < 0) &&
			    (d->name
			    ? (arg->has_get_peers = ('p' == view))
			    : (arg->has_get_servers = ('f' == view))))
				generate_rpc_multicall_method(d->name ? "aria2.getPeers" : "aria2.getServers", d);

			if ((arg->has_get_options = (d->status < 0 || (!d->requested_options && visible)))) {
				generate_rpc_multicall_method("aria2.getOption", d);
				d->requested_options = true;
			}

			if ((arg->has_get_position = (-D_WAITING == d->status))) {
				begin_rpc_multicall_method("aria2.changePosition", d);
				/* "pos" */
				json_write_num(jw, 0);
				/* "how" */
				json_write_str(jw, "POS_CUR");
				end_rpc_multicall_method();
			}

			d->status = abs(d->status);
			++arg;
		}
	} else {
		rpc->arg = NULL;
	}

	end_rpc_multicall();

	if (do_rpc(rpc))
		on_timeout = NULL;
}


static void
handle_position_change(JSONNode const *result, Download *d)
{
	if (result) {
		queue_changed();
		update_downloads();

		d->queue_index = result->num;
		on_downloads_change(true);
		refresh();
	}

	unref_download(d);
}

static void
change_position(Download *d, int32_t pos, int whence)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)handle_position_change;
	rpc->arg = ref_download(d);

	begin_rpc_method("aria2.changePosition", d);
	/* "pos" */
	json_write_int(jw, pos);
	/* "how" */
	char const *how;
	switch (whence) {
	case SEEK_SET: how = "POS_SET"; break;
	case SEEK_CUR: how = "POS_CUR"; break;
	case SEEK_END: how = "POS_END"; break;
	default: abort();
	};
	json_write_str(jw, how);
	end_rpc_method();

	do_rpc(rpc);
}

static void
handle_shutdown(JSONNode const *result, UpdateArg *arg)
{
	(void)arg;

	if (!result)
		return;

	exit(EXIT_SUCCESS);
}

static Selection *
get_selection(bool all)
{
	Download **dd, **end;
	if (all || !num_downloads) {
		dd = downloads;
		end = &downloads[num_downloads];
	} else {
		dd = &downloads[selidx];
		end = dd + 1;
	}

	size_t count = (size_t)(end - dd);

	Selection *s;
	if (!(s = malloc(offsetof(Selection, downloads[count]))))
		abort();

	s->count = count;

	for (size_t i = 0; i < count; ++i) {
		Download *d = dd[i];
		s->downloads[i] = ref_download(d);
	}

	return s;
}

static void
generate_rpc_for_selection(RPCRequest *rpc, Selection *s, char const *method)
{
	rpc->arg = s;

	begin_rpc_multicall();

	for (size_t i = 0; i < s->count; ++i) {
		Download *d = s->downloads[i];

		generate_rpc_multicall_method(method, d);
	}

	end_rpc_multicall();

}

static void
generate_rpc_for_selected(RPCRequest *rpc, bool all, char const *method)
{
	Selection *s = get_selection(all);
	generate_rpc_for_selection(rpc, s, method);
}

static void
pause_download(bool all, bool pause)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	if (!num_selects && all)
		generate_rpc_method(pause
				? (do_forced ? "aria2.forcePauseAll" : "aria2.pauseAll")
				: "aria2.unpauseAll");
	else
		generate_rpc_for_selected(rpc, all, pause
				? (do_forced ? "aria2.forcePause" : "aria2.pause")
				: "aria2.unpause");

	do_rpc(rpc);
}

static void
free_selection(Selection *s)
{
	if (!s)
		return;

	for (size_t i = 0; i < s->count; ++i)
		unref_download(s->downloads[i]);
	free(s);
}

static void
handle_download_purge(JSONNode const *result, Selection *s)
{
	if (result) {
		for (size_t i = 0; i < s->count; ++i) {
			Download *d = s->downloads[i], **dd;
			if (upgrade_download(d, &dd))
				switch (abs((*dd)->status)) {
				case D_COMPLETE:
				case D_ERROR:
				case D_REMOVED:
					delete_download_at(dd);
					break;
				}
		}

		on_downloads_change(true);
		refresh();
	}

	free_selection(s);
}

static void
purge_download(bool all)
{
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)handle_download_purge;
	rpc->arg = get_selection(all);

	if (!num_selects && all)
		generate_rpc_method("aria2.purgeDownloadResult");
	else
		generate_rpc_for_selected(rpc, all, "aria2.removeDownloadResult");

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
draw_peer(Download const *d, size_t i, int *y)
{
	Peer const *p = &d->peers[i];
	char fmtbuf[5];
	int n;
	int x, width = COLS;

	attr_set(A_NORMAL, 0, NULL);
	mvprintw(*y, 0, "  %s:%-5u", p->ip, p->port);
	clrtoeol();

	x = PEER_ADDRESS_WIDTH;
	move(*y, x + PEER_INFO_WIDTH <= width ? x : width - PEER_INFO_WIDTH);

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

	draw_progress(d, p->progress, 0, d->total);

	++*y;
}

static void
draw_our_peer(Download const *d, int *y)
{
	mvprintw((*y)++, 0, "  %*.*s%*.s", -sizeof global.external_ip, sizeof global.external_ip, global.external_ip, 6 + PEER_INFO_WIDTH, "");
#if 0
	Download D = {
		.num_pieces = 8,
		.piece_size = 1000
	};
	move(getcury(stdscr), COLS - 16 - 3);
	draw_progress(&D, (uint8_t *)"\x82", 0, 8000);
#endif
	draw_progress(d, d->progress, 0, d->have < d->total ? d->total : 0);
}

static void
draw_peers(void)
{
	if (0 < num_downloads) {
		Download *d = downloads[selidx];
		int y = 0;

		draw_download(d, false, &y);

		draw_our_peer(d, &y);

		if (d->peers) {
			for (uint32_t i = 0; i < d->num_peers; ++i)
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

static char const *
get_file_display_name(Download const *d, File const *f)
{
	char const *ret;

	if (f->path) {
		ret = f->path;
		if (d->dir) {
			size_t dir_size = strlen(d->dir);

			if (!strncmp(f->path, d->dir, dir_size) &&
			    '/' == f->path[dir_size])
			{
				ret = f->path + dir_size + 1;
				/* Leading slash is misleading. */
				while ('/' == *ret)
					++ret;
			}
		}
	} else {
		ret = f->selected ? "(not downloaded yet)" : "(none)";
	}
	return ret;
}

static void
draw_file(Download const *d, uint32_t i, int *y, uint64_t offset)
{
	File const *f = &d->files[i];
	char szhave[6];
	char sztotal[6];
	char szpercent[6];
	uint32_t j;

	szhave[fmt_space(szhave, f->have)] = '\0';
	sztotal[fmt_space(sztotal, f->total)] = '\0';
	szpercent[fmt_percent(szpercent, f->have, f->total)] = '\0';

	attr_set(A_BOLD, 0, NULL);
	mvprintw((*y)++, 0, "  %6"PRIu32": ", i + 1);

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

	char const *name = get_file_display_name(d, f);
	printw("] %s", name);
	clrtoeol();

	if (f->total != f->have && 0 < f->have && d->progress) {
		mvprintw((*y)++, 0, "%*.s", 6 + 3, "");
		draw_progress(d, d->progress, offset, offset + f->total);
	}

	for (j = 0; j < f->num_uris; ++j) {
		URI const *u = &f->uris[j];

		attr_set(A_NORMAL, 0, NULL);
		mvprintw((*y)++, 0, "       %s╴",
		         j + 1 < f->num_uris ? "├" : "└");

		attr_set(u->status == URI_STATUS_USED ? A_BOLD : A_NORMAL, 0, NULL);
		printw("%3d%s ", j + 1, u->status == URI_STATUS_USED ? "*" : " ");
		attr_set(A_NORMAL, 0, NULL);
		addstr(u->uri);
		clrtoeol();

		for (uint32_t k = 0; k < u->num_servers; ++k) {
			Server const *s = &u->servers[k];
			char fmtbuf[5];
			int n;

			mvprintw((*y)++, 0, "       %s   ↓  ",
			         j + 1 < f->num_uris ? "│" : " ");

			attr_set(A_BOLD, COLOR_DOWN, NULL);
			n = fmt_speed(fmtbuf, s->download_speed);
			addnstr(fmtbuf, n);
			attr_set(A_NORMAL, 0, NULL);

			if (s->current_uri)
				printw(" ↪ %s", s->current_uri);

			clrtoeol();
		}
	}
}

static void
draw_files(void)
{
	if (0 < num_downloads) {
		Download *d = downloads[selidx];
		int y = 0;

		draw_download(d, false, &y);
		if (d->dir) {
			mvprintw(y++, 0, "  %s", d->dir);
			clrtoeol();
		}

		if (0 < d->num_files) {
			uint64_t offset = 0;

			for (uint32_t i = 0; i < d->num_files; ++i) {
				draw_file(d, i, &y, offset);
				offset += d->files[i].total;
			}

			/* linewrap */
			move(y, 0);
		} else {
			update_downloads();
		}

	} else {
		move(0, 0);
	}
	clrtobot();
	move(0, curx);
}

static int
downloadcmp(Download const **pthis, Download const **pother, void *arg);

/* XXX: Cache value? */
static Download const *
downloadtreebest(Download const *tree)
{
	Download const *best = tree;
	Download const *sibling;

	for (sibling = best->first_child;
	     sibling;
	     sibling = sibling->next_sibling) {
		Download const *treebest = downloadtreebest(sibling);
		if (downloadcmp(&best, &treebest, (void *)1) > 0)
			best = treebest;
	}

	return best;
}

static int
downloadcmp(Download const **pthis, Download const **pother, void *arg)
{
	int cmp;
	uint64_t x, y;
	Download const *this = *pthis;
	Download const *other = *pother;
	int8_t this_status, other_status;

	if (!arg) {
	/* if ((this_status == D_PAUSED || other_status == D_PAUSED) &&
	    (this_status == D_COMPLETE || other_status == D_COMPLETE))
		__asm__("int3"); */
		for (;; this = this->parent) {
			Download const *p = other;

			do {
				if (p == this->parent)
					return 1;
				else if (this == p->parent)
					return -1;
				else if (this->parent == p->parent) {
					other = p;
					goto compare_siblings;
				}
			} while ((p = p->parent));
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

	if (D_WAITING == this_status)
		/* sort by position in queue */
		return other->queue_index - this->queue_index;

	if (D_ACTIVE == this_status) {
		/* prefer not stalled downloads */
		cmp = (0 < other->download_speed) - (0 < this->download_speed);
		if (cmp)
			return cmp;
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

	if (D_ACTIVE == this_status) {
		/* prefer higher upload speed */
		if (this->upload_speed != other->upload_speed)
			return this->upload_speed > other->upload_speed ? -1 : 1;
	}

#if 0
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
#endif

	/* sort by name */
	return strcoll(this->display_name, other->display_name);
}

/* draw download d at screen line y */
static void
draw_download(Download const *d, bool draw_parents, int *y)
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
	case D_UNKNOWN:
		addstr("? ");
		goto fill_spaces;

	case D_REMOVED:
		addstr("- ");

	fill_spaces:
		attr_set(A_NORMAL, 0, NULL);
		addspaces(29 +
			(0 < global.num_download_limited ? 5 : 0) +
			(0 < global.num_upload_limited ? 6 : 0));
		break;

	case D_ERROR:
	{
		int usable_width =
			29 +
			(!d->tags ? tag_col_width : 0) +
			(0 < global.num_download_limited ? 5 : 0) +
			(0 < global.num_upload_limited ? 6 : 0);

		addstr("* ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		printw("%-*.*s", usable_width, usable_width, d->error_msg && (strlen(d->error_msg) <= 30 || VIEWS[1] == view) ? d->error_msg : "");
		attr_set(A_NORMAL, 0, NULL);

		if (!d->tags)
			goto skip_tags;
	}
		break;

	case D_WAITING:
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

	case D_PAUSED:
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
			if (d->num_files != d->num_files_selected) {
				n = fmt_number(fmtbuf, d->num_files_selected);
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

	case D_COMPLETE:
		attr_set(A_NORMAL, 0, NULL);

		addspaces(10);

		n = fmt_space(fmtbuf, d->total);
		addnstr(fmtbuf, n);

		addstr(",      ");

		if (0 < global.num_download_limited)
			addspaces(5);

		n = fmt_number(fmtbuf, d->num_files_selected);
		addnstr(fmtbuf, n);
		addstr(1 == d->num_files_selected ? " file " : " files");
		if (0 < global.num_upload_limited)
			addspaces(6);
		break;

	case D_ACTIVE:
		addstr(d->verified == 0 ? "> " : "v ");
		attr_set(A_NORMAL, 0, NULL);

		if (!d->verified) {
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

	if (d->tags) {
		int oy, ox, ny, nx, mx;

		getyx(stdscr, oy, ox);

		addstr(" ");
		addstr(d->tags);

		mx = LINES;
		getyx(stdscr, ny, nx);

		tagwidth = (ny - oy) * mx + (nx - ox);
		if (tag_col_width < tagwidth) {
			longest_tag = d; /*no ref*/
			tag_col_width = tagwidth;
			downloads_need_reflow = true;
		}
	} else {
		tagwidth = 0;
	}
	if (0 < tag_col_width)
		printw("%*.s", tag_col_width - tagwidth, "");

skip_tags:
	addstr(" ");
	namex = x = curx = getcurx(stdscr);
	if (draw_parents && d->parent) {
		Download const *parent;

		for (parent = d; parent->parent; parent = parent->parent)
			x += 2;
		x += 1;
		namex = x;

		if (d->follows)
			mvaddstr(*y, x -= 3, "⮡  ");
		else
			mvaddstr(*y, x -= 3, d->next_sibling ? "├─ " : "└─ ");

		for (parent = d->parent; parent; parent = parent->parent)
			mvaddstr(*y, x -= 2, d->next_sibling ? "│ " : "  ");
	}

	mvaddstr(*y, namex, d->display_name);
	clrtoeol();
	++*y;

	switch (abs(d->status)) {
	case D_ERROR:
		if (view != VIEWS[1] && d->error_msg && strlen(d->error_msg) > 30) {
			attr_set(A_BOLD, COLOR_ERR, NULL);
			mvaddstr(*y, 0, "  ");
			addstr(d->error_msg);
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
		downloads_need_reflow = false;
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
on_downloads_change(bool stickycurs)
{
	/* re-sort downloads list */
	if (0 < num_downloads) {
		char selgid[sizeof ((Download *)0)->gid];

		if (selidx < 0)
			selidx = 0;

		if (num_downloads <= (size_t)selidx)
			selidx = num_downloads - 1;

		memcpy(selgid, downloads[selidx]->gid, sizeof selgid);

		qsort_r(downloads, num_downloads, sizeof *downloads, (int(*)(void const *, void const *, void*))downloadcmp, NULL);

		/* move selection if download moved */
		if (stickycurs && memcmp(selgid, downloads[selidx]->gid, sizeof selgid)) {
			oldidx = selidx;
			selidx = get_download_by_gid(selgid) - downloads;
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
	int height = VIEWS[1] == view ? getmainheight() : 1;

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

	if (tui.topidx == topidx) {
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

	if (tui.topidx == topidx) {
		move(VIEWS[1] == view ? selidx - topidx : 0, curx);
	} else {
		tui.topidx = topidx;
		on_scroll_changed();
		/* update now seen downloads */
		update_downloads();
	}

	if (tui.selidx != selidx) {
		tui.selidx = selidx;
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

	if (!ti.tsl || !ti.fsl)
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

	y = LINES, w = COLS;

	/* print downloads info at the left */
	--y, x = 0;

	move(y, 0);
	attr_set(A_NORMAL, 0, NULL);

	printw("%c%c %4d/%d",
			view,
			num_selects ? '=' : '\0',
			0 < num_downloads ? selidx + 1 : 0, num_downloads);

	for (i = D_UNKNOWN; i < D_NB; ++i) {
		if (!global.num_perstatus[i])
			continue;

		addstr(first ? " (" : " ");
		first = false;

		attr_set(A_BOLD, i == D_ERROR ? COLOR_ERR : 0, NULL);
		addnstr(DOWNLOAD_SYMBOLS + i, 1);
		attr_set(A_NORMAL, i == D_ERROR ? COLOR_ERR : 0, NULL);
		printw("%d", global.num_perstatus[i]);
		attr_set(A_NORMAL, 0, NULL);
	}

	if (!first)
		addstr(")");

	printw(" @ %s:%u%s",
			remote_host, remote_port,
			ws_is_alive() ? "" : " (not connected)");

	if (error_msg) {
		addstr(": ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		addstr(error_msg);
		attr_set(A_NORMAL, 0, NULL);
	}

	clrtoeol();

	/* build bottom-right widgets from right-to-left */
	x = w;
	mvaddstr(y, x -= 1, " ");

	if (!global.uploaded_total)
		goto skip_upload;

	/* upload */
	if (0 < global.upload_speed_limit) {
		n = fmt_speed(fmtbuf, global.upload_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	speed = !num_selects ? global.upload_speed : global.upload_speed_total;
	if (0 < speed)
		attr_set(A_BOLD, COLOR_UP, NULL);
	n = fmt_speed(fmtbuf, speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	attr_set(A_NORMAL, 0, NULL);
	mvaddstr(y, x -= 4, "] @ ");
	if (0 < speed)
		attr_set(A_NORMAL, COLOR_UP, NULL);
	n = fmt_percent(fmtbuf, global.uploaded_total, global.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);
	attr_set(A_NORMAL, 0, NULL);
	mvaddstr(y, x -= 1, "[");

	if (0 < speed)
		attr_set(A_BOLD, COLOR_UP, NULL);
	n = fmt_space(fmtbuf, global.uploaded_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	if (0 < speed)
		attr_set(A_BOLD, COLOR_UP, NULL);
	mvaddstr(y, x -= 3, " ↑ ");
	attr_set(A_NORMAL, 0, NULL);
skip_upload:

	/* download */
	if (0 < global.download_speed_limit) {
		n = fmt_speed(fmtbuf, global.download_speed_limit);
		mvaddnstr(y, x -= n, fmtbuf, n);
		mvaddstr(y, x -= 1, "/");
	}

	speed = !num_selects ? global.download_speed : global.download_speed_total;
	if (0 < speed)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
	n = fmt_speed(fmtbuf, speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	attr_set(A_NORMAL, 0, NULL);
	mvaddstr(y, x -= 3, " @ ");

	if (0 < speed)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
	n = fmt_space(fmtbuf, global.have_total);
	mvaddnstr(y, x -= n, fmtbuf, n);

	if (0 < speed)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
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
handle_update_all(JSONNode const *result, void *arg)
{
	size_t download_index = 0;

	(void)arg;

	if (!result)
		return;

	clear_downloads();

	result = json_children(result);
	parse_global_stat(result);

	result = json_next(result);
	do {
		JSONNode const *downloads_list;
		JSONNode const *node;

		if (JSONT_ARRAY != json_type(result)) {
			handle_error(result);
			continue;
		}

		downloads_list = json_children(result);

		for (node = json_first(downloads_list);
		     node;
		     node = json_next(node))
		{
			Download *d;
			/* XXX: parse_download's belongsTo and followedBy may
			 * create the download before this update arrives, so
			 * we check if only we modified the list */
			Download **dd = download_index == num_downloads
				? new_download()
				: get_download_by_gid(json_get(node, "gid")->str);
			if (!dd)
				continue;
			d = *dd;

			/* we did it initially but we have to mark it now since
			 * we only received download object now */
			d->initialized = true;

			parse_download(d, node);

			download_changed(d);
			++download_index;
		}

	} while ((result = json_next(result)));

	on_downloads_change(false);
	refresh();

	arm_periodic_update_timer();
	update_downloads();
}

static void
runaction_maychanged(Download *d)
{
	if (d) {
		update_download_tags(d);
		draw_all();
	}
}

/* revive ncurses */
static void
begwin(void)
{
	/* heh. this shit gets forgotten. */
	keypad(stdscr, TRUE);

	refresh();

	/* endwin() + refresh() fuckery does not work. crying. */
	struct winsize w;
	if (!ioctl(STDERR_FILENO, TIOCGWINSZ, &w))
		resizeterm(w.ws_row, w.ws_col);

	/* *slap* */
	refresh();
}

/* Returns:
 * - <0: mode did not run.
 * - =0: mode executed and terminated successfully.
 * - >0: mode executed but failed. */
static int
xwaitpid(pid_t pid)
{
	for (;;) {
		int status;
		if (waitpid(pid, &status, 0) < 0) {
			if (EINTR == errno)
				continue;

			return 1;
		}

		if (WIFEXITED(status) && 127 == WEXITSTATUS(status))
			return -1;
		else if (WIFEXITED(status) && EXIT_SUCCESS == WEXITSTATUS(status))
			return 0;
		else
			return 1;
	}
}

static int
run_action(Download *d, char const *name, ...)
{
	clear_error();

	char filename[NAME_MAX];
	char pathname[PATH_MAX];
	va_list ap;

	va_start(ap, name);
	vsnprintf(filename, sizeof filename, name, ap);
	va_end(ap);

	if (!d && 0 < num_downloads)
		d = downloads[selidx];

	if (getenv("ARIA2T_CONFIG"))
		snprintf(pathname, sizeof pathname, "%s/actions/%s",
				getenv("ARIA2T_CONFIG"), filename);
	else if (getenv("HOME"))
		snprintf(pathname, sizeof pathname, "%s/.config/aria2t/actions/%s",
				getenv("HOME"), filename);
	else
		return -1;

	endwin();

	pid_t pid;
	if (!(pid = vfork())) {
		execl(pathname, filename, d ? d->gid : "", session_file, NULL);
		_exit(127);
	}

	int ret = xwaitpid(pid);

	/* Become foreground process. */
	tcsetpgrp(STDERR_FILENO, getpgrp());
	begwin();

	if (!ret) {
		runaction_maychanged(d);
		refresh();
	}

	return ret;
}

static char *
b64_enc_file(char const *pathname)
{
	char *b64 = NULL;

	int fd;
	if ((fd = open(pathname, O_RDONLY)) < 0)
		goto out;

	struct stat st;
	if (fstat(fd, &st) < 0)
		goto out;

	uint8_t *buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (MAP_FAILED == buf)
		goto out;

	size_t b64_size;
	b64 = b64_enc(buf, st.st_size, &b64_size);

	munmap(buf, st.st_size);

out:
	if (0 <= fd)
		close(fd);
	return b64;
}

static int
fileout(bool must_edit, size_t select_lnum)
{
	endwin();

	pid_t pid;
	if (!(pid = vfork())) {
		char const *cmd;
		if (must_edit) {
			(cmd = getenv("VISUAL")) ||
			(cmd = getenv("EDITOR")) ||
			(cmd = "vi");
		} else {
			(cmd = getenv("PAGER")) ||
			(cmd = "less");
		}

		char buf[22];
		char const *lnum_arg = NULL;

		if (select_lnum &&
		    (strstr(cmd, "vi") ||
		     strstr(cmd, "less")))
		{
			sprintf(buf, "+%"PRIu64, select_lnum);
			lnum_arg = buf;
		}

		execlp(cmd, cmd, session_file, lnum_arg, NULL);
		_exit(127);
	}

	int ret = xwaitpid(pid);

	begwin();

	return ret;
}

static void
print_option(JSONNode const *option, FILE *stream)
{
	if (stream)
		fprintf(stream, "%s=%s\n", option->key, option->str);
}

static void
print_download(Download const *d, FILE *stream)
{
	static char const *const STATUS_MAP[D_NB] = {
		[D_UNKNOWN] = "unknown",
		[D_WAITING] = "waiting",
		[D_ACTIVE] = "active",
		[D_PAUSED] = "paused",
		[D_COMPLETE] = "complete",
		[D_REMOVED] = "removed",
		[D_ERROR] = "error",
	};

	fprintf(stream, "%s\t%s\t%d\t",
			d->gid,
			STATUS_MAP[d->status],
			d->total ? (int)(d->have * 100 / d->total) : -1);

	if (longest_tag)
		fprintf(stream, "%s\t", d->tags ? d->tags : "");

	fprintf(stream, "%s\n", d->display_name);
}

static void
parse_selection_options(JSONNode const *result, Selection const *s, FILE *stream)
{
	if (!s) {
		for (JSONNode const *option = json_first(result);
		     option;
		     option = json_next(option))
		{
			parse_option(option->key, option->str, NULL);
			print_option(option, stream);
		}
	} else {
		size_t i = 0;
		assert(json_length(result) == s->count);
		for (result = json_first(result);
		     result;
		     (result = json_next(result)), ++i)
		{
			Download *d = s->downloads[i];
			if (!upgrade_download(d, NULL))
				continue;

			if (stream) {
				fputs("# ", stream);
				print_download(d, stream);
			}

			JSONNode const *options = json_children(result);
			for (JSONNode const *option = json_first(options);
			     option;
			     option = json_next(option))
			{
				parse_option(option->key, option->str, d);
				print_option(option, stream);
			}

			if (stream)
				fputc('\n', stream);
		}
	}
}

static void
handle_option_update(JSONNode const *result, Selection *s)
{
	if (result) {
		parse_selection_options(result, s, NULL);
		draw_main();
		refresh();
	}

	free_selection(s);
}

static void
handle_option_change(JSONNode const *result, Selection *s)
{
	if (result)
		fetch_options(s, true /* !s => fetch_options(all := true); otherwise ignored */, false);
	else
		free_selection(s);
}

static FILE *
open_input(void)
{
	FILE *ret;
	if (!(ret = fopen(session_file, "r")))
		show_error("Failed to read session file: %s",
				strerror(errno));
	return ret;
}

static FILE *
open_output(void)
{
	FILE *ret;
	if (!(ret = fopen(session_file, "w")))
		show_error("Failed to write session file: %s",
				strerror(errno));
	return ret;
}

/* TODO: Not sure if it is the most efficient way to do this. */
static void
generate_options_from_stream(FILE *stream)
{
	char *line = NULL;
	size_t line_size = 0;
	ssize_t line_len;

	fseek(stream, 0, SEEK_SET);

	/* "options" */
	json_write_beginobj(jw);

	while (0 <= (line_len = getline(&line, &line_size, stream))) {
		char *name = line, *value;
		if ('#' == line[0] ||
		    !(value = strchr(name, '=')))
			continue;
		*value++ = '\0';

		if ('\n' == line[line_len - 1])
			line[--line_len] = '\0';

		json_write_key(jw, name);
		json_write_str(jw, value);
	}

	json_write_endobj(jw);

	free(line);
}

/* Display retrieved options to user for edit. */
static void
handle_option_show(JSONNode const *result, Selection *s)
{
	RPCRequest *rpc = NULL;

	if (!result)
		goto out;

	if (!(rpc = new_rpc()))
		goto out;

	FILE *stream;
	if (!(stream = open_output()))
		goto out;

	parse_selection_options(result, s, stream);
	fclose(stream);

	int status;
	if ((status = run_action(NULL, s ? "i" : "I")) < 0)
		status = fileout(false, 0);

	if (status)
		goto out;

	rpc->handler = (RPCHandler)handle_option_change;

	if (!(stream = open_input()))
		goto out;

	/* TODO: Maybe options should be requested again and only altered
	 * options should be uploaded? */
	rpc->arg = s;
	if (!s) {
		begin_rpc_method("aria2.changeGlobalOption", NULL);
		generate_options_from_stream(stream);
		end_rpc_method();
	} else {
		begin_rpc_multicall();

		for (size_t i = 0; i < s->count; ++i) {
			Download *d = s->downloads[i];
			begin_rpc_multicall_method("aria2.changeOption", d);
			generate_options_from_stream(stream);
			end_rpc_multicall_method();
		}

		end_rpc_multicall();
	}
	s = NULL;

	do_rpc(rpc);
	rpc = NULL;

out:
	if (rpc)
		free_rpc(rpc);
	free_selection(s);
}

/* If requested by user, display returned options. Otherwise just update them. */
static void
fetch_options(Selection *s, bool all, bool user)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc())) {
		free_selection(s);
		return;
	}

	rpc->handler = (RPCHandler)(user ? handle_option_show : handle_option_update);
	rpc->arg = NULL;

	if (s)
		generate_rpc_for_selection(rpc, s, "aria2.getOption");
	else if (!num_selects && all)
		generate_rpc_method("aria2.getGlobalOption");
	else
		generate_rpc_for_selected(rpc, all, "aria2.getOption");

	do_rpc(rpc);
}

static void
show_options(bool all)
{
	fetch_options(NULL, all, true);
}

/* Select (last) newly added download and write back errorneous files. */
static void
handle_download_add(JSONNode const *result, void *arg)
{
	(void)arg;

	if (!result)
		return;

	for (result = json_first(result); result; result = json_next(result)) {
		Download **dd;

		if (JSONT_OBJECT == json_type(result)) {
			handle_error(result);
		} else {
			JSONNode const *gid = json_children(result);

			if (JSONT_STRING == json_type(gid)) {
				/* single GID returned */
				if ((dd = get_download_by_gid(gid->str)))
					selidx = dd - downloads;
			} else {
				/* get first GID of the array */
				if ((dd = get_download_by_gid(json_children(gid)->str)))
					selidx = dd - downloads;
			}
		}
	}

	on_downloads_change(true);
	refresh();
}

static void
add_downloads(char cmd)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	int status;
	if ((status = run_action(NULL, "%c", cmd)) < 0)
		status = fileout(true, 0);

	FILE *stream;
	if (status ||
	   !(stream = open_input()))
	{
		free_rpc(rpc);
		return;
	}

	rpc->handler = (RPCHandler)handle_download_add;

	begin_rpc_multicall();

	char *line = NULL;
	size_t line_size = 0;
	ssize_t line_len;

next_line:
	for (line_len = getline(&line, &line_size, stream); 0 <= line_len;) {
		char *b64str = NULL;
		enum { URI, TORRENT, METALINK } type;

		if (0 < line_len && '\n' == line[line_len - 1])
			line[--line_len] = '\0';

		if (line_len <= 0)
			goto next_line;

		char *uri = line, *next_uri;
		if ((next_uri = strchr(uri, '\t')))
			*next_uri++ = '\0';

#define IS_SUFFIX(lit) \
	((size_t)line_len > ((sizeof lit) - 1) && \
		 !memcmp(uri + (size_t)line_len - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (IS_SUFFIX(".torrent"))
			type = TORRENT;
		else if (IS_SUFFIX(".meta4") || IS_SUFFIX(".metalink"))
			type = METALINK;
		else
			type = URI;

		if (type != URI && !(b64str = b64_enc_file(uri)))
			type = URI;
#undef IS_SUFFIX

		char const *method;
		switch (type) {
		case TORRENT:  method = "aria2.addTorrent"; break;
		case METALINK: method = "aria2.addMetalink"; break;
		case URI:      method = "aria2.addUri"; break;
		default: abort();
		}

		begin_rpc_multicall_method(method, NULL);

		/* "data" */
		switch (type) {
		case TORRENT:
		case METALINK:
			json_write_str(jw, b64str);
			free(b64str);
			break;

		case URI:
			json_write_beginarr(jw);
			for (;;) {
				json_write_str(jw, uri);

				if (!(uri = next_uri))
					break;

				if ((next_uri = strchr(uri, '\t')))
					*next_uri++ = '\0';
			}
			json_write_endarr(jw);
			break;
		}
		if (TORRENT == type) {
			/* "uris" */
			json_write_beginarr(jw);
			while (next_uri) {
				uri = next_uri;
				if ((next_uri = strchr(uri, '\t')))
					*next_uri++ = '\0';

				json_write_str(jw, uri);
			}
			json_write_endarr(jw);
		}
		/* "options" */
		json_write_beginobj(jw);
		while (0 <= (line_len = getline(&line, &line_size, stream))) {
			if (0 < line_len && '\n' == line[line_len - 1])
				line[--line_len] = '\0';

			if (line_len <= 0)
				continue;

			char *name = line;
			while (isspace(*name))
				++name;

			/* No leading spaces, not an option. */
			if (name == line)
				break;

			/* No equal sign, not an option. */
			char *value;
			if (!(value = strchr(name, '=')))
				break;
			*value++ = '\0';

			json_write_key(jw, name);
			json_write_str(jw, value);
		}

		/* (none) */
		json_write_endobj(jw);
		/* "position" */
		/* Insert position at specified queue index. */
		/* Do not care if it's bigger, it will be added at the end. */
		json_write_num(jw, selidx);

		end_rpc_multicall_method();
	}

	free(line);
	fclose(stream);

	end_rpc_multicall();

	do_rpc(rpc);
}

static void
update_all(void)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	rpc->handler = handle_update_all;

	begin_rpc_multicall();

	generate_rpc_multicall_method("aria2.getGlobalStat", NULL);

	for (int n = 0;; ++n) {
		char const *method;
		bool pageable;

		switch (n) {
		case 0:
			method = "aria2.tellWaiting";
			pageable = true;
			break;

		case 1:
			method = "aria2.tellActive";
			pageable = false;
			break;

		case 2:
			method = "aria2.tellStopped";
			pageable = true;
			break;

		default:
			goto out_of_loop;
		}

		begin_rpc_multicall_method(method, NULL);

		if (pageable) {
			/* "offset" */
			json_write_int(jw, 0);

			/* "num" */
			json_write_int(jw, 99999);
		}

		/* "keys" {{{ */
		json_write_beginarr(jw);
		json_write_str(jw, "gid");
		json_write_str(jw, "status");
		json_write_str(jw, "errorMessage");
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
		if (is_local || select_need_files)
			json_write_str(jw, "files");
		json_write_endarr(jw);
		/* }}} */

		end_rpc_multicall_method();
	}
out_of_loop:

	end_rpc_multicall();

	do_rpc(rpc);
}

static void
handle_download_remove(JSONNode const *result, Selection *s)
{
	if (result) {
		for (size_t i = 0; i < s->count; ++i) {
			Download *d = s->downloads[i];
			d->status = -abs(d->status);
		}

		update_downloads();
	}

	free_selection(s);
}

static void
remove_download(void)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	Selection *s = get_selection(!!num_selects);

	rpc->handler = (RPCHandler)handle_download_remove;
	rpc->arg = s;

	begin_rpc_multicall();

	for (size_t i = 0; i < s->count; ++i) {
		Download *d = s->downloads[i];

		int res = run_action(d, "D");
		if (!res)
			continue;
		/* Avoid further deletions on failure. */
		else if (0 < res)
			break;

		if (D_ACTIVE == abs(d->status)) {
			show_error("Refusing to delete active download");
			continue;
		}

		generate_rpc_multicall_method(do_forced ? "aria2.forceRemove" : "aria2.remove", d);
	}

	end_rpc_multicall();

	do_rpc(rpc);

	draw_statusline();
	refresh();
}

static void
switch_view(int next)
{
	if (!(view = strchr(VIEWS + 1, view)[next ? 1 : -1])) {
		view = VIEWS[next ? 1 : ARRAY_SIZE(VIEWS) - 2];
		tui.selidx = -1; /* force redraw */
	}

	update_downloads();
	draw_all();
	refresh();
}

static void
select_download(Download const *d)
{
	Download **dd;

	if (d && upgrade_download(d, &dd)) {
		selidx = dd - downloads;
		draw_cursor();
		refresh();
	}
}

static void
jump_prev_group(void)
{
	if (0 < num_downloads) {
		Download **dd = &downloads[selidx];
		Download **lim = &downloads[0];
		int8_t status = abs((*dd)->status);

		while (lim <= --dd) {
			if (status != abs((*dd)->status)) {
				status = abs((*dd)->status);

				/* Go to the first download of the group. */
				while (lim <= --dd && status == abs((*dd)->status));

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
		Download **dd = &downloads[selidx];
		Download **lim = &downloads[num_downloads];
		int8_t status = abs((*dd)->status);

		while (++dd < lim) {
			if (status != abs((*dd)->status)) {
				select_download(*dd);
				break;
			}
		}
	}
}

typedef struct {
	Selection *s;
	size_t num_has_tell_status;
} FetchFilesArg;

static void
select_files(Selection *s, bool all);

static void
handle_fetch_files(JSONNode const *result, FetchFilesArg *arg)
{
	if (result) {
		size_t i = 0;
		assert(json_length(result) == arg->num_has_tell_status);
		for (JSONNode const *node = json_first(result);
		     node;
		     (node = json_next(node)), ++i)
		{
			Download *d = arg->s->downloads[i];

			if (JSONT_OBJECT == json_type(node)) {
				Download **dd;
				if (upgrade_download(d, &dd))
					delete_download_at(dd);
				continue;
			}

			parse_download(d, json_children(node));
		}

		on_downloads_change(true);
		refresh();

		select_files(arg->s, false /* Ignored. */);
	} else {
		free_selection(arg->s);
	}

	free(arg);
}

static void
fetch_files(Selection *s)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc())) {
		free_selection(s);
		return;
	}

	rpc->handler = (RPCHandler)handle_fetch_files;
	FetchFilesArg *arg = malloc(sizeof *arg);
	if (!arg)
		abort();

	arg->s = s;
	arg->num_has_tell_status = s->count;

	rpc->arg = arg;

	begin_rpc_multicall();

	for (size_t i = 0; i < arg->num_has_tell_status;) {
		Download *d = s->downloads[i];
		if (0 < d->num_files) {
			/* Order downloads without files at first to avoid an
			 * allocation. */
			Download **p = &s->downloads[--arg->num_has_tell_status];
			Download *t = *p;
			*p = d;
			s->downloads[i] = t;
			continue;
		}

		begin_rpc_multicall_method("aria2.tellStatus", d);
		/* "keys" */
		json_write_beginarr(jw);
		json_write_str(jw, "files");
		json_write_endarr(jw);
		end_rpc_multicall_method();

		++i;
	}

	end_rpc_multicall();

	do_rpc(rpc);
}

static void
select_files(Selection *s, bool all)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc())) {
		free_selection(s);
		return;
	}

	FILE *stream;
	if (!(stream = open_output())) {
		free_rpc(rpc);
		free_selection(s);
		return;
	}

	if (!s)
		s = get_selection(all);

	fputs("# ID\tHave\tTotal\tPercent\tPath\n", stream);
	for (int state = 1;;) {
		fprintf(stream, "# %s files", state ? "Selected" : "Unselected");
		if (1 == s->count) {
			Download const *d = s->downloads[0];
			fprintf(stream, " (%"PRIu32"/%"PRIu32")",
					state ? d->num_files_selected : d->num_files - d->num_files_selected,
					d->num_files);
		}
		fputc('\n', stream);

		for (size_t i = 0; i < s->count; ++i) {
			Download const *d = s->downloads[i];
			if (!d->num_files) {
				fclose(stream);
				free_rpc(rpc);
				fetch_files(s);
				return;
			}

			for (uint32_t j = 0; j < d->num_files; ++j) {
				File const *f = &d->files[j];
				if (f->selected != state)
					continue;

				char const *name = get_file_display_name(d, f);
				char szhave[6];
				char sztotal[6];
				char szpercent[6];

				szhave[fmt_space(szhave, f->have)] = '\0';
				sztotal[fmt_space(sztotal, f->total)] = '\0';
				szpercent[fmt_percent(szpercent, f->have, f->total)] = '\0';

				if (1 < s->count)
					fprintf(stream, "%s_", d->gid);
				fprintf(stream, "%"PRIu32"\t%s\t%s\t%s\t%s\n",
						j + 1,
						szhave, sztotal, szpercent,
						name);
			}
		}

		if (--state < 0)
			break;
		fputc('\n', stream);
	}

	fprintf(stream,
			"\n"
			"# Select files of %s%s\n"
			"#\n"
			"# To unselect a file remove that line.\n"
			"#\n"
			"# Lines can be re-ordered and edited unless their ID is present.\n"
			"#\n"
			"# Lines starting with # are ignored.\n"
			"#\n"
			"# If you do not remove unselected files, they will be selected.\n"
			"#\n",
			1 == s->count ? "download " : "several downloads",
			1 == s->count ? s->downloads[0]->display_name : "");

	fclose(stream);

	int status;
	if ((status = run_action(1 == s->count ? s->downloads[0] : NULL, "f")) < 0)
		status = fileout(true, 0);

	if (status) {
		free_rpc(rpc);
		return;
	}

	if (!(stream = open_input())) {
		free_rpc(rpc);
		return;
	}

	for (size_t i = 0; i < s->count; ++i) {
		Download *d = s->downloads[i];
		for (uint32_t j = 0; j < d->num_files; ++j)
			d->files[j].selected = false;
		d->num_files_selected = 0;
	}

	char *line = NULL;
	size_t line_size = 0;
	ssize_t line_len;

	while (0 <= (line_len = getline(&line, &line_size, stream))) {
		if ('#' == line[0])
			continue;

		Download *d;
		char const *p;
		if (1 < s->count) {
			d = find_download_by_gid(line, &p);
			if (!d)
				continue;
			if ('_' != *p)
				continue;
			++p;
		} else {
			d = s->downloads[0];
			p = line;
		}

		uint32_t file_index;
		if (1 != sscanf(p, "%"SCNu32, &file_index))
			continue;
		--file_index;

		if (d->num_files <= file_index)
			continue;

		d->files[file_index].selected = true;
		++d->num_files_selected;
	}

	free(line);
	fclose(stream);

	rpc->handler = (RPCHandler)handle_option_change;
	rpc->arg = s;

	begin_rpc_multicall();

	for (size_t i = 0; i < s->count; ++i) {
		Download *d = s->downloads[i];

		char *value = NULL;
		int value_len = 0;
		int value_size = 0;

		for (;;) {
			for (uint32_t i = 0; i < d->num_files; ++i) {
				if (!d->files[i].selected)
					continue;

				uint32_t from = i;
				uint32_t to = i;

				for (; to + 1 < d->num_files && d->files[to + 1].selected; ++to);

				value_len += snprintf(value + value_len, value ? value_size : 0,
						from == to
							? ",%"PRIu32
							: ",%"PRIu32"-%"PRIu32,
						from + 1, to + 1);

				i = to;
			}

			if (value || !value_len)
				break;

			value_size = value_len + 1 /* NUL */;
			value_len = 0;

			if (!(value = malloc(value_size)))
				abort();
		}

		begin_rpc_multicall_method("aria2.changeOption", d);
		/* "options" */
		json_write_beginobj(jw);
		json_write_key(jw, "select-file");
		json_write_str(jw, value ? value + 1 /* Skip initial ",". */ : "");
		json_write_endobj(jw);
		end_rpc_multicall_method();

		free(value);
	}

	end_rpc_multicall();

	do_rpc(rpc);
}

static int
plumb_downloads(char cmd)
{
	FILE *stream;
	if (!(stream = open_output()))
		return EXIT_FAILURE;

	size_t select_lnum = 0;
	for (size_t i = 0; i < num_downloads; ++i) {
		Download *d = downloads[i];
		print_download(d, stream);
		select_lnum += i <= (size_t)selidx;
		if (D_ERROR == abs(d->status)) {
			fprintf(stream, "# %s\n", d->error_msg);
			select_lnum += i < (size_t)selidx;
		}
	}

	fclose(stream);

	int status;
	if ((status = run_action(NULL, "%c", cmd)) < 0)
		status = fileout(true, select_lnum);

	return status;
}

static void
select_downloads(char cmd)
{
	int status = plumb_downloads(cmd);
	if (status)
		return;

	FILE *stream;
	if (!(stream = open_input()))
		return;

	for (size_t i = 0; i < num_downloads; ++i)
		downloads[i]->deleted = true;

	char *line = NULL;
	size_t line_size = 0;
	ssize_t line_len;

	while (0 <= (line_len = getline(&line, &line_size, stream))) {
		Download *d = find_download_by_gid(line, NULL);
		if (!d)
			continue;

		d->deleted = false;
	}

	free(line);
	fclose(stream);

	for (size_t i = num_downloads; 0 < i;) {
		Download **dd = &downloads[--i], *d = *dd;
		if (d->deleted) {
			d->deleted = false;
			delete_download_at(dd);
		}
	}

	num_selects = SIZE_MAX;

	on_downloads_change(true);
	refresh();
}

static void
shutdown_aria(void)
{
	if (0 < run_action(NULL, "Q"))
		return;

	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)handle_shutdown;

	generate_rpc_method(do_forced ? "aria2.forceShutdown" : "aria2.shutdown");

	do_rpc(rpc);
}

static void
read_stdin(void)
{
	int ch;
	MEVENT event;

	while (ERR != (ch = getch())) {
		switch (ch) {
		/*MAN(KEYS)
		 * .TP
		 * .BR j ,\  Down
		 * Go downwards.
		 */
		case 'j':
		case KEY_DOWN:
			++selidx;
			draw_cursor();
			refresh();
			break;

		case KEY_RESIZE:
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR k ,\  Up
		 * Go upwards.
		 */
		case 'k':
		case KEY_UP:
			--selidx;
			draw_cursor();
			refresh();
			break;

		case KEY_MOUSE:
			if (getmouse(&event) != OK)
				break;

			/*MAN(KEYS)
			 * .TP
			 * .B MouseScroll
			 * Move cursor.
			 */
#ifdef BUTTON5_PRESSED
			if (event.bstate & BUTTON4_PRESSED)
				selidx -= VIEWS[1] == view ? 5 : 1;
			else if (event.bstate & BUTTON5_PRESSED)
				selidx += VIEWS[1] == view ? 5 : 1;
			else
#endif
			/*MAN(KEYS)
			 * .TP
			 * .B MouseDown
			 * Move cursor.
			 */
			if (((BUTTON1_PRESSED | BUTTON3_PRESSED) & event.bstate) &&
			    VIEWS[1] == view &&
			    event.y < getmainheight())
				selidx = topidx + event.y;

			draw_cursor();
			refresh();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR h
		 * Go to parent download.
		 */
		case 'h':
			if (0 < num_downloads)
				select_download(downloads[selidx]->parent);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR l
		 * Go to first children.
		 */
		case 'l':
			if (0 < num_downloads)
				select_download(downloads[selidx]->first_child);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR n
		 * Go to next sibling.
		 */
		case 'n':
			if (0 < num_downloads)
				select_download(downloads[selidx]->next_sibling);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR N
		 * Go to previous sibling.
		 */
		case 'N':
			if (0 < num_downloads)
				select_download(download_prev_sibling(downloads[selidx]));
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR g ,\  Home
		 * Go to first.
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
		 * Go to last.
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
		 * .B Ctrl-O
		 * Jump to old position.
		 * .IP
		 * Old position is the place where download was before re-sorting.
		 */
		case CONTROL('O'):
		{
			int tmp = selidx;
			selidx = oldidx;
			oldidx = tmp;
			draw_cursor();
			refresh();
		}
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR [
		 * Go to first download of the previous status group.
		 */
		case '[':
			jump_prev_group();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR ]
		 * Go to first download of the next status group.
		 */
		case ']':
			jump_next_group();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR v ,\  Right
		 * Switch to next view.
		 */
		case 'v':
		case KEY_RIGHT:
			switch_view(1);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR V ,\  Left
		 * Switch to previous view.
		 */
		case 'V':
		case KEY_LEFT:
			switch_view(0);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B J
		 * Move download forward in the queue.
		 */
		case 'J':
		/*MAN(KEYS)
		 * .TP
		 * .B K
		 * Move download backward in the queue.
		 */
		case 'K':
			if (0 < num_downloads)
				change_position(downloads[selidx], 'J' == ch ? -1 : 1, SEEK_CUR);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR a ,\  A ,\  +
		 * Add downloads from
		 * .IR "session file" .
		 * Open
		 * .I session file
		 * for editing unless there are associated actions.
		 * .IP
		 * For the expected format refer to aria2's
		 * .IR "Input File" .
		 * .IP
		 * Added downloads are queued after the currently selected download.
		 * .IP
		 * Note that URIs that end with
		 * .BR .torrent \ and\  .meta4 \ or\  .metalink
		 * and refer to a readable file on the local filesystem are uploaded to
		 * aria2 as a blob.
		 */
		case 'a':
		case 'A':
		case '+':
			add_downloads(ch);
			break;


		/*MAN(KEYS)
		 * .TP
		 * .BR s ,\  S
		 * Start (unpause) downloads.
		 * .TP
		 * .BR p ,\  P
		 * Pause downloads.
		 */
		case 's':
		case 'p':
		case 'S':
		case 'P':
			pause_download('S' == ch || 'P' == ch, 'p' == ch || 'P' == ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR f ,\  F
		 * Interactively select files.
		 */
		case 'f':
		case 'F':
			select_files(NULL, 'F' == ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR i ,\  I
		 * Write selected download/global options in a
		 * name=value format into the
		 * .IR "session file" .
		 * After that, open
		 * .I session file
		 * for viewing unless there are associated actions. To change
		 * options, edit the file.
		 * .IP
		 * Be extra careful not to accidentally overwriting global
		 * options (which contained among returned options), so make
		 * sure final file contains only options you really want to be
		 * modified. Note that you can always edit aria2's
		 * .I Input file
		 * to correct unwanted edits.
		 */
		case 'i':
		case 'I':
			show_options('I' == ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR = ,\ / ,\ ?
		 * Select downloads.
		 * .IP
		 * When only a subset of all downloads are displayed, an
		 * .BR = -sign
		 * is shown at the status bar.
		 * .IP
		 * When selection is active it slightly modifies upper case
		 * commands that would operate on global resources (like
		 * .BR I )
		 * so that they will take action an all selected downloads.
		 * .IP
		 * To clear selection use
		 * .BR q .
		 */
		case '=':
		case '/':
		case '?':
			select_downloads(ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR x ,\  X
		 * Remove download result for download.
		 */
		case 'x':
		case 'X':
			purge_download('X' == ch);
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR D ,\  Del
		 * If there is an action for \*(lqD\*(rq execute it, otherwise
		 * remove a non-active download.
		 * .IP
		 * Because it exists only as an upper case command its
		 * behaviour slightly differs:
		 * .RS
		 * .IP \(bu
		 * By default, delete a single download if selection is not
		 * active.
		 * .IP \(bu
		 * Delete all downloads if selection is active. Refer to
		 * .BR = .
		 * .RE
		 */
		case 'D':
		case KEY_DC: /* Delete. */
			remove_download();
			break;

		/*MAN(KEYS)
		 * .TP
		 * .B !
		 * Do next command by force.
		 * .IP
		 * For example, pausing a Torrent download by force
		 * .RB ( !p )
		 * means that
		 * aria2 will not contact to trackers.
		 */
		case '!':
			do_forced = true;
			continue;

		/*MAN(KEYS)
		 * .TP
		 * .BR |
		 * Emit GIDs of selected downloads.
		 */
		case '|':
			plumb_downloads('|');
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
				if (0 < num_downloads) {
					Download const *d = downloads[selidx];
					printf("%s\n", d->gid);
				}

				exit(EXIT_SUCCESS);
			}
			break;

		/*MAN(KEYS)
		 * .TP
		 * .BR q ,\  Esc
		 * Do one of the followings in this order:
		 * .RS
		 * .IP EITHER
		 * Return to default view
		 * .IP OR
		 * Clear selection
		 * .IP OR
		 * Quit program.
		 * .RE
		 */
		case 'q':
		case CONTROL('['):
			if (view != VIEWS[1]) {
				view = VIEWS[1];
				tui.selidx = -1; /* Force redraw. */
				draw_all();
				refresh();
				break;
			}

			if (num_selects) {
				num_selects = 0;
				update_all();
				break;
			}

			/* FALLTHROUGH */

		/*MAN(KEYS)
		 * .TP
		 * .B Z
		 * Quit program.
		 */
		case 'Z':
			exit(EXIT_SUCCESS);

		/*MAN(KEYS)
		 * .TP
		 * .B Q
		 * Shut down aria2 if action \*(lqQ\*(rq does not exist or
		 * terminated with success.
		 */
		case 'Q':
			shutdown_aria();
			break;

		/* Undocumented. */
		case CONTROL('L'):
			num_selects = 0;

			/* Try connect if not connected. */
			if (!ws_is_alive())
				try_connect();
			else
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
		 * .BR keyname (3x)
		 * for translating pressed key to a name.
		 */
		default:
		default_action:
			run_action(NULL, "%s", keyname(ch));
			break;
		}

		do_forced = false;
	}
}

static void
fill_pairs(void)
{
	switch (!getenv("NO_COLOR") ? COLORS : 0) {
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
	if (*session_file) {
		unlink(session_file);
		*session_file = '\0';
	}
}

static void
handle_remote_info(JSONNode const *result, void *arg)
{
	JSONNode const *node;

	(void)arg;

	if (!result)
		return;

	result = json_children(result);
	node = json_children(result);
	parse_session_info(node);

	result = json_next(result);
	node = json_children(result);
	parse_options(node, NULL);

	/* we have is_local correctly set, so we can start requesting downloads */
	update_all();
}

static void
remote_info(void)
{
	RPCRequest *rpc;
	if (!(rpc = new_rpc()))
		return;

	rpc->handler = handle_remote_info;

	begin_rpc_multicall();

	generate_rpc_multicall_method("aria2.getSessionInfo", NULL);
	generate_rpc_multicall_method("aria2.getGlobalOption", NULL);

	end_rpc_multicall();

	do_rpc(rpc);
}

void
on_ws_open(void)
{
	pfds[1].fd = ws_fileno();
	pfds[1].events = POLLIN;

	on_timeout = NULL;

	clear_error();

	tui.selidx = -1;
	tui.topidx = -1;

	is_local = false;
	remote_info();

	draw_all();
	refresh();
}

void
on_ws_close(void)
{
	pfds[1].fd = -1;

	if (!errno)
		exit(EXIT_SUCCESS);

	if (try_connect != on_timeout) {
		on_timeout = try_connect;
		interval = (struct timespec){
			.tv_sec = 1,
		};
	} else {
		interval.tv_sec *= 2;
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
	for (size_t i = 0; i < num_selects; ++i) {
		if (!is_gid(selects[i])) {
			select_need_files = true;
			break;
		}
	}
}

static void
handle_signal_resize(int signum)
{
	(void)signum;

	endwin();
	begwin();

	/* and now we can redraw the whole screen with the
	 * hopefully updated screen dimensions */
	draw_all();
	refresh();
}

static void
handle_signal_quit(int signum)
{
	(void)signum;

	exit(EXIT_SUCCESS);
}

static void
handle_signal_disconnect(int signum)
{
	(void)signum;

	ws_close();
}

static void
setup_sighandlers(void)
{
	struct sigaction sa;

	/* Block all signals by default. */
	sigfillset(&sa.sa_mask);
	sigprocmask(SIG_BLOCK, &sa.sa_mask, NULL);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;

	sa.sa_handler = handle_signal_resize;
	sigaction(SIGWINCH, &sa, NULL);

	sa.sa_handler = handle_signal_quit;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGKILL, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	sa.sa_handler = handle_signal_disconnect;
	sigaction(SIGPIPE, &sa, NULL);
}

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	setup_sighandlers();

	for (int argi = 1; argi < argc;) {
		char const *arg = argv[argi++];
		/*MAN(OPTIONS)
		 * .TP
		 * \fB\-\-select\fR { \fIPATH\fR | \fIGID\fR }...
		 * Select downloads by their \fIGID\fR or by matching \fIPATH\fR prefix. If
		 * omitted, every download is selected. Refer to
		 * .BR = .
		 */
		if (!strcmp(arg, "--select")) {
			if (argc <= argi)
				goto show_usage;

			num_selects = 1;
			selects = (char const **)&argv[argi];
			while (++argi, argv[argi] && '-' != *argv[argi])
				++num_selects;
		}
		/*MAN(OPTIONS)
		 * .TP
		 * .B --version
		 * Print version and exit.
		 */
		else if (!strcmp(arg, "--version")) {
			puts("aria2t-"VERSION);
			return EXIT_SUCCESS;
		} else {
		show_usage:
			return EXIT_FAILURE;
		}
	}

#if 0
	if (isatty(STDOUT_FILENO))
		freopen("/dev/null", "w", stderr);
#endif

	init_config();

	init_action();

	json_writer_init(jw);

	/* atexit(clear_downloads); */
	atexit(cleanup_session_file);

	pfds[0].fd = STDIN_FILENO;
	pfds[0].events = POLLIN;

	atexit(_endwin);
	newterm(NULL, stderr, stdin);
	start_color();
	use_default_colors();
	fill_pairs();
	/* No input buffering. */
	cbreak();
	/* No input echo. */
	noecho();
	/* Do not translate '\n's. */
	nonl();
	/* Make getch() non-blocking. */
	nodelay(stdscr, TRUE);
	/* Catch special keys. */
	keypad(stdscr, TRUE);
	/* 8-bit inputs. */
	meta(stdscr, TRUE);
	/* Listen for all mouse events. */
	mousemask(ALL_MOUSE_EVENTS, NULL);
	/* Be immediate. */
	mouseinterval(0);

	update_terminfo();

	draw_all();
	refresh();

	try_connect();

	sigset_t ss;
	sigemptyset(&ss);

	for (;;) {
		switch (ppoll(pfds, ARRAY_SIZE(pfds), on_timeout ? &interval : NULL, &ss)) {
		case 0:
			on_timeout();
			break;

		case -1:
			break;

		default:
			/* terminal gone */
			if (~POLLIN & pfds[0].revents)
				exit(EXIT_SUCCESS);
			else if (POLLIN & pfds[0].revents)
				read_stdin();

			if (pfds[1].revents)
				ws_recv();
			break;
		}
	}
}
