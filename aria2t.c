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

#include "program.h"
#include "rpc.h"
#include "format.h"
#include "b64.h"

/* FIXME: handler->data = d;
 * make dls a pointer + simple ref counted
 * also handle removed dls
 * TODO: following downloads
 * */

static int namecol;
static int retcode = EXIT_FAILURE;

static int do_forced = 0;
static int selidx = 0;
static int topidx = 0;

static char tempfile[PATH_MAX];
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
#define COLOR_EOF  4

#define CONTROL(letter) ((letter) - '@')
#define ARRAY_LEN(arr) (sizeof arr / sizeof *arr)

#define VIEWS "\0dfp"
static char view = 'd';

static void draw_statusline(void);
static void draw_main(void);
static void draw_files(void);
static void draw_peers(void);
static void draw_downloads(void);
static void draw_download(struct aria_download const *d, int y, struct aria_download const *root);
static void draw_cursor(void);

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

static void draw_peers(void)
{
	/* peers or getServers */

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
		struct aria_uri const *uri = &f->uris[j];

		attr_set(A_NORMAL, 0, NULL);
		mvprintw((*y)++, 0, "      %s╴",
				j + 1 < f->num_uris ? "├" : "└");

		attr_set(uri->status == aria_uri_status_used ? A_BOLD : A_NORMAL, 0, NULL);
		printw("%3d%s ",
				j + 1,
				uri->status == aria_uri_status_used ? "*" : " ");

		attr_set(A_NORMAL, 0, NULL);
		addstr(uri->uri ? uri->uri : "(none)");
		clrtoeol();
	}
}


void
on_download_getfiles(struct json_node *result, struct aria_download *d)
{
	rpc_parse_download_files(d, result);

	draw_main();
	refresh();
}

void
aria_download_getfiles(struct aria_download *d)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	handler->proc = on_download_getfiles;
	handler->data = d;
	json_write_key(jw, "method");
	json_write_str(jw, "aria2.getFiles");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

static void
draw_files(void)
{
	if (num_downloads > 0) {
		struct aria_download *d = downloads[selidx];

		draw_download(d, 0, d);

		if (NULL != d->files) {
			size_t i;
			int y = 1;

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
	move(0, namecol);
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

	/* sort by name */
	return other->name != NULL && NULL != this->name
		? strcoll(this->name, other->name)
		: (NULL == this->name) - (NULL == other->name);
}

/* draw download d at screen line y */
static void
draw_download(struct aria_download const *d, int y, struct aria_download const *root)
{
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
	{
		/* struct aria_download *child = d; */
		/* struct aria_download *next;
		for (; child != root && child && (next = child->belongs_to) != child; child = next) */
		if (d->belongs_to != d && d->belongs_to)
			mvaddstr(y, x, "  "), x += 2;
	}

	attr_set(A_BOLD, -1, NULL);
	switch (abs(d->status)) {
	case DOWNLOAD_REMOVED:
		mvaddstr(y, x, "- "), x += 2;
		attr_set(A_NORMAL, 0, NULL);
		break;

	case DOWNLOAD_ERROR:
		mvaddstr(y, x, "* "), x += 2;
		attr_set(A_NORMAL, 0, NULL);
		mvaddstr(y, x, d->error_message);
		x += 30;
		attr_set(A_NORMAL, 0, NULL);
		break;

	case DOWNLOAD_WAITING:
		mvprintw(y, x, "%-8d", d->queue_index), x += 8;

		attr_set(A_NORMAL, 0, NULL);

		n = fmt_space(fmtbuf, d->total);
		mvaddnstr(y, x, fmtbuf, n), x += n;
		mvaddstr(y, x, " "), x += 1;

		goto print_files;

	case DOWNLOAD_PAUSED:
		mvaddstr(y, x, "| ");
		goto print_completed;

	case DOWNLOAD_COMPLETE:
		mvaddstr(y, x, "  ");

	print_completed:
		x += 2;
		attr_set(A_NORMAL, 0, NULL);

		n = fmt_space(fmtbuf, d->total);
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, "["), x += 1;

		n = fmt_percent(fmtbuf, d->have, d->total);
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, "] ("), x += 3;

	print_files:
		if (NULL != d->files) {
			if (d->num_files == d->num_selfiles) {
				fmtbuf[0] = ' ';
				fmtbuf[1] = 'a';
				fmtbuf[2] = 'l';
				fmtbuf[3] = 'l';
				n = 4;
			} else if (0 == d->num_selfiles) {
				fmtbuf[0] = 'n';
				fmtbuf[1] = 'o';
				fmtbuf[2] = 'n';
				fmtbuf[3] = 'e';
				n = 4;
			} else {
				n = fmt_number(fmtbuf, d->num_selfiles);
			}
		} else {
			fmtbuf[0] = ' ';
			fmtbuf[1] = ' ';
			fmtbuf[2] = ' ';
			fmtbuf[3] = '?';
			n = 4;
		}
		mvaddnstr(y, x, fmtbuf, n), x += n;

		mvaddstr(y, x, "/"), x += 1;

		if (NULL != d->files) {
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

		mvaddstr(y, x, " "), x += 1;
		break;

	case DOWNLOAD_ACTIVE:
		mvaddstr(y, x, "> ");
		attr_set(A_NORMAL, 0, NULL), x += 2;

		if (d->download_speed > 0) {
			attr_set(A_BOLD, COLOR_DOWN, NULL);

			n = fmt_percent(fmtbuf, d->have, d->total);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			attr_set(A_NORMAL, 0, NULL);

			mvaddstr(y, x, " @ "), x += 3;

			attr_set(A_BOLD, COLOR_DOWN, NULL);
			mvaddstr(y, x, " ↓ "), x += 3;

			n = fmt_speed(fmtbuf, d->download_speed);
			mvaddnstr(y, x, fmtbuf, n), x += n;
			mvaddstr(y, x, " "), x += 1;
			/* mvaddstr(y, x, "/"); */
			attr_set(A_NORMAL, -1, NULL);
		} else {
			n = fmt_space(fmtbuf, d->total);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			mvaddstr(y, x, "["), x += 1;

			attr_set(d->have == d->total ? A_NORMAL : A_BOLD, COLOR_DOWN, NULL);

			n = fmt_percent(fmtbuf, d->have, d->total);
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
			mvaddstr(y, x, " ↑ "), x += 3;

			n = fmt_space(fmtbuf, d->uploaded);
			mvaddnstr(y, x, fmtbuf, n), x += n;

			if (d->upload_speed > 0) {
				mvaddstr(y, x, " @ "), x += 3;

				n = fmt_speed(fmtbuf, d->upload_speed);
				mvaddnstr(y, x, fmtbuf, n), x += n;
			} else {
				mvaddstr(y, x, "["), x += 1;

				attr_set(A_NORMAL, COLOR_UP, NULL);

				n = fmt_percent(fmtbuf, d->uploaded, d->total);
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
	mvaddstr(y, x, NULL != d->name ? d->name : "(no name)");
	clrtoeol();
}

static int
getmainheight(void)
{
	return getmaxy(stdscr)/*window height*/ - 1/*status line*/;
}

static void
draw_downloads(void)
{
	int line = 0, height = getmainheight();

	for (;line < height; ++line) {
		if ((size_t)(topidx + line) < num_downloads) {
			draw_download(downloads[topidx + line], line, NULL);
		} else {
			attr_set(A_NORMAL, COLOR_EOF, NULL);
			mvaddstr(line, 0, "~");
			attr_set(A_NORMAL, 0, NULL);
			clrtoeol();
		}
	}

	draw_statusline();
	draw_cursor();
}

static void
on_downloads_changed(int stickycurs)
{
	/* re-sort downloads list */
	if (num_downloads > 0) {
		char selgid[sizeof ((struct aria_download *)0)->gid];

		if (selidx >= num_downloads)
			selidx = num_downloads - 1;
		memcpy(selgid, downloads[selidx]->gid, sizeof selgid);

		/* FIXME: conversion maybe not valid */
		qsort(downloads, num_downloads, sizeof *downloads, (int(*)(void const *, void const *))downloadcmp);

		/* move selection if download moved */
		if (stickycurs && 0 != memcmp(selgid, downloads[selidx]->gid, sizeof selgid)) {
			struct aria_download **dd = get_download_bygid(selgid);

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
	int const h = view == 'd' ? getmainheight() : 1;
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
		move(view == 'd' ? selidx - topidx : 0, namecol);
	} else {
		on_scroll_changed();
	}

	draw_statusline();
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

	printw("[%c %4d/%-4d] [%4d/%4d|%4d] [%s%s%dc] @ %s:%d%s%s%s",
			view,
			num_downloads > 0 ? selidx + 1 : 0, num_downloads,
			globalstat.num_active,
			globalstat.num_waiting,
			globalstat.num_stopped_total,
			globalstat.save_session ? "S" : "",
			globalstat.optimize_concurrency ? "O" : "",
			globalstat.max_concurrency,
			rpc_host, rpc_port,
			ws_fd >= 0 ? "" : " (not connected)",
			last_error ? ": " : "",
			last_error ? last_error : "");

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

	move(view == 'd' ? selidx - topidx : 0, namecol);
}

static void
draw_all(void)
{
	draw_main();
	draw_cursor();
}

void
rpc_on_error(struct json_node *error)
{
	struct json_node const *message = json_get(error, "message");

	free(last_error);
	last_error = strdup(message->val.str);

	draw_statusline();
	refresh();
}

static void
on_update_all(struct json_node *result, void *data)
{
	(void)data;

	/* flush everything */
	num_downloads = 0;

	result = json_children(result);
	do {
		struct json_node *downloads_list;
		struct json_node *node;
		uint32_t downloadidx;

		if (json_arr != json_type(result)) {
			rpc_on_error(result);
			continue;
		}

		downloads_list = json_children(result);

		if (json_isempty(downloads_list))
			continue;

		downloadidx = num_downloads;
		node = json_children(downloads_list);
		do {
			struct aria_download *d = rpc_download_alloc();
			if (NULL == d)
				continue;

			rpc_parse_download(d, node);
			if (DOWNLOAD_WAITING == abs(d->status)) {
				/* we know that waiting downloads are arriving sequentially
				 * ordered */
				d->queue_index = (num_downloads - 1) - downloadidx;
			}
		} while (NULL != (node = json_next(node)));

	} while (NULL != (result = json_next(result)));

	on_downloads_changed(0);
	refresh();
}

static void
compute_global(void)
{
	struct aria_download *d, **dd = downloads;
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
on_periodic(struct json_node *result, void *data)
{
	(void)data;

	/* Result is an array. Go to the first element. */
	result = json_children(result);
	rpc_parse_globalstat(result);

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

	draw_statusline();

	result = json_next(result);
	if (NULL != result) {
		rpc_parse_downloads(result);
		on_downloads_changed(1);
	}

	refresh();
}

void
rpc_on_download_status_change(struct aria_download *d, int status)
{
	(void)d, (void)status;

	on_downloads_changed(1);
	refresh();
}

static char *
file_b64_enc(char *pathname)
{
	int fd = open(pathname, O_RDONLY);
	char *buf;
	char *b64;
	size_t b64len;
	struct stat st;

	if (-1 == fd)
		return NULL;

	if (-1 == fstat(fd, &st))
		return NULL;

	buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (MAP_FAILED == buf)
		return NULL;

	b64 = b64_enc(buf, st.st_size, &b64len);

	(void)munmap(buf, st.st_size);

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

static void
on_session_info(struct json_node *result, void *data)
{
	char *tmp = getenv("TMP");

	(void)data;

	snprintf(tempfile, sizeof tempfile, "/%s/aria2.%s",
			NULL != tmp ? tmp : "tmp",
			rpc_parse_session_info(result));
}

static void
gentmp(void)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	handler->proc = on_session_info;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, "aria2.getSessionInfo");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

static int
fileout(int mustedit)
{
	char *prog;
	pid_t pid;
	int status;

	if (mustedit) {
		if (NULL == (prog = getenv("EDITOR")))
			prog = "vi";
	} else {
		if (NULL == (prog = getenv("PAGER")))
			prog = "less";
	}

	def_prog_mode();
	endwin();

	if (0 == (pid = fork())) {
		execlp(prog, prog, tempfile, NULL);
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
parse_option(char const *option, char const *value, struct aria_download *d)
{
	if (NULL != d) {
		if (0 == strcmp("max-download-limit", option))
			d->download_speed_limit = atol(value);
		else if (0 == strcmp("max-upload-limit", option))
			d->upload_speed_limit = atol(value);
	} else {
		if (0 == strcmp("max-concurrent-downloads", option))
			globalstat.max_concurrency = atol(value);
		else if (0 == strcmp("save-session", option))
			globalstat.save_session = 0 == strcmp(value, "true");
		else if (0 == strcmp("optimize-concurrent-downloads", option))
			globalstat.optimize_concurrency = 0 == strcmp(value, "true");
		else if (0 == strcmp("max-overall-download-limit", option))
			globalstat.download_speed_limit = atol(value);
		else if (0 == strcmp("max-overall-upload-limit", option))
			globalstat.upload_speed_limit = atol(value);
	}
}

static void
on_options(struct json_node *result, struct aria_download *d)
{
	rpc_parse_options(result, parse_option, d);
}

/* Returns:
 * - <0: action did not run.
 * - =0: action executed and terminated successfully.
 * - >0: action executed but failed. */
static int
runaction(const char *name, ...)
{
	char filename[PATH_MAX];
	char filepath[PATH_MAX];
	va_list argptr;
	pid_t pid;
	int status;

	va_start(argptr, name);
	vsnprintf(filename, sizeof filename, name, argptr);
	va_end(argptr);

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
				num_downloads > 0 ? downloads[selidx]->gid : NULL,
				tempfile,
				NULL);
		_exit(127);
	}

	while (-1 == waitpid(pid, &status, 0) && errno == EINTR)
		;

	refresh();

	if (WIFEXITED(status) && 127 == WEXITSTATUS(status))
		return -1;
	else
		return WIFEXITED(status) && EXIT_SUCCESS == WEXITSTATUS(status) ? 0 : 1;
}

static void
on_show_options(struct json_node *result, struct aria_download *d)
{
	FILE *f;
	int err;
	char *line;
	size_t linesiz;
	struct rpc_handler *handler;
	int action;

	/* FIXME: error handling */

	if (NULL == (f = fopen(tempfile, "w")))
		return;

	rpc_parse_options(result, write_option, f);
	rpc_parse_options(result, parse_option, d);

	fclose(f);

	if ((action = runaction(NULL != d ? "i" : "I")) < 0)
		action = fileout(0);

	if (EXIT_SUCCESS != action)
		return;

	if (NULL == (f = fopen(tempfile, "r")))
		return;

	handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.changeOption" : "aria2.changeGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
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
	fclose(f);

	rpc_writer_epilog(handler);
}

static void
ar_update_delta(int all);

static void
update_options(struct aria_download *d, int user)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	handler->proc = user ? on_show_options : on_options;
	handler->data = d;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.getOption" : "aria2.getGlobalOption");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	if (NULL != d) {
		/* “gid” */
		json_write_str(jw, d->gid);
	}
	json_write_endarr(jw);

	rpc_writer_epilog(handler);

	ar_update_delta(NULL != d ? 0 : 1);
}

static void
ar_show_options(struct aria_download *d)
{
	update_options(d, 1);
}

static void
on_select_files(struct json_node *result, struct aria_download *d)
{
	if (NULL != d->files) {
	}
}

static void
ar_download_select_files(void)
{
	struct aria_download *d;

	if (num_downloads == 0)
		return;

	d = downloads[selidx];
	if (NULL == d->files) {
		on_select_files(NULL, d);
		return;
	}

}

static void
ar_download_add(char ch)
{
	struct rpc_handler *handler;
	FILE *f;
	char *line;
	size_t linesiz;
	ssize_t err;
	size_t linelen;
	int action;

	if ((action = runaction("%c", ch)) < 0)
		action = fileout(1);

	if (EXIT_SUCCESS != action)
		return;

	/* FIXME: error handling */
	if (NULL == (f = fopen(tempfile, "r"))) {
		return;
	}

	handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, "system.multicall");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* 1st arg: “methods” */
	json_write_beginarr(jw); /* {{{ */

	line = NULL, linesiz = 0;
	while (-1 != (err = getline(&line, &linesiz, f))) {
		char *uri;
		char *b64str = NULL;
		enum { kind_torrent, kind_metalink, kind_uri } kind;

		linelen = (size_t)err;
		uri = stripwhite(line, &linelen);

#define ISSUFFIX(lit) \
	((size_t)linelen >= sizeof lit && \
		 0 == memcmp(uri + (size_t)linelen - ((sizeof lit) - 1), lit, (sizeof lit) - 1))

		if (0 == linelen)
			continue;

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
		json_write_str(jw, rpc_secret);
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
			json_write_endarr(jw);
		}
		/* “options” */
		json_write_beginobj(jw);
		{
			char buf[PATH_MAX];
			char *cwd;
			json_write_key(jw, "dir");
			if (NULL == (cwd = getcwd(buf, sizeof buf)))
				strcpy(buf, ".");

			json_write_str(jw, cwd);
		}
		json_write_endobj(jw);
		/* “position” */
		/* insert position at specified queue index */
		/* do not care if it’s bigger, it will be added to the end */
		json_write_num(jw, selidx);

		json_write_endarr(jw);

		json_write_endobj(jw);
	}

	free(line);
	fclose(f);

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

static void
ar_update_all(void)
{
	unsigned n;
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	handler->proc = on_update_all;
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
		json_write_str(jw, "errorMessage");
		json_write_str(jw, "uploadSpeed");
		json_write_str(jw, "downloadSpeed");
		json_write_str(jw, "connections");
		json_write_str(jw, "seeder");
		json_write_str(jw, "bittorrent");
		json_write_endarr(jw);
		/* }}} */
		json_write_endarr(jw);

		json_write_endobj(jw);
	}
out_of_loop:

	json_write_endarr(jw); /* }}} */
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

static void
try_connect(void)
{
	assert(ws_fd < 0);
	if (rpc_connect()) {
		fds[1].fd = -1;
		return;
	}

	fds[1].fd = ws_fd;

	if (tempfile[0])
		unlink(tempfile);
	gentmp();
	ar_update_all();
	ar_update_delta(1); /*for global stat*/
}

static void
ar_update_delta(int all)
{
	struct rpc_handler *handler;

	if (ws_fd < 0) {
		try_connect();
		return;
	}

	if (NULL == (handler = rpc_writer_prolog()))
		return;

	handler->proc = on_periodic;
	json_write_int(jw, (int)(handler - rpc_handlers));

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
		json_write_str(jw, rpc_secret);
		json_write_endarr(jw);
		json_write_endobj(jw);
	}
	if (num_downloads > 0) {
		struct aria_download **dd;
		int n;
		if (all && view == 'd') {
			dd = &downloads[topidx];
			n = getmainheight();

			if ((size_t)(topidx + n) > num_downloads)
				n = num_downloads - topidx;
		} else {
			dd = &downloads[selidx];
			n = !!num_downloads;
		}

		for (;n-- > 0; ++dd) {
			struct aria_download *d = *dd;

			if (d->status < 0)
				globalstat.compute_total = 1;

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

			json_write_str(jw, "gid");

			if (DOWNLOAD_REMOVED != abs(d->status))
				json_write_str(jw, "status");

			if (NULL == d->name && d->status < 0) {
				if (DOWNLOAD_REMOVED != abs(d->status))
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
				/* assert(0); */
			}

			if (view == 'f') {
				json_write_str(jw, "files");
			}

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
				if (d->status < 0 || NULL == d->error_message) {
					json_write_str(jw, "errorMessage");
				}
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

	rpc_writer_epilog(handler);
}

static void
writetemp(void)
{
	FILE *f;
	size_t i;
	struct aria_download *d;

	if (num_downloads == 0)
		return;

	if (NULL == (f = fopen(tempfile, "w")))
		return;

	d = downloads[selidx];

	if (NULL != d->files) {
		for (i = 0; i < d->num_files; ++i)
			fprintf(f, "file\t%s\n", d->files[i].path);
	} else {
		aria_download_getfiles(d);
	}

	fclose(f);
}

static void
on_download_remove(struct json_node *result, void *data)
{
	writetemp();
	runaction("D");
}

static int
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
					selidx = get_download_bygid(d->gid) - downloads;
					draw_cursor();
					refresh();
				}
			}
			break;

		case 'v':
			if (!(view = strchr(VIEWS + 1, view)[1]))
				view = VIEWS[1];

			draw_all();
			refresh();
			break;

		case 'V':
			if (!(view = strchr(VIEWS + 1, view)[-1]))
				view = VIEWS[ARRAY_LEN(VIEWS) - 1];

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
			ar_download_add(ch);
			break;

		case 'i':
		case 'I':
			ar_show_options(num_downloads > 0 && ch == 'i' ? downloads[selidx] : NULL);
			break;

		case 'f':
			ar_download_select_files();
			break;

		case 'D':
		case KEY_DC: /*delete*/
			if (num_downloads > 0) {
				aria_download_remove(downloads[selidx], do_forced);
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
				draw_all();
				refresh();
				continue;
			}
			/* FALLTHROUGH */
		case 'Z':
			retcode = EXIT_SUCCESS;
			return 1;

		case CONTROL('M'):
			if (isatty(STDOUT_FILENO)) {
				goto defaction;
			} else {
				endwin();

				if (num_downloads > 0) {
					struct aria_download const *d = downloads[selidx];
					printf("%s\n", d->gid);
				}

				retcode = EXIT_SUCCESS;
				return 1;
			}
			break;

		case 'Q':
			aria_shutdown(do_forced);
			do_forced = 0;
			break;

		case 'u':
			ar_update_delta(0);
			break;

		case 'U':
			ar_update_delta(1);
			break;

		case CONTROL('L'):
			/* try connect if not connected */
			if (ws_fd < 0)
				try_connect();
			ar_update_all();
			break;

		case CONTROL('C'):
			return 1;

		default:
		defaction:
			writetemp();
			runaction("%s", keyname(ch));
			do_forced = 0;
			break;

		}

	}

	return 0;
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

	(void)argc;

	/* Needed for ncurses UTF-8. */
	setlocale(LC_CTYPE, "");

	set_program_name(argv[0]);

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGWINCH);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGQUIT);

	/* (void)sigfillset(&sigmask); */
	/* Block signals to being able to receive it via `signalfd()` */
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	signal(SIGPIPE, SIG_IGN);

	if (rpc_load_cfg())
		return EXIT_FAILURE;

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

	try_connect();

	draw_all();
	refresh();

	for (;;) {
		int const any_activity = globalstat.download_speed + globalstat.upload_speed > 0;
		int const timeout = ws_fd >= 0 ? (any_activity ? 1250 : 2500) : 5000;

		switch (poll(fds, ARRAY_LEN(fds), timeout)) {
		case -1:
			perror("poll");
			break;

		case 0:
			ar_update_delta(1);
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
					draw_all();
					refresh();
					break;
				case SIGTERM:

					break;
				case SIGINT:
					goto exit;
				}

			}
			if (EAGAIN != errno) {
				assert(0);
				break;
			}
		}
#else
		/* sigtimedwait() */
#endif
	}
exit:

	unlink(tempfile);

	endwin();

	rpc_shutdown();

	return retcode;
}
