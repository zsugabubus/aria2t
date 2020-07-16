#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "program.h"
#include "rpc.h"
#include "websocket.h"
#include "jeezson/jeezson.h"
#include "b64.h"

static struct json_node *nodes;
static size_t nnodes;

struct aria_globalstat globalstat;
struct aria_download **downloads;
size_t num_downloads;

char *rpc_secret;
char const* rpc_host;
in_port_t rpc_port;

struct json_writer jw[1];

struct rpc_handler rpc_handlers[10];
#define ARRAY_LEN(a) (sizeof(a) / sizeof(*a))

static void
on_rpc_notification(char const *method, struct json_node *event);

static void
on_rpc_action(struct json_node *result, void *data)
{
	(void)result, (void)data;
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

int
rpc_connect(void)
{
	return ws_connect(rpc_host, rpc_port);
}

int
rpc_shutdown(void)
{
	return ws_shutdown();
}

struct aria_download *rpc_download_alloc(void) {
	void **p;
	struct aria_download *d;

	p = realloc(downloads, (num_downloads + 1) * sizeof *downloads);
	if (NULL == p)
		return NULL;

	if (NULL == (d = calloc(1, sizeof *d)))
		return NULL;

	(downloads = p)[num_downloads++] = d;

	return d;
}

static struct rpc_handler *
rpc_handler_alloc(void)
{
	size_t n = ARRAY_LEN(rpc_handlers);

	while (n > 0) {
		if (NULL == rpc_handlers[--n].proc) {
			rpc_handlers[n].proc = on_rpc_action;
			return &rpc_handlers[n];
		}
	}

	return NULL;
}

struct aria_download **
get_download_bygid(char const*gid)
{
	struct aria_download *d, **dd = downloads;
	struct aria_download **const end = &downloads[num_downloads];

	for (; dd < end; ++dd)
		if (0 == memcmp((*dd)->gid, gid, sizeof (*dd)->gid))
			return dd;

	if (NULL == (d = rpc_download_alloc()))
		return NULL;

	memcpy(d->gid, gid, sizeof d->gid);

	dd = &downloads[num_downloads - 1];
	assert(*dd == d);

	return dd;
}

int
on_ws_message(char *msg, size_t msglen)
{
	struct json_node *id;
	struct json_node *method;

	(void)msglen;

	json_parse(msg, &nodes, &nnodes);

	if (NULL != (id = json_get(nodes, "id"))) {
		struct json_node *const result = json_get(nodes, "result");

		free(last_error);
		last_error = NULL;

		if (NULL != result) {
			struct rpc_handler *const handler = &rpc_handlers[(unsigned)id->val.num];

			assert(NULL != handler->proc);
			handler->proc(result, handler->data);
			handler->proc = NULL;
		} else {
			rpc_on_error(json_get(nodes, "error"));
		}

	} else if (NULL != (method = json_get(nodes, "method"))) {
		struct json_node *const params = json_get(nodes, "params");

		on_rpc_notification(method->val.str, params + 1);
	} else {
		assert(0);
	}

	/* json_debug(nodes, 0); */
	/* json_debug(json_get(json_get(nodes, "result"), "version"), 0); */
	return 0;
}

struct rpc_handler *rpc_writer_prolog() {
	json_write_beginobj(jw);

	json_write_key(jw, "jsonrpc");
	json_write_str(jw, "2.0");

	json_write_key(jw, "id");
	return rpc_handler_alloc();
}

void
rpc_writer_epilog(struct rpc_handler *handler)
{
	json_write_endobj(jw);

	if (ws_write(jw->buf, jw->len)) {
		assert(0);
		handler->proc = NULL;
	}

	json_writer_empty(jw);
}

void rpc_parse_download_files(struct aria_download *d, struct json_node *node) {
	void *p;

	if (d->files) {
		size_t i;
		for (i = 0; i < d->num_files; ++i)
			free(d->files[i].path);
	}

	d->num_files = json_len(node);
	d->num_selfiles = 0;
	if (NULL == (p = realloc(d->files, d->num_files * sizeof *(d->files)))) {
		d->num_files = 0;
		free(d->files), d->files = NULL;
		return;
	} else {
		d->files = p;
	}

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
					struct aria_uri uri;

					do {
						if (0 == strcmp(field->key, "status")) {
							if (0 == strcmp(field->val.str, "used"))
								uri.status = aria_uri_status_used;
							else if (0 == strcmp(field->val.str, "waiting"))
								uri.status = aria_uri_status_waiting;
							else
								uri.status = aria_uri_status_unknown;
						} else if (0 == strcmp(field->key, "uri"))
							uri.uri = strdup(field->val.str);
					} while (NULL != (field = json_next(field)));
					file.uris[uriidx++] = uri;
				} while (NULL != (uris = json_next(uris)));
			}
			else
				assert(!"unknown key in file");
		} while (NULL != (field = json_next(field)));

		assert(index >= 0);
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
			if (NULL == d->files || d->num_files == 0)
				continue;

			if (NULL == d->name) {
				char *path = d->files[0].path;
				if (NULL != path) {
					char *last_slash = strrchr(path, '/');
					d->name = strdup(NULL != last_slash ? last_slash + 1 : d->files[0].path);
				}
			}

			if (NULL == d->name && d->files[0].num_uris > 0) {
				char *uri = d->files[0].uris[0].uri;
				if (NULL != uri) {
					d->name = strdup(uri);
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
		else_if_FIELD(total, "totalLength", ull)
		else_if_FIELD(have, "completedLength", ull)
		else_if_FIELD(uploaded, "uploadLength", ull)
		/* else_if_FIELD(num_seeders, "numSeeders", ul) */
		else_if_FIELD(num_connections, "connections", ul)
#undef else_if_FIELD
		else if (0 == strcmp(field->key, "following")) {
			struct aria_download **dd = get_download_bygid(field->val.str);
			d->following = NULL != dd ? *dd : NULL;
		} else if (0 == strcmp(field->key, "belongsTo")) {
			struct aria_download **dd = get_download_bygid(field->val.str);
			assert(!"eeeeeeeee");
			d->belongs_to = NULL != dd ? *dd : NULL;
		} else if (0 == strcmp(field->key, "errorMessage")) {
			free(d->error_message);
			d->error_message = strdup(field->val.str);
		}
	} while (NULL != (field = json_next(field)));

	assert(strlen(d->gid) == 16);
}

char *
rpc_parse_session_info(struct json_node *node)
{
	/* First kv-pair.  */
	node = json_children(node);
	do {
		if (0 == strcmp(node->key, "sessionId"))
			return node->val.str;
	} while (NULL != (node = json_next(node)));

	return NULL;
}

void
rpc_parse_options(struct json_node *node, void(*cb)(char const *, char const *, void *), void *arg)
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

void
rpc_parse_globalstat(struct json_node *node)
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
		else {
		printf(">>>%s %s<<<\n", node->key, node->val.str);
			/* assert(!"unknown key in global stat"); */

		}
	} while (NULL != (node = json_next(node)));
}

void rpc_parse_downloads(struct json_node *result) {
	/* No more repsonses if no downloads. */
	for (;NULL != result; result = json_next(result)) {
		struct json_node *node;
		struct aria_download **dd;
		struct aria_download *d;

		if (json_obj == json_type(result)) {
			/* FIXME: REMOVE torrent, error + handle other cases too when gid not arrived */
			continue;
		}

		node = json_children(result);

		if (NULL != (dd = get_download_bygid(json_get(node, "gid")->val.str))) {
			d = *dd;

			/* these fields are not included in response if not
			 * applicable, so we do a self reference loop to
			 * indicate that we do not need to request these next
			 * time */
			if (NULL == d->belongs_to)
				d->belongs_to = d;
			if (NULL == d->following)
				d->following = d;

			rpc_parse_download(d, node);
		}
	}
}

void
aria_download_repos(struct aria_download *d, int32_t pos, enum pos_how how)
{
	static char const *const POS_HOW[] = {
		"POS_SET",
		"POS_CUR",
		"POS_END"
	};

	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, "aria2.changePosition");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	/* “pos” */
	json_write_int(jw, pos);
	/* “how” */
	json_write_str(jw, POS_HOW[how]);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);

}

void
aria_download_remove(struct aria_download *d, int force)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceRemove" : "aria2.remove");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

void
aria_shutdown(int force)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, force ? "aria2.forceShutdown" : "aria2.shutdown");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

void
aria_pause_all(int pause, int force)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? (force ? "aria2.forcePauseAll" : "aria2.pauseAll") : "aria2.unpauseAll");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

void
aria_download_pause(struct aria_download *d, int pause, int force)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, pause ? (force ? "aria2.forcePause" : "aria2.pause") : "aria2.unpause");

	json_write_key(jw, "params");
	json_write_beginarr(jw);
	/* “secret” */
	json_write_str(jw, rpc_secret);
	/* “gid” */
	json_write_str(jw, d->gid);
	json_write_endarr(jw);

	rpc_writer_epilog(handler);
}

void
aria_remove_result(struct aria_download *d)
{
	struct rpc_handler *handler = rpc_writer_prolog();
	if (NULL == handler)
		return;

	json_write_int(jw, (int)(handler - rpc_handlers));

	json_write_key(jw, "method");
	json_write_str(jw, NULL != d ? "aria2.removeDownloadResult" : "aria2.purgeDownloadResult");

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
}

static void
on_rpc_notification(char const *method, struct json_node *event)
{
	char *const gid = json_get(event, "gid")->val.str;
	struct aria_download **dd = get_download_bygid(gid);
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
		rpc_on_download_status_change(d, d->status);
	}
}

