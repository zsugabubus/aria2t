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

#include "keys.in"

#define XATTR_NAME "user.tags"

typedef struct json_node JSONNode;

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

typedef struct {
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

static char const *const NONAME = "?";

/* NOTE: Order is relevant for sorting. */
enum {
	D_UNKNOWN,
	D_WAITING,
	D_ACTIVE,
	D_PAUSED,
	D_COMPLETE,
	D_REMOVED,
	D_ERROR,
	D_NB,
};
static char DOWNLOAD_SYMBOLS[D_NB] = "?w>|.-*";

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
	uint32_t num_perstatus[D_NB];

	char external_ip[INET6_ADDRSTRLEN];
} global;

static struct {
	enum {
		ACT_VISUAL = 0,
		ACT_ADD_DOWNLOADS,
		ACT_SHUTDOWN,
		ACT_PRINT_GID,
		ACT_PAUSE,
		ACT_UNPAUSE,
		ACT_PURGE
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

struct pollfd pfds[2];
static struct timespec period;
static void(*periodic)(void);

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
static int do_forced = 0;

static int oldidx;

/* Logical state. */
static int selidx, topidx;

/* Visual state. */
static struct {
	int selidx, topidx;
} tui;

static struct json_writer jw[1];

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
	free(d->error_message);
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
	if (0 == --d->refcnt)
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
default_handler(JSONNode const *result, void *arg)
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

	((str = getenv("ARIA_RPC_PORT")) &&
		(errno = 0,
		 remote_port = strtoul(str, NULL, 10),
		 0 == errno)) ||
	(remote_port = 6800);

	/* “token:$$secret$$” */
	(str = getenv("ARIA_RPC_SECRET")) ||
	(str = "");

	secret_token = malloc(snprintf(NULL, 0, "token:%s", str) + 1);
	if (!secret_token) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}

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

		for (; *dd != d; ++dd)
			;

		*pdd = dd;
	}

	return true;
}

static bool
filter_download(Download *d, Download **dd);

static Download **
get_download_by_gid(char const *gid)
{
	Download *d;
	Download **dd = downloads;
	Download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (0 == memcmp((*dd)->gid, gid, sizeof (*dd)->gid))
			return dd;

	if (!(dd = new_download()))
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
	int size;

	va_start(argptr, format);
	size = vsnprintf(NULL, 0, format, argptr) + 1;
	va_end(argptr);

	if (!(p = realloc(error_message, size))) {
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
error_handler(JSONNode const *error)
{
	JSONNode const *message = json_get(error, "message");

	if (ACT_VISUAL == action.kind) {
		set_error_message("%s", message->val.str);
		refresh();
	} else {
		fprintf(stderr, "%s\n", message->val.str);
		exit(EXIT_FAILURE);
	}
}

static void
free_rpc(RPCRequest *rpc)
{
	rpc->handler = NULL;
}

static void
on_downloads_change(bool stickycurs);

static void
update(void);

static bool
download_insufficient(Download *d)
{
	return d->display_name == NONAME ||
	       -D_WAITING == d->status ||
	       -D_ERROR == d->status ||
	       (is_local && 0 == d->num_files);
}

static void
queue_changed(void)
{
	Download **dd = downloads;
	Download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd) {
		Download *const d = *dd;

		if (D_WAITING == abs(d->status))
			d->status = -D_WAITING;
	}
}

static void
notification_handler(char const *method, JSONNode const *event)
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

	char *const gid = json_get(event, "gid")->val.str;
	Download **dd;
	Download *d;

	if (!(dd = get_download_by_gid(gid)))
		return;
	d = *dd;

	int8_t const new_status = STATUS_MAP[K_notification_parse(method + strlen("aria2.on"))];

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
			update();
	}
}

void
on_ws_message(char *msg, uint64_t msglen)
{
	static JSONNode *nodes;
	static size_t num_nodes;

	JSONNode const *id;
	JSONNode const *method;

	(void)msglen;

	json_parse(msg, &nodes, &num_nodes);

	if ((id = json_get(nodes, "id"))) {
		JSONNode const *const result = json_get(nodes, "result");
		RPCRequest *const rpc = &rpc_requests[(unsigned)id->val.num];

		if (!result)
			error_handler(json_get(nodes, "error"));

		/* NOTE: this condition shall always be true, but aria2c
		 * responses with some long messages with “Parse error.” that
		 * contains id=null, so we cannot get back the handler. the
		 * best we can do is to ignore and may leak a resource inside
		 * data. */
		if (rpc->handler) {
			rpc->handler(result, rpc->arg);
			free_rpc(rpc);
		}

	} else if ((method = json_get(nodes, "method"))) {
		JSONNode const *const params = json_get(nodes, "params");

		notification_handler(method->val.str, params + 1);
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

	set_error_message("too many pending messages");

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
	for (uint8_t n = ARRAY_SIZE(rpc_requests); 0 < n;) {
		RPCRequest *rpc = &rpc_requests[--n];

		if (rpc->handler) {
			rpc->handler(NULL, rpc->arg);
			free_rpc(rpc);
		}
	}
}

static void
fetch_options(Download *d, bool user);

static void
parse_session_info(JSONNode const *result)
{
	char *tmpdir;

	(tmpdir = getenv("TMPDIR")) ||
	(tmpdir = "/tmp");

	snprintf(session_file, sizeof session_file, "%s/aria2t.%s",
			tmpdir,
			json_get(result, "sessionId")->val.str);
}

static bool
do_rpc(RPCRequest *rpc)
{
	json_write_endobj(jw);
	if (-1 == ws_write(jw->buf, jw->len)) {
		set_error_message("write: %s", strerror(errno));
		free_rpc(rpc);

		return false;
	} else {
		return true;
	}
}

static void
update_download_tags(Download *d)
{
	ssize_t size;
	char *p;

	/* do not try read tags if aria2 is remote */
	if (!is_local)
		return;

	if (!d->tags)
		d->tags = malloc(255 + 1);
	if (!d->tags)
		goto no_tags;

	if (d->dir && d->name) {
		char pathbuf[PATH_MAX];

		snprintf(pathbuf, sizeof pathbuf, "%s/%s", d->dir, d->name);
		if (0 < (size = getxattr(pathbuf, XATTR_NAME, d->tags, 255)))
			goto has_tags;
	}

	if (0 < d->num_files && d->files[0].path)
		if (0 < (size = getxattr(d->files[0].path, XATTR_NAME, d->tags, 255)))
			goto has_tags;

	goto no_tags;

has_tags:
	d->tags[size] = '\0';
	/* transform tags: , -> ' ' */
	for (p = d->tags; (p += strcspn(p, "\t\r\n")) && '\0' != *p; ++p)
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

static URI *
find_file_uri(File const *f, char const *uri)
{
	for (uint32_t i = 0; i < f->num_uris; ++i) {
		URI *u = &f->uris[i];

		if (0 == strcmp(u->uri, uri))
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
			if (0 == strcmp(field->key, "uri")) {
				u = find_file_uri(f, field->val.str);
				assert(u);
			} else if (0 == strcmp(field->key, "downloadSpeed"))
				s.download_speed = strtoul(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "currentUri"))
				s.current_uri = field->val.str;
		} while ((field = json_next(field)));

		if (0 == strcmp(u->uri, s.current_uri))
			s.current_uri = NULL;
		if (s.current_uri)
			s.current_uri = strdup(s.current_uri);

		uri_addserver(u, &s);
	}
}

static void
parse_peer_id(Peer *p, char *peer_id)
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
draw_progress(Download const *d, uint8_t const *progress, uint64_t offset, uint64_t end_offset)
{
	uint8_t const
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
		" \0:)",          /* empty */
		"\xe2\x96\x8f\0", /* left 1/8 */
		"\xe2\x96\x8e\0", /* left 2/8 */
		"\xe2\x96\x8d\0", /* left 3/8 */
		"\xe2\x96\x8c\0", /* left 4/8 */
		"\xe2\x96\x8b\0", /* left 5/8 */
		"\xe2\x96\x8a\0", /* left 6/8 */
		"\xe2\x96\x89\0", /* left 7/8 */
		"\xe2\x96\x88\0", /* left 8/8 */
		"\xe2\x96\x90\0", /* right half */
		"\xe2\x96\x91\0", /* light shade */
		"\xe2\x96\x92\0", /* medium shade */
		"\xe2\x96\x93\0", /* dark shade */
	};

	int const width = COLS - getcurx(stdscr) - 3;

	if (/* not enough information */
	    !d->piece_size ||
	    (!progress && offset < end_offset) ||
	    /* not enough space */
	    width <= 0)
	{
		clrtoeol();
		return;
	}

	uint32_t const piece_offset = offset / d->piece_size;
	uint32_t const end_piece_offset = (end_offset + (d->piece_size - 1)) / d->piece_size;
	uint32_t const piece_count = end_piece_offset - piece_offset;

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
			bool const has_piece = (progress[piece_index / CHAR_BIT] >> (CHAR_BIT - 1 - piece_index % CHAR_BIT)) & 1;
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
	if (0 == d->num_pieces) {
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

	for (char const *hex = bitfield; '\0' != *hex; ++hex)
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
			parse_progress(d, &p->progress, field->val.str);
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
	} while ((field = json_next(field)));
}

static void
parse_peers(Download *d, JSONNode const *node)
{
	uint32_t const num_oldpeers = d->num_peers;
	Peer *const oldpeers = d->peers;

	d->num_peers = json_len(node);
	if (!(d->peers = malloc(d->num_peers * sizeof *(d->peers))))
		goto free_oldpeers;

	if (json_isempty(node))
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

			uint64_t const pieces_change =
				p->pieces_have != oldp->pieces_have
					? p->pieces_have - oldp->pieces_have
					: 1;
			uint64_t const bytes_change = pieces_change * d->piece_size;
			uint64_t const time_ns_change =
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
			if (0 == strcmp(field->key, "index"))
				file_index = atoi(field->val.str) - 1;
			else if (0 == strcmp(field->key, "servers"))
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
			if (0 == u->num_servers)
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
			uint32_t i;

			for (i = 0; i < f->num_uris; ++i) {
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

	d->num_files = json_len(node);
	d->num_selfiles = 0;
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
				index = atoi(field->val.str) - 1;
				break;

			case K_file_path:
				file.path = strlen(field->val.str) > 0 ? strdup(field->val.str) : NULL;
				break;

			case K_file_length:
				file.total = strtoull(field->val.str, NULL, 10);
				break;

			case K_file_completedLength:
				file.have = strtoull(field->val.str, NULL, 10);
				break;

			case K_file_selected:
				file.selected = IS_TRUE(field->val.str);
				d->num_selfiles += file.selected;
				break;

			case K_file_uris:
			{
				JSONNode const *uris;
				uint32_t uri_index = 0;

				file.num_uris = json_len(field);
				file.uris = malloc(file.num_uris * sizeof *(file.uris));

				for (uris = json_first(field); uris; uris = json_next(uris)) {
					JSONNode const *field = json_children(uris);
					URI u;

					u.num_servers = 0;
					u.servers = NULL;
					do {
						if (0 == strcmp(field->key, "status")) {
							if (0 == strcmp(field->val.str, "used"))
								u.status = URI_STATUS_USED;
							else if (0 == strcmp(field->val.str, "waiting"))
								u.status = URI_STATUS_WAITING;
							else
								assert(0);
						} else if (0 == strcmp(field->key, "uri")) {
							assert(field->val.str);
							u.uri = strdup(field->val.str);
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
	for (uint8_t i = 0; i < 16; ++i) {
		char const c = str[i];

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
filter_download(Download *d, Download **dd)
{
	/* no --selects means all downloads */
	if (0 == action.num_sel)
		return true;

	for (size_t i = 0; i < action.num_sel; ++i) {
		char const *const sel = action.sel[i];

		if (is_gid(sel)) {
			/* test for matching GID */
			if (0 == memcmp(sel, d->gid, sizeof d->gid))
				return true;
		} else {
			/* test for matching path prefix */
			size_t const sel_size = strlen(sel);

			/* missing data can be anything */
			if (0 == d->num_files || !d->files)
				return true;

			for (size_t j = 0; j < d->num_files; ++j) {
				File const *f = &d->files[j];

				if (f->path && 0 == strncmp(f->path, sel, sel_size))
					return true;
			}
		}
	}

	/* no selectors matched */
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

		case K_download_bitfield:
			if (d->have != d->total)
				parse_progress(d, &d->progress, field->val.str);
			else
				free(d->progress), d->progress = NULL;
			break;

		case K_download_bittorrent:
		{
			JSONNode const *bt_info, *bt_name;

			free(d->name), d->name = NULL;

			if ((bt_info = json_get(field, "info")) &&
			    (bt_name = json_get(bt_info, "name")))
				d->name = strdup(bt_name->val.str);
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

			int8_t const new_status = STATUS_MAP[K_status_parse(field->val.str)];

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
		case K_download_belongsTo:
		{
			Download **dd = get_download_by_gid(field->val.str);

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
	}
}

typedef void(*parse_options_cb)(char const *, char const *, void *);

static void
parse_options(JSONNode const *node, parse_options_cb cb, void *arg)
{
	for (node = json_first(node); node; node = json_next(node))
		cb(node->key, node->val.str, arg);
}

static void
parse_global_stat(JSONNode const *node)
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
	} while ((node = json_next(node)));
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
	Download *download;
	bool has_get_peers: 1;
	bool has_get_servers: 1;
	bool has_get_options: 1;
	bool has_get_position: 1;
};

static void
parse_downloads(JSONNode const *result, struct update_arg *arg)
{
	bool some_insufficient = false;

	for (Download *d; result;
	       result = json_next(result),
	       unref_download(d), ++arg)
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

		if (json_obj == json_type(result)) {
			Download **dd;

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

		if (arg->has_get_position) {
			result = json_next(result);
			if (json_obj == json_type(result)) {
				error_handler(result);
			} else {
				node = json_children(result);
				d->queue_index = node->val.num;
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
update_handler(JSONNode const *result, struct update_arg *arg)
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
	set_periodic_update();

	free(arg);
}

static int
getmainheight(void)
{
	return LINES - 1/*status line*/;
}

static void
update(void)
{
	/* request update only if it was scheduled */
	if (update != periodic)
		return;

	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)update_handler;

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
		Download **top;
		Download **bot;
		Download **end = &downloads[num_downloads];
		Download **dd = downloads;
		struct update_arg *arg;

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
			uint8_t keyidx = 0;
			char *keys[20];

#define WANT(key) keys[keyidx++] = key;

			if (!d->initialized) {
				WANT("bittorrent");
				WANT("numPieces");
				WANT("pieceLength");

				WANT("following");
				WANT("belongsTo");

				d->initialized = true;
			} else {
				if ((0 == d->num_files && (!d->name || (visible && D_ACTIVE != d->status && is_local))) ||
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
			    (D_ACTIVE == abs(d->status) || d->status < 0) &&
			    (d->name
			    ? (arg->has_get_peers = ('p' == view))
			    : (arg->has_get_servers = ('f' == view)))) {
				json_write_beginobj(jw);
				json_write_key(jw, "methodName");
				json_write_str(jw, d->name ? "aria2.getPeers" : "aria2.getServers");
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

			if ((arg->has_get_position = (-D_WAITING == d->status))) {
				json_write_beginobj(jw);
				json_write_key(jw, "methodName");
				json_write_str(jw, "aria2.changePosition");
				json_write_key(jw, "params");
				json_write_beginarr(jw);
				/* “secret” */
				json_write_str(jw, secret_token);
				/* “gid” */
				json_write_str(jw, d->gid);
				/* “pos” */
				json_write_num(jw, 0);
				/* “how” */
				json_write_str(jw, "POS_CUR");
				json_write_endarr(jw);
				json_write_endobj(jw);
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
change_position_handler(JSONNode const *result, Download *d)
{
	if (result) {
		queue_changed();
		update();

		d->queue_index = result->val.num;
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

	rpc->handler = (RPCHandler)change_position_handler;
	rpc->arg = ref_download(d);

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
	char *how;
	switch (whence) {
	case SEEK_SET: how = "POS_SET"; break;
	case SEEK_CUR: how = "POS_CUR"; break;
	case SEEK_END: how = "POS_END"; break;
	default: abort();
	};
	json_write_str(jw, how);

	json_write_endarr(jw);

	do_rpc(rpc);
}

static void
shutdown_handler(JSONNode const *result, struct update_arg *arg)
{
	(void)arg;

	if (!result)
		return;

	exit(EXIT_SUCCESS);
}

static void
shutdown_aria(int force)
{
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)shutdown_handler;

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
action_exit(JSONNode const *result, void *arg)
{
	(void)result, (void)arg;
	exit(EXIT_SUCCESS);
}

static void
pause_download(Download *d, bool pause, bool force)
{
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	if (ACT_VISUAL != action.kind)
		rpc->handler = action_exit;

	if (0 == action.num_sel || d) {
		json_write_key(jw, "method");

		json_write_str(jw,
			d
			? (pause ? (force ? "aria2.forcePause" : "aria2.pause") : "aria2.unpause")
			: (pause ? (force ? "aria2.forcePauseAll" : "aria2.pauseAll") : "aria2.unpauseAll"));

		json_write_key(jw, "params");
		json_write_beginarr(jw);

		/* “secret” */
		json_write_str(jw, secret_token);
		if (d) {
			/* “gid” */
			json_write_str(jw, d->gid);
		}

		json_write_endarr(jw);
	} else {
		Download **dd = downloads;
		Download **const end = &downloads[num_downloads];

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
purge_download_handler(JSONNode const *result, Download *d)
{
	if (result) {
		Download **dd;

		if (d) {
			if (upgrade_download(d, &dd))
				delete_download_at(dd);
		} else {
			Download **end = &downloads[num_downloads];

			for (dd = downloads; dd < end;) {
				switch (abs((*dd)->status)) {
				case D_COMPLETE:
				case D_ERROR:
				case D_REMOVED:
					delete_download_at(dd);
					--end;
					break;

				default:
					++dd;
				}
			}
		}

		on_downloads_change(true);
		refresh();
	}

	unref_download(d);
}

static void
purge_download(Download *d)
{
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)purge_download_handler;
	rpc->arg = ref_download(d);

	json_write_key(jw, "method");
	json_write_str(jw, d ? "aria2.removeDownloadResult" : "aria2.purgeDownloadResult");

	json_write_key(jw, "params");
	json_write_beginarr(jw);

	/* “secret” */
	json_write_str(jw, secret_token);
	if (d) {
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
draw_file(Download const *d, size_t i, int *y, uint64_t offset)
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
	mvprintw((*y)++, 0, "  %6" PRIuPTR ": ", i + 1);

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

	char const *name;
	if (f->path) {
		name = f->path;
		if (d->dir) {
			size_t const dir_size = strlen(d->dir);
			if (!strncmp(f->path, d->dir, dir_size) &&
			    '/' == f->path[dir_size]) {
				name = f->path + dir_size + 1;
				/* leading slash is misleading */
				while ('/' == *name)
					++name;
			}
		}
	} else {
		name = f->selected ? "(not downloaded yet)" : "(none)";
	}
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
			size_t i;
			uint64_t offset = 0;

			for (i = 0; i < d->num_files; ++i) {
				draw_file(d, i, &y, offset);
				offset += d->files[i].total;
			}

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
		int const usable_width =
			29 +
			(!d->tags ? tag_col_width : 0) +
			(0 < global.num_download_limited ? 5 : 0) +
			(0 < global.num_upload_limited ? 6 : 0);

		addstr("* ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		printw("%-*.*s", usable_width, usable_width, d->error_message && (strlen(d->error_message) <= 30 || VIEWS[1] == view) ? d->error_message : "");
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

	case D_COMPLETE:
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

	case D_ACTIVE:
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
		if (view != VIEWS[1] && d->error_message && strlen(d->error_message) > 30) {
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
		if (stickycurs && memcmp(selgid, downloads[selidx]->gid, sizeof selgid))
			selidx = get_download_by_gid(selgid) - downloads;
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
	int const height = VIEWS[1] == view ? getmainheight() : 1;

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
		update();
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

	printw("%c %4d/%d",
			view,
			0 < num_downloads ? selidx + 1 : 0, num_downloads);

	for (i = D_UNKNOWN; i < D_NB; ++i) {
		if (0 == global.num_perstatus[i])
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
			ws_isalive() ? "" : " (not connected)");

	if (error_message) {
		addstr(": ");
		attr_set(A_BOLD, COLOR_ERR, NULL);
		addstr(error_message);
		attr_set(A_NORMAL, 0, NULL);
	}

	clrtoeol();

	/* build bottom-right widgets from right-to-left */
	x = w;
	mvaddstr(y, x -= 1, " ");

	if (0 == global.uploaded_total)
		goto skip_upload;

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

	speed = 0 == action.num_sel ? global.download_speed : global.download_speed_total;
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
print_gid(Download const *d)
{
	fprintf(stdout, "%s\n", d->gid);
}

static void
foreach_download(void(*cb)(Download const *))
{
	Download **dd = downloads;
	Download **const end = &downloads[num_downloads];

	for (dd = downloads; dd < end; ++dd)
		cb(*dd);
}

static void
update_all_handler(JSONNode const *result, void *arg)
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

		if (json_arr != json_type(result)) {
			error_handler(result);
			continue;
		}

		downloads_list = json_children(result);

		for (node = json_first(downloads_list); node; node = json_next(node)) {
			Download *d;
			/* XXX: parse_download's belongsTo and followedBy may
			 * create the download before this update arrives, so
			 * we check if only we modified the list */
			Download **dd = download_index == num_downloads
				? new_download()
				: get_download_by_gid(json_get(node, "gid")->val.str);
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

	switch (action.kind) {
	case ACT_VISUAL:
		/* no-op */
		break;

	case ACT_ADD_DOWNLOADS:
	case ACT_SHUTDOWN:
		/* handled earlier */
		break;

	case ACT_PRINT_GID:
		foreach_download(print_gid);
		exit(EXIT_SUCCESS);
		break;

	case ACT_PAUSE:
	case ACT_UNPAUSE:
		pause_download(NULL, ACT_PAUSE == action.kind, do_forced);
		break;

	case ACT_PURGE:
		purge_download(NULL);
		break;
	}

	if (ACT_VISUAL == action.kind) {
		on_downloads_change(0);
		refresh();

		/* schedule updates and request one now immediately */
		set_periodic_update();
		update();
	}
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
 * - <0: action did not run.
 * - =0: action executed and terminated successfully.
 * - >0: action executed but failed. */
static int
run_action(Download *d, char const *name, ...)
{
	char filename[NAME_MAX];
	char filepath[PATH_MAX];
	va_list argptr;
	pid_t pid;
	int status;

	va_start(argptr, name);
	vsnprintf(filename, sizeof filename, name, argptr);
	va_end(argptr);

	if (!d && 0 < num_downloads)
		d = downloads[selidx];

	if (getenv("ARIA2T_CONFIG"))
		snprintf(filepath, sizeof filepath, "%s/actions/%s",
				getenv("ARIA2T_CONFIG"), filename);
	else if (getenv("HOME"))
		snprintf(filepath, sizeof filepath, "%s/.config/aria2t/actions/%s",
				getenv("HOME"), filename);
	else
		return -1;

	endwin();

	if (0 == (pid = vfork())) {
		execlp(filepath, filepath,
				d ? d->gid : "",
				session_file,
				NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, &status, 0) && errno == EINTR)
		;

	/* become foreground process */
	tcsetpgrp(STDERR_FILENO, getpgrp());
	begwin();

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
fileout(bool must_edit)
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
fetch_options_handler(JSONNode const *result, Download *d)
{
	if (result) {
		clear_error_message();

		if (!d || upgrade_download(d, NULL)) {
			parse_options(result, (parse_options_cb)parse_option, d);

			draw_main();
			refresh();
		}
	}

	unref_download(d);
}

static void
change_option_handler(JSONNode const *result, Download *d)
{
	if (result)
		fetch_options(d, false);

	unref_download(d);
}

/* show received download or global options to user */
static void
show_options_handler(JSONNode const *result, Download *d)
{
	FILE *f;
	ssize_t len;
	char *line;
	size_t linesiz;
	RPCRequest *rpc;
	int action;

	if (!result)
		goto out;

	clear_error_message();

	if (d && !upgrade_download(d, NULL))
		goto out;

	if (!(f = fopen(session_file, "w")))
		goto out;

	parse_options(result, (parse_options_cb)write_option, f);
	parse_options(result, (parse_options_cb)parse_option, d);

	fclose(f);

	if ((action = run_action(NULL, d ? "i" : "I")) < 0)
		action = fileout(false);

	if (EXIT_SUCCESS != action)
		goto out;

	if (!(f = fopen(session_file, "r")))
		goto out;

	if (!(rpc = new_rpc()))
		goto out_fclose;

	rpc->handler = (RPCHandler)change_option_handler;
	rpc->arg = ref_download(d);

	json_write_key(jw, "method");
	json_write_str(jw, d ? "aria2.changeOption" : "aria2.changeGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);

	/* “secret” */
	json_write_str(jw, secret_token);
	if (d)
		/* “gid” */
		json_write_str(jw, d->gid);
	json_write_beginobj(jw);

	line = NULL, linesiz = 0;
	while (-1 != (len = getline(&line, &linesiz, f))) {
		char *name, *value;

		name = line;
		if (!(value = strchr(name, '=')))
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
fetch_options(Download *d, bool user)
{
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)(user ? show_options_handler : fetch_options_handler);
	rpc->arg = ref_download(d);

	json_write_key(jw, "method");
	json_write_str(jw, d ? "aria2.getOption" : "aria2.getGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, secret_token);
	if (d) {
		/* “gid” */
		json_write_str(jw, d->gid);
	}
	json_write_endarr(jw);

	do_rpc(rpc);
}

static void
show_options(Download *d)
{
	fetch_options(d, true);
}

/* select (last) newly added download and write back errorneous files */
static void
add_downloads_handler(JSONNode const *result, void *arg)
{
	bool ok = true;

	(void)arg;

	if (!result)
		return;

	for (result = json_first(result); result; result = json_next(result)) {
		Download **dd;

		if (json_obj == json_type(result)) {
			ok = false;
			error_handler(result);
		} else {
			JSONNode const *gid = json_children(result);

			if (json_str == json_type(gid)) {
				/* single GID returned */
				if ((dd = get_download_by_gid(gid->val.str)))
					selidx = dd - downloads;
			} else {
				/* get first GID of the array */
				if ((dd = get_download_by_gid(json_children(gid)->val.str)))
					selidx = dd - downloads;
			}
		}
	}

	switch (action.kind) {
	case ACT_ADD_DOWNLOADS:
		exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
		break;

	case ACT_VISUAL:
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
	RPCRequest *rpc;
	FILE *f;
	char *line;
	size_t linesiz;
	ssize_t len;
	int action;

	if ('\0' != cmd) {
		if ((action = run_action(NULL, "%c", cmd)) < 0)
			action = fileout(true);

		if (EXIT_SUCCESS != action)
			return;

		clear_error_message();

		if (!(f = fopen(session_file, "r")))
			return;
	} else {
		f = stdin;
	}

	if (!(rpc = new_rpc()))
		goto out_fclose;

	rpc->handler = (RPCHandler)add_downloads_handler;

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
		enum { KIND_URI, KIND_TORRENT, KIND_METALINK } kind;

		if (0 < len && line[len - 1] == '\n')
			line[--len] = '\0';

		if (0 == len)
			goto next;

		uri = line;
		if ((next_uri = strchr(uri, '\t')))
			*next_uri++ = '\0';

#define ISSUFFIX(lit) \
	((size_t)len > ((sizeof lit) - 1) && \
		 0 == memcmp(uri + (size_t)len - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (ISSUFFIX(".torrent"))
			kind = KIND_TORRENT;
		else if (ISSUFFIX(".meta4") || ISSUFFIX(".metalink"))
			kind = KIND_METALINK;
		else
			kind = KIND_URI;

		if (kind != KIND_URI && !(b64str = file_b64_enc(uri)))
			kind = KIND_URI;
#undef ISSUFFIX

		json_write_beginobj(jw);

		json_write_key(jw, "methodName");
		switch (kind) {
		case KIND_TORRENT:
			json_write_str(jw, "aria2.addTorrent");
			break;

		case KIND_METALINK:
			json_write_str(jw, "aria2.addMetalink");
			break;

		case KIND_URI:
			json_write_str(jw, "aria2.addUri");
			break;
		}

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, secret_token);
		/* “data” */
		switch (kind) {
		case KIND_TORRENT:
		case KIND_METALINK:
			json_write_str(jw, b64str);
			free(b64str);
			break;

		case KIND_URI:
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
		if (KIND_TORRENT == kind) {
			/* “uris” */
			json_write_beginarr(jw);
			while (next_uri) {
				uri = next_uri;
				if ((next_uri = strchr(uri, '\t')))
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

			if (!(value = strchr(name, '=')))
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
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
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
		if (ACT_VISUAL == action.kind) {
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
		if (ACT_VISUAL == action.kind ? is_local : action.uses_files)
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
remove_download_handler(JSONNode const *result, Download *d)
{
	if (result) {
		Download **dd;

		if (upgrade_download(d, &dd)) {
			delete_download_at(dd);

			on_downloads_change(1);
			refresh();
		}
	}

	unref_download(d);
}

static void
remove_download(Download *d, bool force)
{
	RPCRequest *rpc;

	if (0 <= run_action(d, "D"))
		return;

	if (D_ACTIVE == abs(d->status)) {
		set_error_message("refusing to delete active download");
		return;
	}

	if (!(rpc = new_rpc()))
		return;

	rpc->handler = (RPCHandler)remove_download_handler;
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
		view = VIEWS[next ? 1 : ARRAY_SIZE(VIEWS) - 2];
		tui.selidx = -1; /* force redraw */
	}

	update();
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
		 * Move selection downwards.
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
		 * Move selected download forward in the queue.
		 */
		case 'J':
		/*MAN(KEYS)
		 * .TP
		 * .B K
		 * Move selected download backward in the queue.
		 */
		case 'K':
			if (0 < num_downloads)
				change_position(downloads[selidx], 'J' == ch ? -1 : 1, SEEK_CUR);
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
				tui.selidx = -1; /* force redraw */
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
		 * example, it means that it will not contact to tracker(s).
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
				if (0 < num_downloads) {
					Download const *d = downloads[selidx];
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
			action.kind = ACT_VISUAL;
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

		case KEY_MOUSE:
			if (getmouse(&event) != OK)
				break;

#ifdef BUTTON5_PRESSED
			if (event.bstate & BUTTON4_PRESSED)
				selidx -= VIEWS[1] == view ? 5 : 1;
			else if (event.bstate & BUTTON5_PRESSED)
				selidx += VIEWS[1] == view ? 5 : 1;
			else
#endif
			if (((BUTTON1_PRESSED | BUTTON3_PRESSED) & event.bstate) &&
			    VIEWS[1] == view &&
			    event.y < getmainheight())
				selidx = topidx + event.y;

			draw_cursor();
			refresh();
			break;

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
	if ('\0' != session_file[0]) {
		unlink(session_file);
		session_file[0] = '\0';
	}
}

static void
remote_info_handler(JSONNode const *result, void *arg)
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
	parse_options(node, (parse_options_cb)parse_option, NULL);

	/* we have is_local correctly set, so we can start requesting downloads */

	switch (action.kind) {
	case ACT_ADD_DOWNLOADS:
		add_downloads('\0');
		break;

	case ACT_SHUTDOWN:
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
	RPCRequest *rpc;

	if (!(rpc = new_rpc()))
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

void
on_ws_open(void)
{
	pfds[1].fd = ws_fileno();
	pfds[1].events = POLLIN;

	periodic = NULL;

	clear_error_message();

	oldselidx = -1;
	oldtopidx = -1;

	is_local = false;
	remote_info();

	draw_all();
	refresh();
}

void
on_ws_close(void)
{
	pfds[1].fd = -1;

	if (0 == errno)
		exit(EXIT_SUCCESS);

	if (ACT_VISUAL != action.kind)
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
		if (!is_gid(action.sel[i])) {
			action.uses_files = true;
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
	sigset_t ss;

	sigemptyset(&ss);
	sigaddset(&ss, SIGWINCH);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGHUP);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGKILL);
	sigaddset(&ss, SIGQUIT);
	sigaddset(&ss, SIGPIPE);

	sigprocmask(SIG_BLOCK, &ss, NULL);

	/* when process signal handlers block every other signals */
	sigfillset(&sa.sa_mask);
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
	int argi;

	setlocale(LC_ALL, "");

	setup_sighandlers();

	for (argi = 1; argi < argc;) {
		char *arg = argv[argi++];
		if (0 == strcmp(arg, "--select")) {
			if (argc <= argi)
				goto show_usage;

			action.num_sel = 1;
			action.sel = &argv[argi];
			while (++argi, argv[argi] && '-' != *argv[argi])
				++action.num_sel;
		} else if (0 == strcmp(arg, "--print-gid")) {
			action.kind = ACT_PRINT_GID;
		} else if (0 == strcmp(arg, "--add")) {
			action.kind = ACT_ADD_DOWNLOADS;
		} else if (0 == strcmp(arg, "--shutdown")) {
			action.kind = ACT_SHUTDOWN;
		} else if (0 == strcmp(arg, "--pause")) {
			action.kind = ACT_PAUSE;
		} else if (0 == strcmp(arg, "--unpause")) {
			action.kind = ACT_UNPAUSE;
		} else if (0 == strcmp(arg, "--purge")) {
			action.kind = ACT_PURGE;
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

	/* atexit(clear_downloads); */
	atexit(cleanup_session_file);

	if (ACT_VISUAL == action.kind) {
		pfds[0].fd = STDIN_FILENO;
		pfds[0].events = POLLIN;

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
		/* listen for all mouse events */
		mousemask(ALL_MOUSE_EVENTS, NULL);
		/* be immediate */
		mouseinterval(0);

		update_terminfo();

		draw_all();
		refresh();
	} else {
		pfds[0].fd = -1;
	}

	try_connect();

	sigset_t ss;
	sigemptyset(&ss);

	for (;;) {
		switch (ppoll(pfds, ARRAY_SIZE(pfds), periodic ? &period : NULL, &ss)) {
		case 0:
			periodic();
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
				ws_read();
			break;
		}
	}
}
