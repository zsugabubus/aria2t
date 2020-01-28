#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "program.h"
#include "aria2-rpc.h"
#include "websocket.h"
#include "jeezson/jeezson.h"
#include "base64.h"

static struct json_node *nodes;
static size_t nnodes;

struct aria_globalstat globalstat;
struct aria_download *downloads;
size_t num_downloads;

char *rpc_secret;
char const* rpc_host;
in_port_t rpc_port;

struct json_writer jw[1];

struct rpc_handler rpc_handlers[10];
#define array_len(a) (sizeof(a) / sizeof(*a))

static int on_rpc_notification(char const *method, struct json_node *event);

static int on_rpc_action(struct json_node *result, void *data) {
	(void)result, (void)data;
	return 0;
}

int rpc_load_cfg(void) {
	char *str;

	if (NULL == (rpc_host = getenv("ARIA_RPC_HOST")))
		rpc_host = "127.0.0.1";

	str = getenv("ARIA_RPC_PORT");
	if (NULL == str ||
		(errno = 0, rpc_port = strtoul(str, NULL, 10), errno))
		rpc_port = 6800;

	/* “token:secret” */
	(str = getenv("ARIA_RPC_SECRET")) || (str = "");
	rpc_secret = malloc(snprintf(NULL, 0, "token:%s", str));
	if (NULL == rpc_secret) {
		fprintf(stderr, "%s: Failed to allocate memory.\n",
				program_name);
		return -1;
	}

	(void)sprintf(rpc_secret, "token:%s", str);
	return 0;
}

int rpc_connect(void) {
	return  ws_connect(rpc_host, rpc_port);
}

int rpc_shutdown(void) {
	ws_shutdown();
	free(rpc_secret);
	return 0;
}

struct aria_download *rpc_download_alloc(void) {
	void *p = realloc(downloads, (num_downloads + 1) * sizeof *downloads);

	if (NULL == p)
		return NULL;

	return memset(&(downloads = p)[num_downloads++], 0, sizeof *downloads);
}

static struct rpc_handler *rpc_handler_alloc(void) {
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

struct aria_download *find_download_bygid(char const*gid) {
	struct aria_download *d = downloads;
	struct aria_download *const end = &downloads[num_downloads];

	for (;d < end; ++d)
		if (0 == memcmp(d->gid, gid, sizeof d->gid))
			return d;
	return NULL;
}

/* static char *file_b64_enc(char *pathname) {
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
	if (NULL != b64)
		b64[b64len] = '\0';

	(void)munmap(buf, st.st_size);

	return b64;
} */

int on_ws_message(char *msg, size_t msglen) {
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
			r = rpc_on_error(json_get(nodes, "error"), handler->data);

		handler_free(handler);
		if (r)
			return r;
	} else if (NULL != (method = json_get(nodes, "method"))) {
		struct json_node *const params = json_get(nodes, "params");

		if ((r = on_rpc_notification(method->val.str, params + 1)))
			return r;
	} else {
		assert(0);
	}

	/* json_debug(nodes, 0); */
	/* json_debug(json_get(json_get(nodes, "result"), "version"), 0); */
	return 0;
}

struct rpc_handler *rpc_writer_prolog() {
	json_writer_init(jw);
	json_write_beginobj(jw);

	json_write_key(jw, "jsonrpc");
	json_write_str(jw, "2.0");

	json_write_key(jw, "id");
	return rpc_handler_alloc();
}

void rpc_writer_epilog() {
	json_write_endobj(jw);

	ws_write(jw->buf, jw->len);

	jw->buf = NULL;
	json_writer_free(jw);
}

void rpc_parse_download_files(struct aria_download *d, struct json_node *node) {
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
				file.path = strlen(field->val.str) > 0 ? strdup(field->val.str) : NULL;
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

void rpc_parse_download(struct aria_download *d, struct json_node *node) {
	struct json_node *field = json_children(node);

	do {
		if (0 == strcmp(field->key, "gid")) {
			assert(strlen(field->val.str) == sizeof d->gid - 1);
			memcpy(d->gid, field->val.str, sizeof d->gid);
		} else if (0 == strcmp(field->key, "files")) {
			rpc_parse_download_files(d, field);
			if (NULL == d->files)
				continue;

			if (NULL == d->name) {
				char *path = d->files[0].path;
				if (NULL != path) {
					char *last_slash = strrchr(path, '/');
					d->name = strdup(NULL != last_slash ? last_slash + 1 : d->files[0].path);
				}
			}

		} else if (0 == strcmp(field->key, "bittorrent")) {
			struct json_node *bt_info, *bt_name;

			if (NULL == (bt_info = json_get(field, "info")))
				continue;

			if (NULL == (bt_name = json_get(bt_info, "name")))
				continue;

			free((char *)d->name);
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
		else_if_FIELD(total_size, "totalLength", ull)
		else_if_FIELD(have, "completedLength", ull)
		else_if_FIELD(uploaded, "uploadLength", ull)
		/* else_if_FIELD(num_seeders, "numSeeders", ul) */
		else_if_FIELD(num_connections, "connections", ul)
#undef else_if_FIELD
	} while (NULL != (field = json_next(field)));

	assert(strlen(d->gid) == 16);
}

void rpc_parse_globalstat(struct json_node *node) {
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

void rpc_parse_downloads(struct json_node *result) {
	/* No more repsonses if no downloads. */
	for (;NULL != result; result = json_next(result)) {
		struct json_node *node;
		struct aria_download *d;

		if (json_obj == json_type(result)) {
			assert(0);
			/* REMOVE torrent, error */
			continue;
		}

		node = json_children(result);

		d = find_download_bygid(json_get(node, "gid")->val.str);

		if (NULL == d)
			d = rpc_download_alloc();
		if (NULL != d)
			rpc_parse_download(d, node);
	}
}

void aria_download_remove(struct aria_download *d, int force) {
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	handler->func = on_rpc_action;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceRemove" : "aria2.remove" );

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	rpc_writer_epilog();
}

int aria_shutdown(int force) {
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = on_rpc_action;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceShutdown" : "aria2.shutdown");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	json_write_endarr(jw);

	rpc_writer_epilog();

	return 0;
}

int aria_pause_all(int pause) {
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = on_rpc_action;
	handler->data = NULL;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? "aria2.pauseAll" : "aria2.unpauseAll");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	json_write_endarr(jw);

	rpc_writer_epilog();
	return 0;
}

int aria_download_pause(struct aria_download *d, int pause) {
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return -1;

	handler->func = on_rpc_action;
	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? "aria2.pause" : "aria2.unpause");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	rpc_writer_epilog();

	return 0;
}

static int on_rpc_notification(char const *method, struct json_node *event) {
	char *const gid = json_get(event, "gid")->val.str;
	struct aria_download *d = find_download_bygid(gid);
	int newstatus;

	if (NULL == d) {
		d = rpc_download_alloc();
		if (NULL == d)
			return 0; /* TODO: Error messag. */

		memcpy(d->gid, gid, sizeof d->gid);
	}

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
		assert(0);
		return 1;
	}

	if (newstatus != d->status) {
		d->status = -newstatus;
		rpc_on_download_status_change(d, d->status);
	}

	return 0;
}

