#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <netinet/in.h>
#include <stdbool.h>

void ws_open(char const *host, in_port_t port);
void ws_close(void);
int ws_fileno(void);
int ws_is_alive(void);
int ws_recv(void);
int ws_send(char const *msg, size_t msg_size);

extern void on_ws_message(char *msg, uint64_t msg_size);
extern void on_ws_close(void);
extern void on_ws_open(void);

#endif
