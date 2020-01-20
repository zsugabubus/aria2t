#ifndef RPC_H
#define RPC_H
#include <netinet/in.h>
#include "uv.h"

extern char const* rpc_host;
extern in_port_t rpc_port;

int rpc_init(void);
int rpc_connect(void);

int rpc_write(char *msg, size_t msglen);
void on_rpc_connect(void);
void on_rpc_message(char *msg, size_t msglen);

#endif
