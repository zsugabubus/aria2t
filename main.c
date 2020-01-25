#define _XOPEN_SOURCE_EXTENDED
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#ifdef __linux__
#include <sys/signalfd.h>
#include <sys/prctl.h>
#endif
#include <sys/wait.h>

#include <locale.h>
#include <ncurses.h>
#include "websocket.h"
#include "aria.h"
#include "format.h"
#include "jeezson/jeezson.h"

#define ctrl(x) ((x) & 0x1f)
#define array_len(arr) (sizeof arr / sizeof *arr)

static struct json_node *nodes;
static size_t nnodes;
static char *aria_token;
typedef int(*rpc_msg_handler)(struct json_node *result, void *data);
static struct rpc_handler {
	rpc_msg_handler func;
	void *data;
} rpc_handlers[10];

static int namecol;
static int retcode = EXIT_SUCCESS;

static struct json_writer jw[1];

static struct aria_globalstat globalstat;
static struct aria_download *downloads;
static size_t num_downloads;
static size_t selidx = SIZE_MAX;
static size_t firstidx = 0;

static char const *program_name;

#define COLOR_DOWN 1
#define COLOR_UP   2
#define COLOR_CONN 3
#define COLOR_EOF  4

void shellout() {
	char *editor;
	pid_t pid;
	char filepath[256];
	char *tmp = getenv("TMP");
	int file;
#ifdef __linux__
	int ppid = getpid();
#endif

	def_prog_mode();
	endwin();

	if (NULL == (editor = getenv("EDITOR")))
		assert(0);

	snprintf(filepath, sizeof filepath, "/%s/%s.XXXXXX",
			NULL != tmp ? tmp : "tmp",
			program_name);
	if (-1 == (file = mkstemp(filepath)))
		assert(0);

	if (0 == (pid = fork())) {
#ifdef __linux__
		(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
		/* Has been reparented since. */
		if (getppid() != ppid)
			raise(SIGKILL);
#endif

		execlp(editor, editor, filepath, NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, NULL, 0) && errno == EINTR)
		;

	unlink(filepath);

	refresh();
}

static int ar_redraw_globalstat(void) {
	int y, x, w;
	char fmtbuf[5];
	int n;

	getmaxyx(stdscr, y, w);

	--y, x = 0;

	move(y, 0);
	/* Downloads number widget. */
	attr_set(A_NORMAL, 0, NULL);
	addstr(" 契");

	n = fmt_number(fmtbuf, globalstat.num_active);
	addnstr(fmtbuf, n);

	if (globalstat.num_waiting > 0) {
		attr_set(A_NORMAL, 0, NULL);
		addstr("+");

		n = fmt_number(fmtbuf, globalstat.num_waiting);
		addnstr(fmtbuf, n);
	}

	addstr(" 栗");

	n = fmt_number(fmtbuf, globalstat.num_stopped);
	addnstr(fmtbuf, n);

	if (globalstat.num_stopped_total > globalstat.num_stopped) {
		attr_set(A_NORMAL, 0, NULL);
		addstr("+");

		n = fmt_number(fmtbuf, globalstat.num_stopped_total - globalstat.num_stopped);
		addnstr(fmtbuf, n);
	}
	attr_set(A_NORMAL, 0, NULL);

	clrtoeol();

	x = w;
	mvaddstr(y, x -= 1, " ");

	/* Upload speed widget. */
	if (globalstat.upload_speed > 0)
		attr_set(A_BOLD, COLOR_UP, NULL);
	n = fmt_speed(fmtbuf, globalstat.upload_speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, "  ");
	attr_set(A_NORMAL, 0, NULL);

	/* Download speed widget. */
	if (globalstat.download_speed > 0)
		attr_set(A_BOLD, COLOR_DOWN, NULL);
	n = fmt_speed(fmtbuf, globalstat.download_speed);
	mvaddnstr(y, x -= n, fmtbuf, n);

	mvaddstr(y, x -= 3, "  ");
	attr_set(A_NORMAL, 0, NULL);

	return 0;
}

static int download_comparator(struct aria_download const*this, struct aria_download const*other) {
	int cmp;
	uint64_t x, y;
	int this_status = abs(this->status);
	int other_status = abs(other->status);

	/* Prefer removed downloads. */
	cmp = (DOWNLOAD_REMOVED == other_status) -
		(DOWNLOAD_REMOVED == this_status);
	if (cmp)
		return cmp;

	/* Prefer incomplete downloads. */
	cmp = (DOWNLOAD_ACTIVE == other_status) -
		(DOWNLOAD_ACTIVE == this_status);
	if (cmp)
		return cmp;

	/* Prefer active downloads that almost downloaded. */
	/* NOTE: total_size can be null for example in cases if there is no
	 * information about the download. */
	x = DOWNLOAD_ACTIVE == this_status && this->total_size > 0
		? this->have * 10000 / this->total_size
		: 0;
	y = DOWNLOAD_ACTIVE == other_status && other->total_size > 0
		? other->have * 10000 / other->total_size
		: 0;
	if (x != y)
		return x > y ? -1 : 1;

	/* Prefer waiting downloads. */
	cmp = (DOWNLOAD_WAITING == other_status) -
		(DOWNLOAD_WAITING == this_status);
	if (cmp)
		return cmp;

	/* Prefer downloads higher upload speed. */
	if (this->upload_speed != other->upload_speed)
		return this->upload_speed > other->upload_speed ? -1 : 1;

	/* Prefer downloads with better upload ratio. */
	x = this->total_size > 0
		? this->uploaded * 10000 / this->total_size
		: 0;
	y = other->total_size > 0
		? other->uploaded * 10000 / other->total_size
		: 0;
	if (x != y)
		return x > y ? -1 : 1;

	/* Prefer completed and active downloads. */
	cmp = (DOWNLOAD_COMPLETE == other_status ||
		DOWNLOAD_ACTIVE == other_status) -
		(DOWNLOAD_COMPLETE == this_status ||
		DOWNLOAD_ACTIVE == this_status);
	if (cmp)
		return -cmp;

	return strcmp(this->name, other->name);
}

static struct aria_download *find_download_bygid(char *gid) {
	struct aria_download *d = downloads;
	struct aria_download *const end = &downloads[num_downloads];

	for (;d < end; ++d)
		if (0 == memcmp(d->gid, gid, sizeof d->gid))
			return d;
	return NULL;
}

static void ar_redraw(void);

static void ar_scroll_changed(void) {
	ar_redraw();
}

static void ar_cursor_changed(void) {
	int show = num_downloads > 0 && selidx != SIZE_MAX;
	int h = getmaxy(stdscr);
	size_t oldfirstidx = firstidx;

	(void)curs_set(show);

	if (!show)
		return;

	/* Scroll selection into view. */
	if (selidx < firstidx)
		firstidx = selidx;
	if (firstidx + h - 1 <= selidx)
		firstidx = selidx - (h - 2);
	if (firstidx + h - 1 >= num_downloads)
		firstidx = num_downloads > (size_t)(h - 1) ? num_downloads - (size_t)(h - 1) : 0;

	if (firstidx == oldfirstidx) {
		move(selidx - firstidx, namecol);
	} else {
		ar_scroll_changed();
	}
}

static void ar_downloads_changed(void) {
	char selgid[sizeof ((struct aria_download *)0)->gid];

	if (SIZE_MAX != selidx)
		memcpy(selgid, downloads[selidx].gid, sizeof selgid);

	qsort(downloads, num_downloads, sizeof *downloads, (int(*)(void const*,void const*))download_comparator);

	if (SIZE_MAX != selidx &&
		0 != memcmp(selgid, downloads[selidx].gid, sizeof selgid)) {
		struct aria_download *d = find_download_bygid(selgid);

		assert(NULL != d);
		selidx = d - downloads;
	}

	ar_redraw();
}


static void ar_redraw_download(struct aria_download *d, int y) {
	int x;
	char fmtbuf[5];
	int n;

	if (num_downloads < 10)
		n = 2;
	else if (num_downloads < 100)
		n = 3;
	else if (num_downloads < 1000)
		n = 4;
	else if (num_downloads < 10000)
		n = 5;
	else
		n = 7;

	attr_set(A_NORMAL, -1, NULL);
	x = 0;
	mvaddnstr(y, x, d->gid, n), x += n;
	mvaddstr(y, x, " "), x += 1;

	attr_set(A_BOLD, -1, NULL);
	switch (abs(d->status)) {
	case DOWNLOAD_REMOVED:
		mvaddstr(y, x, "x "), x += 2;
		attr_set(A_NORMAL, 0, NULL);
		break;
	case DOWNLOAD_ERROR:
		mvaddstr(y, x, "E "), x += 2;
		attr_set(A_NORMAL, 0, NULL);
		break;
		for (;;) {
	case DOWNLOAD_WAITING:
			mvaddstr(y, x, "w ");
			break;
	case DOWNLOAD_PAUSED:
			mvaddstr(y, x, "| ");
			break;
	case DOWNLOAD_COMPLETE:
			mvaddstr(y, x, ". ");
			break;
		}
		x += 2;
		attr_set(A_NORMAL, 0, NULL);

		n = fmt_space(fmtbuf, d->total_size);
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, "["), x += 1;

		n = fmt_percent(fmtbuf, d->have, d->total_size);
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, "] ("), x += 3;

		if (d->num_files > 0) {
			n = fmt_number(fmtbuf, d->num_files);
		} else {
			fmtbuf[0] = ' ';
			fmtbuf[1] = ' ';
			fmtbuf[2] = ' ';
			fmtbuf[3] = '?';
			n = 4;
		}
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, " files)"), x += 7;

		mvaddstr(y, x, "       "), x += 3 + 3;
		break;
	case DOWNLOAD_ACTIVE:
		mvaddstr(y, x, "> ");
		attr_set(A_NORMAL, 0, NULL), x += 2;

		if (d->download_speed > 0) {
			attr_set(A_BOLD, COLOR_DOWN, NULL);

			n = fmt_percent(fmtbuf, d->have, d->total_size);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			attr_set(A_NORMAL, 0, NULL);

			mvaddstr(y, x, " @ "), x += 3;

			attr_set(A_BOLD, COLOR_DOWN, NULL);
			mvaddstr(y, x, "  "), x += 3;

			n = fmt_speed(fmtbuf, d->download_speed);
			mvaddnstr(y, x, fmtbuf, n), x += n;
			/* mvaddstr(y, x, "/"); */
			attr_set(A_NORMAL, -1, NULL);
		} else {
			n = fmt_space(fmtbuf, d->total_size);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			mvaddstr(y, x, "["), x += 1;

			attr_set(d->have == d->total_size ? A_NORMAL : A_BOLD, COLOR_DOWN, NULL);

			n = fmt_percent(fmtbuf, d->have, d->total_size);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			attr_set(A_NORMAL, 0, NULL);

			mvaddstr(y, x, "] "), x += 2;

			/* if (d->num_seeders > 0)
				n = fmt_number(fmtbuf, d->num_seeders);
			else
				n = fmt_white(fmtbuf, 4);
			mvaddnstr(y, x, fmtbuf, n);
			x += n; */

			attr_set(A_NORMAL, COLOR_CONN, NULL);
			if (d->num_connections > 0) {
				n = fmt_number(fmtbuf, d->num_connections);
				mvaddnstr(y, x, fmtbuf, n), x += n;
			} else {
				mvaddstr(y, x, "    "), x += 4;
			}

			attr_set(A_NORMAL, 0, NULL);
		}

		if (d->uploaded > 0) {
			if (d->upload_speed > 0)
				attr_set(A_BOLD, COLOR_UP, NULL);
			mvaddstr(y, x, "  "), x += 3;

			n = fmt_space(fmtbuf, d->uploaded);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			if (d->upload_speed > 0) {
				mvaddstr(y, x, " @ "), x += 3;

				n = fmt_speed(fmtbuf, d->upload_speed);
				mvaddnstr(y, x, fmtbuf, n), x += n;
			} else {
				mvaddstr(y, x, "["), x += 1;

				attr_set(A_NORMAL, COLOR_UP, NULL);

				n = fmt_percent(fmtbuf, d->uploaded, d->total_size);
				mvaddnstr(y, x, fmtbuf, n), x += n;

				attr_set(A_NORMAL, 0, NULL);

				mvaddstr(y, x, "]"), x += 1;
			}

			if (d->upload_speed > 0)
				attr_set(A_NORMAL, -1, NULL);
		} else {
			mvaddstr(y, x, "              "), x += 3 + 4 + 3 + 4;
		}

		break;
	}

	mvaddstr(y, x, " "), x += 1;
	namecol = x;
	mvaddstr(y, x, d->name);
	clrtoeol();
}

static void ar_redraw(void) {
	int line = 0, height = getmaxy(stdscr) - 1/*status line*/;

	for (;line < height; ++line) {
		if (firstidx + line < num_downloads) {
			ar_redraw_download(&downloads[firstidx + line], line);
		} else {
			attr_set(A_NORMAL, COLOR_EOF, NULL);
			mvaddstr(line, 0, "~");
			attr_set(A_NORMAL, 0, NULL);
			clrtoeol();
		}
	}

	ar_redraw_globalstat();

	ar_cursor_changed();
}

static struct aria_download *download_alloc(void) {
	void *p = realloc(downloads, (num_downloads + 1) * sizeof *downloads);

	if (NULL == p)
		return NULL;

	return memset(&(downloads = p)[num_downloads++], 0, sizeof *downloads);
}

char const *NONAME = "[No Name]";

static void rpc_parse_download_files(struct aria_download *d, struct json_node *node) {
	free(d->files);

	d->files = malloc((d->num_files = json_len(node)) * sizeof *(d->files));
	if (NULL == d->files)
		return;

	node = json_children(node);
	do {
		struct json_node *field = json_children(node);
		struct aria_file file;
		size_t index = index;

		do {
			if (0 == strcmp(field->key, "index"))
				index = (size_t)field->val.num;
			else if (0 == strcmp(field->key, "path"))
				file.path = strdup(field->val.str);
			else if (0 == strcmp(field->key, "length"))
				file.total_size = strtoull(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "completedLength"))
				file.have = strtoull(field->val.str, NULL, 10);
			else if (0 == strcmp(field->key, "selected"))
				file.selected = json_true == json_type(field);
			else if (0 == strcmp(field->key, "uris"))
				;
			else
				assert(!"unknown key in file");
		} while (NULL != (field = json_next(field)));

		d->files[index] = file;
	} while (NULL != (node = json_next(node)));
}

static void rpc_parse_download(struct aria_download *d, struct json_node *node) {
	struct json_node *field = json_children(node);

	do {
		if (0 == strcmp(field->key, "gid")) {
			assert(strlen(field->val.str) == sizeof d->gid - 1);
			memcpy(d->gid, field->val.str, sizeof d->gid);
		} else if (0 == strcmp(field->key, "files")) {
			rpc_parse_download_files(d, field);
			if (NULL == d->files)
				continue;

			if (NULL == d->name || NONAME == d->name) {
				char *path = d->files[0].path;
				char *last_slash = strrchr(path, '/');

				d->name = strdup(NULL != last_slash ? last_slash + 1 : d->files[0].path);
			}

		} else if (0 == strcmp(field->key, "bittorrent")) {
			struct json_node *bt_info, *bt_name;

			if (NULL == (bt_info = json_get(field, "info")))
				continue;

			if (NULL == (bt_name = json_get(bt_info, "name")))
				continue;

			if (NONAME != d->name)
				free((char *)d->name);
			d->name = strdup(bt_name->val.str);
		} else if (0 == strcmp(field->key, "status")) {
#define IS(statusstr, STATUS) if (0 == strcmp(field->val.str, #statusstr)) d->status = -DOWNLOAD_##STATUS
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
		else_if_FIELD(total_size, "totalLength", ull)
		else_if_FIELD(have, "completedLength", ull)
		else_if_FIELD(uploaded, "uploadLength", ull)
		/* else_if_FIELD(num_seeders, "numSeeders", ul) */
		else_if_FIELD(num_connections, "connections", ul)
#undef else_if_FIELD
	} while (NULL != (field = json_next(field)));

	assert(strlen(d->gid) == 16);
}

static int rpc_on_error(struct json_node *error) {
	struct json_node const *code = json_get(error, "code");
	struct json_node const *message = json_get(error, "message");

	def_prog_mode();
	endwin();
	fflush(stdout);

	fprintf(stderr, "%s: %s:%u: RPC error %d: “%s”\n",
			program_name,
			ws_host, ws_port,
			(int)code->val.num, message->val.str);
	fflush(stderr);

	getchar();

	reset_prog_mode();
	flushinp();
	refresh();

	return 0;
}

static int rpc_on_action(struct json_node *result, void *data) {
	(void)result, (void)data;
	return 0;
}

static int rpc_on_populate(struct json_node *result, void *data) {
	(void)data;

	result = json_children(result);
	do {
		struct json_node *downloads_list;
		struct json_node *node;

		if (json_arr != json_type(result)) {
			rpc_on_error(result);
			continue;
		}

		downloads_list = json_children(result);

		if (json_isempty(downloads_list))
			continue;

		node = json_children(downloads_list);
		do {
			struct aria_download *d = download_alloc();
			if (NULL == d)
				continue;

			rpc_parse_download(d, node);
		} while (NULL != (node = json_next(node)));

	} while (NULL != (result = json_next(result)));

	ar_downloads_changed();
	refresh();

	return 0;
}

static void rpc_parse_globalstat(struct json_node *node) {
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
		else
			assert(!"unknown key in global stat");
	} while (NULL != (node = json_next(node)));
}

int rpc_on_periodic(struct json_node *result, void *data) {
	int any_changed = 0;

	(void)data;

	/* Result is an array. Go to the first element. */
	result = json_children(result);
	rpc_parse_globalstat(result);

	result = json_next(result);
	/* No more repsonses if no downloads. */
	for (;NULL != result; result = json_next(result)) {
		struct json_node *node;
		struct aria_download *d;

		if (json_obj == json_type(result)) {
			assert(0);
			printf("%s\n", json_tostring(result));
			/* REMOVE torrent, error */
			continue;
		}

		node = json_children(result);

		d = find_download_bygid(json_get(node, "gid")->val.str);

		if (NULL == d)
			d = download_alloc();
		if (NULL != d) {
			rpc_parse_download(d, node);
			any_changed = 1;
		}
	}

	if (any_changed) {
		ar_downloads_changed();
		refresh();
	}

	return 0;
}

static struct rpc_handler *handler_alloc(void) {
	size_t n = array_len(rpc_handlers);

	while (n > 0)
		if (NULL == rpc_handlers[--n].func)
			return &rpc_handlers[n];

	return NULL;
}

static void handler_free(struct rpc_handler *handler) {
	handler->func = NULL;
	free(handler->data);
}

static int rpc_on_notification(char const *method, struct json_node *event) {
	char *const gid = json_get(event, "gid")->val.str;
	struct aria_download *d = find_download_bygid(gid);

	if (0 == strcmp(method, "aria2.onDownloadStart")) {
		if (NULL == d) {
			d = download_alloc();
			if (NULL != d)
				memcpy(d->gid, gid, sizeof d->gid);
		}
		if (NULL != d)
			d->status = -DOWNLOAD_ACTIVE;

	} else if (0 == strcmp(method, "aria2.onDownloadPause")) {
		if (NULL != d)
			d->status = -DOWNLOAD_PAUSED;

	} else if (0 == strcmp(method, "aria2.onDownloadStop")) {
		if (NULL != d)
			d->status = -DOWNLOAD_PAUSED;

	} else if (0 == strcmp(method, "aria2.onDownloadComplete")) {
		if (NULL != d)
			d->status = -DOWNLOAD_COMPLETE;

	} else if (0 == strcmp(method, "aria2.onDownloadError")) {
		if (NULL != d)
			d->status = -DOWNLOAD_ERROR;

	} else if (0 == strcmp(method, "aria2.onBtDownloadComplete")) {
		if (NULL != d)
			d->status = -DOWNLOAD_COMPLETE;

	} else {
		assert(0);
		return 1;
	}

	ar_downloads_changed();
	refresh();

	return 0;
}

int on_ws_message(char *msg, size_t msglen){
	struct json_node *id;
	struct json_node *method;
	int r;

	(void)msglen;

	json_parse(msg, &nodes, &nnodes);

	if (NULL != (id = json_get(nodes, "id"))) {
		struct json_node *const result = json_get(nodes, "result");
		struct rpc_handler *const handler = &rpc_handlers[(unsigned)id->val.num];

		if (NULL != result)
			r = handler->func(result, handler->data);
		else
			r = rpc_on_error(json_get(nodes, "error"));

		handler_free(handler);
		if (r)
			return r;
	} else if (NULL != (method = json_get(nodes, "method"))) {
		struct json_node *const params = json_get(nodes, "params");

		if ((r = rpc_on_notification(method->val.str, params + 1)))
			return r;
	} else {
		assert(0);
	}

	/* json_debug(nodes, 0); */
	/* json_debug(json_get(json_get(nodes, "result"), "version"), 0); */
	return 0;
}

static struct rpc_handler *writer_prolog() {
	json_writer_init(jw);
	json_write_beginobj(jw);

	json_write_key(jw, "jsonrpc");
	json_write_str(jw, "2.0");

	json_write_key(jw, "id");
	return handler_alloc();
}

static void writer_epilog() {
	json_write_endobj(jw);

	ws_write(jw->buf, jw->len);
	json_writer_term(jw);

	jw->buf = NULL;
	json_writer_free(jw);
}

static int ar_populate(void) {
	unsigned n;
	struct rpc_handler *handler = writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = rpc_on_populate;
	handler->data = NULL;
	json_write_int(jw, (int)(handler - rpc_handlers));

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
			tell = "aria2.tellActive";
			pageable = 0;
			break;
		case 1:
			tell = "aria2.tellWaiting";
			pageable = 1;
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
		json_write_str(jw, aria_token);

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
		/* json_write_str(jw, "verifiedLength");
		json_write_str(jw, "verifyIntegrityPending"); */
		json_write_str(jw, "uploadSpeed");
		json_write_str(jw, "downloadSpeed");
		json_write_str(jw, "errorCode");
		/* json_write_str(jw, "numSeeders"); */
		json_write_str(jw, "connections");
		json_write_str(jw, "seeder");
		json_write_str(jw, "bittorrent");
		json_write_str(jw, "files");
		json_write_endarr(jw);
		/* }}} */
		json_write_endarr(jw);

		json_write_endobj(jw);
	}
out_of_loop:

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	writer_epilog();
	return 0;
}

static int periodic(void) {
	struct rpc_handler *handler = writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = rpc_on_periodic;
	handler->data = NULL;
	json_write_int(jw, (int)(handler - rpc_handlers));

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
		json_write_str(jw, aria_token);
		json_write_endarr(jw);
		json_write_endobj(jw);
	}
	{
		struct aria_download *d = &downloads[firstidx];
		int n = getmaxy(stdscr) - 1;

		if (firstidx + n > num_downloads)
			n = num_downloads - firstidx - 1;

		for (;n-- > 0; ++d) {
			json_write_beginobj(jw);
			json_write_key(jw, "methodName");
			json_write_str(jw, "aria2.tellStatus");
			json_write_key(jw, "params");
			json_write_beginarr(jw);
			/* “secret” */
			json_write_str(jw, aria_token);
			/* “gid” */
			json_write_str(jw, d->gid);
			/* “keys” */
			json_write_beginarr(jw);

			if (DOWNLOAD_REMOVED == abs(d->status))
				continue;

			json_write_str(jw, "gid");
			json_write_str(jw, "status");
			if (NULL == d->name) {
				d->name = NONAME;
				json_write_str(jw, "totalLength");
				json_write_str(jw, "completedLength");
				json_write_str(jw, "uploadLength");

				json_write_str(jw, "seeder");
				json_write_str(jw, "bittorrent");
			} else if (NONAME == d->name) {
				/* “bittorrent.info.name” was empty. Assign the
				 * name of the first file as name. */
				json_write_str(jw, "files");
			}

			if (d->status < 0 && d->total_size == 0) {
				json_write_str(jw, "totalLength");
				json_write_str(jw, "completedLength");
				json_write_str(jw, "uploadLength");
				json_write_str(jw, "uploadSpeed");
			}

			switch (abs(d->status)) {
			case DOWNLOAD_ERROR:
				if (NULL == d->progress)
					json_write_str(jw, "errorMessage");
				break;
			case DOWNLOAD_WAITING:
			case DOWNLOAD_PAUSED:
				break;
			default:
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
				/* json_write_str(jw, "numSeeders"); */
				break;
			}
			d->status = abs(d->status);

			json_write_endarr(jw);

			json_write_endarr(jw);
			json_write_endobj(jw);
		}
	}

	json_write_endarr(jw); /* }}} */

	json_write_endarr(jw); /* }}} */

	writer_epilog();
	return 0;
}

static int ar_pauseall(int pause) {
	struct rpc_handler *handler = writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = rpc_on_action;
	handler->data = NULL;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? "aria2.pauseAll" : "aria2.unpauseAll");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, aria_token);
	json_write_endarr(jw);

	writer_epilog();
	return 0;
}

static int ar_download_pause(struct aria_download *d, int pause) {
	struct rpc_handler *handler = writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = rpc_on_action;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? "aria2.pause" : "aria2.unpause");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, aria_token);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	writer_epilog();

	return 0;
}

static int ar_shutdown() {
	struct rpc_handler *handler = writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = rpc_on_action;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, "aria2.shutdown");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, aria_token);
	json_write_endarr(jw);

	writer_epilog();

	return 0;
}

static int stdin_read(void) {
	int ch;

	while (ERR != (ch = getch())) {
		switch (ch) {
		case 'k':
		case KEY_UP:
			if (SIZE_MAX == selidx)
				selidx = num_downloads - 1;
			else if (selidx > 0)
				--selidx;
			else
				break;

			ar_cursor_changed();
			refresh();
			break;

		case 'P':
			if (SIZE_MAX == selidx)
				break;

			(void)ar_pauseall(DOWNLOAD_PAUSED != abs(downloads[selidx].status));
			break;

		case 'g':
			if (0 == num_downloads)
				break;

			selidx = 0;
			ar_cursor_changed();
			refresh();
			break;

		case 'G':
			if (0 == num_downloads)
				break;

			selidx = num_downloads - 1;
			ar_cursor_changed();
			refresh();
			break;

		case ' ':
		case 'p':
			if (SIZE_MAX == selidx)
				break;

			(void)ar_download_pause(&downloads[selidx], DOWNLOAD_PAUSED != abs(downloads[selidx].status));
			break;

		case 'j':
		case KEY_DOWN:
			if (SIZE_MAX == selidx && num_downloads > 0)
				selidx = 0;
			else if (selidx + 1 < num_downloads)
				++selidx;
			else
				break;

			ar_cursor_changed();
			refresh();
			break;

		case 'a':
			shellout();
			break;

		case 'q':
			retcode = EXIT_SUCCESS;
			return 1;

		/* case 'X':
			ar_delete_removed();
			return 1; */

		case KEY_F(9):
			ar_shutdown();
			break;

		case ctrl('c'):
			return 1;
		}

	}

	return 0;
}

static int load_cfg(void) {
	char *str;

	if (NULL == (ws_host = getenv("ARIA_RPC_HOST")))
		ws_host = "127.0.0.1";

	str = getenv("ARIA_RPC_PORT");
	if (NULL == str ||
		(errno = 0, ws_port = strtoul(str, NULL, 10), errno))
		ws_port = 6801;

	/* “token:secret” */
	(str = getenv("ARIA_RPC_SECRET")) || (str = "");
	aria_token = malloc(snprintf(NULL, 0, "token:%s", str));
	if (NULL == aria_token) {
		fprintf(stderr, "%s: Failed to allocate memory.\n",
				program_name);
		return -1;
	}

	(void)sprintf(aria_token, "token:%s", str);
	return 0;
}

static void fill_pairs(void) {
	switch (NULL == getenv("NO_COLOR") ? COLORS : 0) {
	/* https://upload.wikimedia.org/wikipedia/commons/1/15/Xterm_256color_chart.svg */
	default:
	case 256:
		init_pair(COLOR_DOWN,  34,  -1);
		init_pair(COLOR_UP,    33,  -1);
		init_pair(COLOR_CONN, 133,  -1);
		init_pair(COLOR_EOF,  250,  -1);
		break;
	case 8:
		init_pair(COLOR_DOWN,   2,  -1);
		init_pair(COLOR_UP,     4,  -1);
		init_pair(COLOR_CONN,   5,  -1);
		init_pair(COLOR_EOF,    6,  -1);
		break;
	case 0:
		/* https://no-color.org/ */
		init_pair(COLOR_DOWN,   0,  -1);
		init_pair(COLOR_UP,     0,  -1);
		init_pair(COLOR_CONN,   0,  -1);
		init_pair(COLOR_EOF,    0,  -1);
		break;
	}
}

int main(int argc, char *argv[])
{
	sigset_t sigmask;
	struct pollfd fds[
#ifdef __linux__
		3
#else
		2
#endif
	];

	(void)argc, (void)argv;

	if (NULL != (program_name = strrchr(argv[0], '/')))
		++program_name;
	else
		program_name = argv[0];

	if (NULL == program_name || '\0' == *program_name)
		program_name = "aria2t";

	(void)sigemptyset(&sigmask);
	(void)sigaddset(&sigmask, SIGWINCH);
	(void)sigaddset(&sigmask, SIGINT);
	(void)sigaddset(&sigmask, SIGQUIT);

	/* (void)sigfillset(&sigmask); */
	/* Block signals to being able to receive it via `signalfd()` */
	(void)sigprocmask(SIG_BLOCK, &sigmask, NULL);

	if (load_cfg())
		return EXIT_FAILURE;

	if (ws_connect())
		return EXIT_FAILURE;

	/* Needed for ncurses UTF-8. */
	(void)setlocale(LC_CTYPE, "");

	(void)initscr();
	(void)start_color();
	(void)use_default_colors();
	fill_pairs();
	/* No input buffering. */
	(void)raw();
	/* No input echo. */
	(void)noecho();
	/* Do not translate '\n's. */
	(void)nonl();
	/* Make `getch()` non-blocking. */
	(void)nodelay(stdscr, TRUE);
	/* Catch special keys. */
	(void)keypad(stdscr, TRUE);
	/* 8-bit inputs. */
	(void)meta(stdscr, TRUE);

	/* (void)sigfillset(&sigmask); */
	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;
	fds[1].fd = ws_fd;
	fds[1].events = POLLIN;
#if __linux__
	(void)(fds[2].fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC));
	fds[2].events = POLLIN;
#endif

	ar_populate();

	/* Do an early redraw. */
	ar_cursor_changed();
	ar_redraw();
	refresh();

	for (;;) {
		int const any_activity = globalstat.download_speed + globalstat.upload_speed > 0;
		int const timeout = any_activity ? 1250 : 2500;

		switch (poll(fds, array_len(fds), timeout)) {
		case -1:
			abort();
		case 0:
			periodic();
			continue;
		}

		if (fds[0].revents & POLLIN) {
			if (stdin_read())
				break;
		}

		if (fds[1].revents & POLLIN) {
			if (ws_read())
				break;
		}

#ifdef __linux__
		if (fds[2].revents & POLLIN) {
			struct signalfd_siginfo ssi;

			while (sizeof ssi == read(fds[2].fd, &ssi, sizeof ssi)) {
				switch (ssi.ssi_signo) {
				case SIGWINCH:
					endwin();
					/* First refresh is for updating changed window size. */
					refresh();
					ar_redraw();
					refresh();
					break;
				case SIGTERM:

					break;
				case SIGINT:
					goto out_of_loop;
				}

			}
			if (EAGAIN != errno) {
				assert(0);
				break;
			}
		}
#else
		sigtimedwait()
#endif
	}
out_of_loop:

	endwin();

	ws_shutdown();
	free(aria_token);
	return retcode;
}
