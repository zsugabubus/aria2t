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
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/signalfd.h>
#endif

#include "program.h"
#include "aria2-rpc.h"
#include "format.h"

static int namecol;
static int retcode = EXIT_FAILURE;

static size_t selidx = SIZE_MAX;
static size_t firstidx = 0;

#define COLOR_DOWN 1
#define COLOR_UP   2
#define COLOR_CONN 3
#define COLOR_EOF  4

#define ctrl(x) ((x) & 0x1f)
#define array_len(arr) (sizeof arr / sizeof *arr)

static int redraw_globalstat(void);
static void redraw_download(struct aria_download *d, int y);
static void redraw_downloads(void);
static void redraw_cursor(void);

static int downloadcmp(struct aria_download const*this, struct aria_download const*other) {
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

	cmp = (NULL == other->name) - (NULL == this->name);
	if (cmp)
		return -cmp;

	return other->name != NULL && NULL != this->name
		? strcmp(this->name, other->name)
		: 0;
}

static void redraw_download(struct aria_download *d, int y) {
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
	mvaddstr(y, x, NULL != d->name ? d->name : "[No Name]");
	clrtoeol();
}

static void redraw_downloads(void) {
	int line = 0, height = getmaxy(stdscr) - 1/*status line*/;

	for (;line < height; ++line) {
		if (firstidx + line < num_downloads) {
			redraw_download(&downloads[firstidx + line], line);
		} else {
			attr_set(A_NORMAL, COLOR_EOF, NULL);
			mvaddstr(line, 0, "~");
			attr_set(A_NORMAL, 0, NULL);
			clrtoeol();
		}
	}

	redraw_globalstat();

	redraw_cursor();
}

static void on_downloads_changed(void) {
	char selgid[sizeof ((struct aria_download *)0)->gid];

	if (SIZE_MAX != selidx)
		memcpy(selgid, downloads[selidx].gid, sizeof selgid);

	qsort(downloads, num_downloads, sizeof *downloads, (int(*)(void const*,void const*))downloadcmp);

	if (SIZE_MAX != selidx &&
		0 != memcmp(selgid, downloads[selidx].gid, sizeof selgid)) {
		struct aria_download *d = find_download_bygid(selgid);

		assert(NULL != d);
		selidx = d - downloads;
	}

	redraw_downloads();
}

static void on_scroll_changed(void) {
	redraw_downloads();
}

static void redraw_cursor(void) {
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
		on_scroll_changed();
	}
}

static int redraw_globalstat(void) {
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

static void redraw_all(void) {
	redraw_downloads();
	redraw_globalstat();
	redraw_cursor();
}

int rpc_on_error(struct json_node *error, void *data) {
	struct json_node const *code = json_get(error, "code");
	struct json_node const *message = json_get(error, "message");

	(void)data;

	def_prog_mode();
	endwin();
	fflush(stdout);

	fprintf(stderr, "%s: %s:%u: RPC error %d: “%s”\n",
			program_name,
			rpc_host, rpc_port,
			(int)code->val.num, message->val.str);
	fflush(stderr);

	getchar();

	reset_prog_mode();
	flushinp();
	refresh();

	return 0;
}

static int on_populate(struct json_node *result, void *data) {
	(void)data;

	result = json_children(result);
	do {
		struct json_node *downloads_list;
		struct json_node *node;

		if (json_arr != json_type(result)) {
			rpc_on_error(result, data);
			continue;
		}

		downloads_list = json_children(result);

		if (json_isempty(downloads_list))
			continue;

		node = json_children(downloads_list);
		do {
			struct aria_download *d = rpc_download_alloc();
			if (NULL == d)
				continue;

			rpc_parse_download(d, node);
		} while (NULL != (node = json_next(node)));

	} while (NULL != (result = json_next(result)));

	on_downloads_changed();
	refresh();

	return 0;
}

int on_periodic(struct json_node *result, void *data) {
	(void)data;

	/* Result is an array. Go to the first element. */
	result = json_children(result);
	rpc_parse_globalstat(result);
	redraw_globalstat();

	result = json_next(result);
	if (NULL != result) {
		rpc_parse_downloads(result);
		on_downloads_changed();
	}
	refresh();

	return 0;
}


void rpc_on_download_status_change(struct aria_download *d, int status) {
	(void)d, (void)status;

	on_downloads_changed();
	refresh();
}

#if 0
static int on_downloads_add(struct json_node *result, void *data) {
	(void)result;
	if (NULL != data)
		unlink(data);
	return 0;
}

static char *stripwhite(char *str, size_t *n) {
	while (isspace(str[*n - 1]))
		--*n;
	str[*n] = '\0';

	while (isspace(str[0]))
		++str, --*n;

	return str;
}

static int fileout(char *filepath, size_t n) {
	char *editor;
	pid_t pid;
	char *tmp = getenv("TMP");
	int fd;
	int wst;
#ifdef __linux__
	int ppid = getpid();
#endif

	def_prog_mode();
	endwin();

	if (NULL == (editor = getenv("EDITOR")))
		assert(0);

	snprintf(filepath, n, "/%s/%s.XXXXXX",
			NULL != tmp ? tmp : "tmp",
			program_name);
	if (-1 == (fd = mkstemp(filepath)))
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

	while (-1 == waitpid(pid, &wst, 0) && errno == EINTR)
		;

	refresh();

	if (!WIFEXITED(wst) || EXIT_SUCCESS != WEXITSTATUS(wst)) {
		(void)unlink(filepath);
		fd = -1;
	}

	return fd;
}

static void ar_download_add() {
	struct rpc_handler *handler;
	FILE *file;
	int fd;
	char *line;
	size_t linesize;
	ssize_t err;
	size_t linelen;
	char filepath[256];

	handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	if (-1 == (fd = fileout(filepath, sizeof filepath)))
		return;

	handler->func = on_downloads_add;
	handler->data = strdup(filepath);
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	file = fdopen(fd, "r");
	line = NULL, linesize = 0;
	while (-1 != (err = getline(&line, &linesize, file))) {
		char *uri;
		enum { TORRENT, METALINK, URI } type;

		linelen = (size_t)err;
		uri = stripwhite(line, &linelen);

#define ISSUFFIX(lit) \
	((size_t)linelen >= sizeof lit && \
		 0 == memcmp(uri + (size_t)linelen - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (0 == linelen)
			continue;

		if (ISSUFFIX(".torrent"))
			type = TORRENT;
		else if (ISSUFFIX(".meta4") || ISSUFFIX(".metalink"))
			type = METALINK;
		else
			type = URI;
#undef ISSUFFIX

		json_write_beginobj(jw);

		json_write_key(jw, "methodName");
		switch (type) {
		case TORRENT:
			json_write_str(jw, "aria2.addTorrent");
			break;
		case METALINK:
			json_write_str(jw, "aria2.addMetalink");
			break;
		case URI:
			json_write_str(jw, "aria2.addUri");
			break;
		}

		json_write_key(jw, "params");
		json_write_beginarr(jw);
		/* “secret” */
		json_write_str(jw, rpc_secret);
		/* “data” */
		switch (type) {
		case TORRENT:
		case METALINK: {
			char *str = file_b64_enc(uri);
			if (NULL == str)
				break;

			json_write_str(jw, str);
			free(str);
			break;
		}
		case URI:
			json_write_beginarr(jw);
			json_write_str(jw, uri);
			json_write_endarr(jw);
			break;
		}
		if (TORRENT == type) {
			/* “uris” */
			json_write_beginarr(jw);
			json_write_endarr(jw);
		}
		/* “options” */
		json_write_beginobj(jw);
		{
			char buf[256];
			char *cwd;
			json_write_key(jw, "dir");
			if (NULL == (cwd = getcwd(buf, sizeof buf)))
				assert(0);

			json_write_str(jw, cwd);
		}
		json_write_endobj(jw);

		json_write_endarr(jw);

		json_write_endobj(jw);
	}
	free(line);

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	rpc_writer_epilog();
}
#endif

static int populate(void) {
	unsigned n;
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = on_populate;
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
		json_write_str(jw, rpc_secret);

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

	rpc_writer_epilog();
	return 0;
}

static int periodic(void) {
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = on_periodic;
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
		json_write_str(jw, rpc_secret);
		json_write_endarr(jw);
		json_write_endobj(jw);
	}
	{
		struct aria_download *d = &downloads[firstidx];
		size_t n = getmaxy(stdscr) - 1;

		if (firstidx + n > num_downloads)
			n = num_downloads - firstidx;

		for (;n-- > 0; ++d) {
			json_write_beginobj(jw);
			json_write_key(jw, "methodName");
			json_write_str(jw, "aria2.tellStatus");
			json_write_key(jw, "params");
			json_write_beginarr(jw);
			/* “secret” */
			json_write_str(jw, rpc_secret);
			/* “gid” */
			json_write_str(jw, d->gid);
			/* “keys” */
			json_write_beginarr(jw);

			if (DOWNLOAD_REMOVED == abs(d->status))
				continue;

			json_write_str(jw, "gid");
			json_write_str(jw, "status");
			if (NULL == d->name && d->status < 0) {
				json_write_str(jw, "seeder");
				json_write_str(jw, "bittorrent");
			} else if (NULL == d->name && NULL == d->files && d->status >= 0) {
				/* If “bittorrent.info.name” is empty then
				 * assign the name of the first file as name.
				 * */
				json_write_str(jw, "files");
				assert(d->progress != (void*)1);
				d->progress = (void*)1;
			} else if (NULL == d->name) {
				assert(0);
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

	rpc_writer_epilog();
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

			redraw_cursor();
			refresh();
			break;

		case 'P':
			if (SIZE_MAX == selidx)
				break;

			(void)aria_pause_all(DOWNLOAD_PAUSED != abs(downloads[selidx].status));
			break;

		case 'g':
			if (0 == num_downloads)
				break;

			selidx = 0;
			redraw_cursor();
			refresh();
			break;

		case 'G':
			if (0 == num_downloads)
				break;

			selidx = num_downloads - 1;
			redraw_cursor();
			refresh();
			break;

		case ' ':
		case 'p':
			if (SIZE_MAX == selidx)
				break;

			(void)aria_download_pause(&downloads[selidx], DOWNLOAD_PAUSED != abs(downloads[selidx].status));
			break;

		case 'j':
		case KEY_DOWN:
			if (SIZE_MAX == selidx && num_downloads > 0)
				selidx = 0;
			else if (selidx + 1 < num_downloads)
				++selidx;
			else
				break;

			redraw_cursor();
			refresh();
			break;

#if 0
		case 'a':
			ar_download_add();
			break;
#endif

		case 'D':
		case KEY_DC: /* Delete */
			if (SIZE_MAX == selidx)
				break;
			aria_download_remove(&downloads[selidx],  0);
			break;

		case ctrl('D'):
			if (SIZE_MAX == selidx)
				break;
			aria_download_remove(&downloads[selidx],  1);
			break;

		case 'q':
			retcode = EXIT_SUCCESS;
			return 1;

		/* case 'X':
			ar_delete_removed();
			return 1; */

		case KEY_F(9):
			aria_shutdown(0);
			break;

		case ctrl('c'):
			return 1;
		}

	}

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

	/* Needed for ncurses UTF-8. */
	(void)setlocale(LC_CTYPE, "");

	set_program_name("aria2-watch");

	(void)sigemptyset(&sigmask);
	(void)sigaddset(&sigmask, SIGWINCH);
	(void)sigaddset(&sigmask, SIGINT);
	(void)sigaddset(&sigmask, SIGQUIT);

	/* (void)sigfillset(&sigmask); */
	/* Block signals to being able to receive it via `signalfd()` */
	(void)sigprocmask(SIG_BLOCK, &sigmask, NULL);

	if (rpc_load_cfg())
		return EXIT_FAILURE;

	if (rpc_connect())
		return EXIT_FAILURE;

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

	populate();
	periodic();

	redraw_all();
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
					redraw_all();
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

	rpc_shutdown();

	return retcode;
}
