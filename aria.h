#include <stdint.h>

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
	uint32_t rx_speed;
	uint32_t tx_speed;
};

struct aria_download {
	struct aria_download *next;

	char const*name;
	char gid[16 + 1];
	int status;
	uint32_t num_files;
	uint32_t num_pieces;
	uint32_t piece_size;

	uint64_t total_size;
	uint64_t have;
	uint64_t uploaded;
	char *progress;

	uint32_t rx_speed;
	uint32_t tx_speed;

	struct aria_file *files;

};


struct aria_globalstat {
	uint64_t rx_speed;
	uint64_t tx_speed;

	uint32_t num_active;
	uint32_t num_waiting;
	uint32_t num_stopped;
	uint32_t num_stopped_total;
};
