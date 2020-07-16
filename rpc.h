#ifndef ARIAT_RPC_H
#define ARIAT_RPC_H
#include <stdint.h>
#include <netinet/in.h>

#include "websocket.h"
#include "jeezson/jeezson.h"

extern char const *const NONAME;

extern struct aria_globalstat globalstat;
extern struct aria_download **downloads;
extern size_t num_downloads;

extern char *rpc_secret;
extern char const *rpc_host;
extern in_port_t rpc_port;

extern struct json_writer jw[1];

struct aria_download **get_download_bygid(char const *gid);

/* NOTE: order is relevant for sorting */
#define DOWNLOAD_UNKNOWN  0
#define DOWNLOAD_WAITING  1
#define DOWNLOAD_ACTIVE   2
#define DOWNLOAD_PAUSED   3
#define DOWNLOAD_COMPLETE 4
#define DOWNLOAD_REMOVED  5
#define DOWNLOAD_ERROR    6

enum aria_uri_status {
	aria_uri_status_unknown,
	aria_uri_status_used,
	aria_uri_status_waiting,
	aria_uri_status_count
};

struct aria_uri {
	enum aria_uri_status status;
	char *uri;
};

struct aria_file {
	char *path;
	uint64_t total;
	uint64_t have;
	unsigned selected: 1;
	uint32_t num_uris;
	struct aria_uri *uris;
};

struct aria_peer {
	char *id;
	char *ip_addr;
	uint16_t port;

	char *progress;

	int rx_choked;
	int tx_choked;
	uint32_t download_speed;
	uint32_t upload_speed;
};

struct aria_download {
	char const *name;
	char gid[16 + 1];
	uint8_t refcnt;
	/* DOWNLOAD_*; or negative if changed. */
	int status;
	uint32_t queue_index;

	char *error_message;

	uint32_t num_files;
	uint32_t num_selfiles;
	/* uint32_t num_pieces;
	uint32_t piece_size; */

	uint32_t num_seeders;
	uint32_t num_connections;

	uint64_t total;
	uint64_t have;
	uint64_t uploaded;
	char *progress;

	uint32_t download_speed;
	uint32_t upload_speed;
	uint32_t download_speed_limit;
	uint32_t upload_speed_limit;

	struct aria_file *files;
	struct aria_download *belongs_to;
	struct aria_download *following;
};

struct aria_globalstat {
	uint64_t download_speed;
	uint64_t upload_speed;
	uint64_t download_speed_limit;
	uint64_t upload_speed_limit;

	uint64_t upload_total;
	/* uint64_t progress_total_total;
	uint64_t progress_have_total; */
	uint64_t have_total;
	uint8_t compute_total;

	unsigned optimize_concurrency: 1;
	unsigned save_session: 1;
	uint32_t max_concurrency;

	uint32_t num_active;
	uint32_t num_waiting;
	uint32_t num_stopped;
	uint32_t num_stopped_total;
};

int rpc_load_cfg(void);
int rpc_connect(void);
int rpc_shutdown(void);

void aria_download_remove(struct aria_download *d, int force);
void aria_shutdown(int force);
void aria_download_pause(struct aria_download *d, int pause, int force);
void aria_pause_all(int pause, int force);
void aria_remove_result(struct aria_download *d);
enum pos_how { POS_SET, POS_CUR, POS_END };
void aria_download_repos(struct aria_download *d, int32_t pos, enum pos_how how);

void rpc_parse_downloads(struct json_node *result);
void rpc_parse_download(struct aria_download *d, struct json_node *node);
void rpc_parse_download_files(struct aria_download *d, struct json_node *node);
void rpc_parse_globalstat(struct json_node *node);
void rpc_parse_options(struct json_node *node, void(*cb)(char const *, char const *, void *), void *arg);
char *rpc_parse_session_info(struct json_node *node);
void rpc_on_error(struct json_node *error);
void rpc_on_download_status_change(struct aria_download *d, int status);

typedef void(*rpc_handler)(struct json_node *result, void *data);

struct rpc_handler {
	rpc_handler proc;
	void *data;
};

extern struct rpc_handler rpc_handlers[10];

void rpc_writer_epilog(struct rpc_handler *handler);
struct rpc_handler *rpc_writer_prolog(void);

struct aria_download *rpc_download_alloc(void);

#endif
