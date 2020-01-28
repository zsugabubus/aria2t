#ifndef ARIA_RPC_H
#define ARIA_RPC_H
#include <stdint.h>
#include <netinet/in.h>

#include "websocket.h"
#include "jeezson/jeezson.h"

extern char const *const NONAME;

extern struct aria_globalstat globalstat;
extern struct aria_download *downloads;
extern size_t num_downloads;

extern char *rpc_secret;
extern char const* rpc_host;
extern in_port_t rpc_port;

extern struct json_writer jw[1];

struct aria_download *find_download_bygid(char const*gid);

/* Definitions. */

#define DOWNLOAD_UNKNOWN  0
#define DOWNLOAD_ACTIVE   1
#define DOWNLOAD_WAITING  2
#define DOWNLOAD_PAUSED   3
#define DOWNLOAD_ERROR    4
#define DOWNLOAD_COMPLETE 5
#define DOWNLOAD_REMOVED  6

struct aria_file {
	char *path;
	uint64_t total_size;
	uint64_t have;
	int selected;
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
	struct aria_download *next;

	char const*name;
	char gid[16 + 1];
	/* DOWNLOAD_*; Negative if changed. */
	int status;
	uint32_t num_files;
	uint32_t num_pieces;
	uint32_t piece_size;

	uint32_t num_seeders;
	uint32_t num_connections;

	uint64_t total_size;
	uint64_t have;
	uint64_t uploaded;
	char *progress;

	uint32_t download_speed;
	uint32_t upload_speed;

	struct aria_file *files;

};

struct aria_globalstat {
	uint64_t download_speed;
	uint64_t upload_speed;

	uint32_t num_active;
	uint32_t num_waiting;
	uint32_t num_stopped;
	uint32_t num_stopped_total;
};

int rpc_load_cfg(void);
int rpc_connect(void);
int rpc_shutdown(void);

void aria_download_remove(struct aria_download *d, int force);

int aria_shutdown(int force);
int aria_download_pause(struct aria_download *d, int pause);
int aria_pause_all(int pause);

void rpc_parse_downloads(struct json_node *result);
void rpc_parse_downloads(struct json_node *result);
void rpc_parse_download(struct aria_download *d, struct json_node *node);
void rpc_parse_download_files(struct aria_download *d, struct json_node *node);
void rpc_parse_globalstat(struct json_node *node);

int rpc_on_error(struct json_node *error, void *data);
void rpc_on_download_status_change(struct aria_download *d, int status);

typedef int(*rpc_handler)(struct json_node *result, void *data);

struct rpc_handler {
	rpc_handler func;
	void *data;
};

extern struct rpc_handler rpc_handlers[10];

void rpc_writer_epilog();
struct rpc_handler *rpc_writer_prolog();

struct aria_download *rpc_download_alloc(void);

#endif
