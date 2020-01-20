#include <stdint.h>

enum aria_download_status {
    download_active,
    download_waiting,
    download_paused,
    download_error,
    download_complete,
    download_removed,
};

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
    uint64_t rx_speed; /* From. */
    uint64_t tx_speed; /* To. */
};

struct aria_download {
    char gid;
    enum aria_download_status status;
    uint64_t total_size;
    uint64_t have;
    uint64_t uploaded;
    uint64_t num_pieces;
    uint32_t piece_size;
    char *progress;

    uint64_t rx_speed;
    uint64_t tx_speed;

    uint32_t num_files;
    struct aa_file *files;
};


struct aria_stat {
    uint64_t rx_speed;
    uint64_t tx_speed;

    uint32_t num_active;
    uint32_t num_waiting;
    uint32_t num_stopped;
    uint32_t num_stopped_total;
};
