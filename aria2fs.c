#define FUSE_USE_VERSION 39
#include <fuse_lowlevel.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <sys/stat.h>

#include "aria2-rpc.h"

/*
/shutdown 1
/pause_all 0: unpause, 1: pause, 2: forcepause
/stat
/...
/stopped/{gid}/
/waiting/{gid}/
/active/{gid}/
/all/{gid}/
 status
 dir@
 peers
 servers
 files/{num}/
  selected
  path@
  uris

 */
#define array_len(arr) (sizeof arr / sizeof *arr)

int rpc_on_error(struct json_node *error, void *data) {
	(void)error, (void)data;
	return 0;
}

void rpc_on_download_status_change(struct aria_download *d, int status) {
	(void)d, (void)status;

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

	printf("populated %d\n", (int)num_downloads);

	return 0;
}

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

static struct stat defst;

static ino_t gid2ino(char const*gid) {
	ino_t ino;
	memcpy(&ino, gid, sizeof ino);
	ino <<= 18;
	return ino;
}

static ssize_t ino2dl(ino_t ino, struct aria_download **outd) {
	struct aria_download *d = downloads;
	struct aria_download *end = &downloads[num_downloads];

	for (; d < end; ++d) {
		size_t diff = ino - gid2ino(d->gid);
		if (diff < (1 << 18)) {
			*outd = d;
			return (ssize_t)diff;
		}
	}

	return -1;
}

static int f_stat(struct stat *st, struct aria_download *d, size_t entry) {
	(void)d;

	printf("dstat [%lu]/entry %lu %d\n", d - downloads, entry, st->st_mode);

	switch (entry) {
	case 0:
		st->st_mode = S_IFDIR | 0750;
		st->st_nlink = 20;
		st->st_size = 4096;
		break;
	case 1:
		st->st_mode = S_IFREG;
		st->st_nlink = 1;
		st->st_size = 4096;
		break;
	default:
		return -1;
	}

	return 0;
}

static void f_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat st;

	(void)fi;

	printf("getattry ino %lu\n", ino);

	memcpy(&st, &defst, sizeof st);
	st.st_ino = ino;

	if (1 == ino) {
		st.st_nlink = 2 + num_downloads;
	} else {
		struct aria_download *d;
		ssize_t entry = ino2dl(ino, &d);
		if (-1 == entry)
			goto enoent;
		if (f_stat(&st, d, entry))
			goto enoent;
	}

	fuse_reply_attr(req, &st, 1.0);
	return;

enoent:
	fuse_reply_err(req, ENOENT);
}

static void f_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;

	printf("lookup %lu/%s\n", parent, name);
	memcpy(&e.attr, &defst, sizeof defst);
	if (1 == parent) {
		struct aria_download *d = find_download_bygid(name);
		if (NULL == d) {
			printf("no such gid: %s\n", name);
			goto enoent;

		}

		memset(&e, 0, sizeof(e));

		e.ino = gid2ino(d->gid);
		e.attr_timeout = 1.0;
		e.entry_timeout = 1.0;

		f_stat(&e.attr, d, 0);

	} else {
		struct aria_download *d;
		ssize_t entry = ino2dl(parent, &d);
		if (-1 == entry)
			goto enoent;

		e.ino = gid2ino(d->gid);
		e.attr_timeout = 1.0;
		e.entry_timeout = 1.0;

		if (f_stat(&e.attr, d, entry))
			goto enoent;
	}

	e.attr.st_ino = e.ino;
	fuse_reply_entry(req, &e);
	return;

enoent:
	fuse_reply_err(req, ENOENT);
}

static struct {
	void *buf;
	size_t len;
	size_t size;
	size_t maxsize;
	off_t off;
} dirbuf;

static int dirbuf_add(fuse_req_t req, struct stat *st, const char *name)
{
	++dirbuf.off;

	for (;;) {
		size_t remain = dirbuf.size - dirbuf.len;
		size_t needed = fuse_add_direntry(req, (char *)dirbuf.buf + dirbuf.len, remain, name, st, dirbuf.off);
		size_t newsize;
		void *p;

		printf("off=%u needed=%u\n", (unsigned)dirbuf.off, (unsigned)needed);
		if (needed <= remain) {
			dirbuf.len += needed;
			break;
		}
		if (dirbuf.size + needed > dirbuf.maxsize)
			return -1;

		newsize = needed + dirbuf.size / 4 * 5;
		p = realloc(dirbuf.buf, newsize);
		if (NULL == p)
			return -1;
		dirbuf.buf = p, dirbuf.size = newsize;
	}

	return 0;
}

static void f_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		struct fuse_file_info *fi) {
	(void)fi;

	printf("readdir %lu %lu %lu\n", ino, size, off);
	if (1 == ino) {
		struct stat st;
		struct aria_download *d;
		struct aria_download *end = &downloads[num_downloads];

		/* memset(&st, 0, sizeof st); */

		st.st_mode = S_IFDIR;

		dirbuf.off = off;
		dirbuf.len = 0;
		dirbuf.maxsize = size;

		printf("off=%u size=%u\n", (unsigned)off, (unsigned)size);

		switch (dirbuf.off) {
		case 0:
			st.st_ino = 1;
			dirbuf_add(req, &st, ".");
			--off;
			/* Fall through. */
		case 1:
			st.st_ino = 1;
			dirbuf_add(req, &st, "..");
			--off;
			/* Fall through. */
		}

		printf("num_dl=%u\n", (unsigned)num_downloads);

		for (d = &downloads[off - 2]; d < end; ++d) {
			memcpy(&st, &defst, sizeof st);
			st.st_mode = S_IFDIR;
			st.st_ino = gid2ino(d->gid);
			if (dirbuf_add(req, &st, d->gid))
				break;
		}

		printf("dirbuf.len=%u dirbuf.size=%u dirbuf.off=%u\n", (unsigned)dirbuf.len, (unsigned)dirbuf.size, (unsigned)dirbuf.off);
	} else {
		struct aria_download *d;
		ssize_t entry = ino2dl(ino, &d);
		if (-1 == entry)
			goto enotdir;



	}

	fuse_reply_buf(req, dirbuf.buf, dirbuf.len);
	return;

enotdir:
	fuse_reply_err(req, ENOTDIR);
}

static void f_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino != 2)
		fuse_reply_err(req, EISDIR);
	else if ((fi->flags & O_ACCMODE) != O_RDONLY)
		fuse_reply_err(req, EACCES);
	else
		fuse_reply_open(req, fi);
}

static struct fuse_lowlevel_ops const ops = {
	.readdir = f_readdir,
	.open = f_open,
	.lookup = f_lookup,
	.getattr = f_getattr,
};

int main(int argc, char *argv[]) {
	struct pollfd fds[2];
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_session *se;
	struct fuse_buf buf = { .mem = NULL, };
	int err = 1;

	if (rpc_load_cfg())
		goto err_rpc;

	if (rpc_connect())
		goto err_rpc;

	se = fuse_session_new(&args, &ops, sizeof ops, NULL);
	if (NULL == se)
		goto err_sess;

	if (fuse_set_signal_handlers(se))
		goto err_sighandler;

	stat("a", &defst);
	if (fuse_session_mount(se, "a"))
		goto err_mount;

	fds[0].fd = fuse_session_fd(se);
	fds[0].events = POLLIN;
	fds[1].fd = ws_fd;
	fds[1].events = POLLIN;

	populate();
	for (;;) {
		if (-1 == poll(fds, array_len(fds), -1)) {
			break;
		}

		if (fds[0].revents & POLLIN) {
			int err = fuse_session_receive_buf(se, &buf);
			if (err > 0)
				fuse_session_process_buf(se, &buf);
			else if (EINTR != err)
				break;

			if (fuse_session_exited(se))
				break;
		} else if (fds[0].revents & POLLERR)
			break;

		if (fds[1].revents & POLLIN) {
			if (ws_read())
				break;
		}
	}
	free(buf.mem);

	fuse_session_unmount(se);

	err = 0;
err_mount:
	fuse_remove_signal_handlers(se);
err_sighandler:
	fuse_session_destroy(se);
err_sess:
	fuse_opt_free_args(&args);
	rpc_shutdown();
err_rpc:
	return !err ? EXIT_SUCCESS : EXIT_FAILURE;

}
