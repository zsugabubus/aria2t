/* A minimal WebSocket implementation. */
#include <stdlib.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/uio.h>

#include "program.h"
#include "websocket.h"

#define container_of(ptr, type, member) (type *)((char *)ptr - offsetof(type, member))
#define array_len(arr) (sizeof(arr) / sizeof *(arr))

static int ws = -1;

#define BUFPAD 6
static uint64_t buflen;
static uint64_t bufsiz;
static char *_buf;
#define buf (_buf + BUFPAD)

int
ws_fileno(void)
{
	return ws;
}

int
ws_isalive(void)
{
	return 0 <= ws;
}

static int
expect_http_upgrade(void)
{
	for (;;) {
		ssize_t res;

		if (0 <= (res = read(ws, buf, bufsiz - buflen - 1/*nil*/))) {
			char *end;

			buflen += (size_t)res;

#define HTTP_SWITCHING_PROTOCOLS "HTTP/1.1 101 "
			if (sizeof HTTP_SWITCHING_PROTOCOLS - 1 <= buflen) {
				if (0 != memcmp(buf, HTTP_SWITCHING_PROTOCOLS, sizeof HTTP_SWITCHING_PROTOCOLS - 1))
					return -1;
			}

			buf[buflen] = '\0';
			if (NULL != (end = strstr(buf, "\r\n\r\n"))) {
				buflen -= ((end + 4) - buf);
				return 0;
			}
		}

	}

}

static int
http_upgrade(char const *host, in_port_t port)
{
	size_t len;

	len = (size_t)sprintf(buf,
"GET /jsonrpc HTTP/1.1\r\n"
"Host:%s:%hu\r\n"
"Upgrade:websocket\r\n"
"Connection:Upgrade\r\n"
"Sec-WebSocket-Key:AQIDBAUGBwgJCgsMDQ4PEC==\r\n"
"Sec-WebSocket-Version:13\r\n"
"\r\n",
		host, port);

	if (write(ws, buf, len) < 0)
		return -1;

	return expect_http_upgrade();
}

void
ws_connect(char const *host, in_port_t port)
{
	struct addrinfo hints;
	struct addrinfo *addrs, *p;
	int ret;
	char portstr[sizeof "65536"];

	assert("not connected" && 0 > ws);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = 0;

	sprintf(portstr, "%hu", port);

	if ((ret = getaddrinfo(host, portstr, &hints, &addrs))) {
		free(error_message);
		if (EAI_SYSTEM != ret)
			error_message = strdup(gai_strerror(ret));
		else
			error_message = strdup(strerror(errno));
		return;
	}

	ws = -1;
	for (p = addrs; NULL != p; p = p->ai_next) {
		ws = socket(p->ai_family,
				p->ai_socktype
#ifdef SOCK_CLOEXEC
				| SOCK_CLOEXEC,
#endif
				p->ai_protocol);
		if (-1 == ws)
			continue;

#ifndef SOCK_CLOEXEC
		{
			int flags = fcntl(ws, F_GETFD);
			flags |= O_CLOEXEC;
			(void)fcntl(ws, F_SETFD, flags);
		}
#endif

		if (0 == connect(ws, p->ai_addr, p->ai_addrlen))
			break;

		close(ws);
	}

	freeaddrinfo(addrs);

	buflen = 0;
	_buf = realloc(_buf, BUFPAD + (bufsiz = BUFSIZ));

	if (ws < 0) {
		free(error_message), error_message = strdup(strerror(errno));
	} else if (http_upgrade(host, port)) {
		close(ws), ws = -1;
		free(error_message), error_message = strdup("connection refused");
	} else {
		on_ws_open();
	}
}

void
ws_write(char const *msg, size_t msglen)
{
	struct iovec iov[2];
	unsigned char hdr[6 + 2 + 8 + 4] __attribute__((aligned(8)));
	unsigned char *p = hdr + 6;

	iov[0].iov_base = p;
	iov[1].iov_base = (char *)msg;
	iov[1].iov_len = msglen;

	/* header */
	p[0] = 0x81U/*fin+text*/;
	p[1] = 0x80U/*mask*/;
	/* payload length */
	if (msglen < 126) {
		p[1] |= msglen;
		p += 2;
	} else if (msglen <= UINT16_MAX) {
		p[1] |= 126;
		*(uint16_t *)(p += 2) = htobe16((uint16_t)msglen);
		p += sizeof(uint16_t);
	} else {
		p[1] |= 127;
		*(uint64_t *)(p += 2) = htobe64((uint64_t)msglen);
		p += sizeof(uint64_t);
	}
	/* mask; we use 0, so XOR-ing is fast and easy */
	memset(p, 0, sizeof(uint32_t));
	p += sizeof(uint32_t);

	iov[0].iov_len = p - (hdr + 6);

	while (-1 == writev(ws, iov, array_len(iov))) {
		if (EINTR == errno)
			continue;
		on_ws_close();
		close(ws), ws = -1;
		return;
	}
}

/* process all websocket messages (frames) found in buffer */
static void
ws_process_frames(void)
{
	for (;;) {
		uint64_t payloadlen, framelen;
		uint8_t hdrlen;

		if (buflen <= 2)
			return;

		/* server->client must not mask */
		switch (buf[1]) {
		default:
			hdrlen = 2;
			payloadlen = buf[1];
			break;

		case 126:
			if (buflen <= (hdrlen = 2 + sizeof(uint16_t)))
				return;
			payloadlen = be16toh(*(uint16_t *)(buf + 2));
			break;

		case 127:
			if (buflen <= (hdrlen = 2 + sizeof(uint64_t)))
				return;
			payloadlen = be64toh(*(uint64_t *)(buf + 2));
			break;
		}

		framelen = hdrlen + payloadlen;
		/* do we have the whole frame in buffer? */
		if (framelen <= buflen) {
			switch ((unsigned char)buf[0]) {
			case 0x81U: /* fin := 1, rsv{1,2,3} := 0, text frame */
				on_ws_message(buf + hdrlen, payloadlen);
				buflen -= framelen;
				memmove(buf, buf + framelen, buflen);
				break;

			default:
				assert(!"unexpected websocket frame");
			}
		} else {
			/* maybe we have to realloc for the next read? */
			if (bufsiz < framelen) {
				char *p;

				if (NULL == (p = realloc(_buf, BUFPAD + framelen)))
					return;
				_buf = p;
				bufsiz = framelen;
			}

			return;
		}
	}
}

void
ws_read(void)
{
	ssize_t res;

again:
	if (0 < (res = read(ws, buf + buflen, bufsiz - buflen))) {
		buflen += (size_t)res;
		ws_process_frames();
	} else if (EINTR == errno) {
		goto again;
	} else {
		on_ws_close();
		close(ws), ws = -1;
	}
}
