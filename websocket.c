#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

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

bool
ws_isalive(void)
{
	return 0 <= ws;
}

void
ws_close(void)
{
	close(ws), ws = -1;
	on_ws_close();
}

static bool
http_upgrade(char const *host, in_port_t port)
{
	buflen = (size_t)sprintf(buf,
"GET /jsonrpc HTTP/1.1\r\n"
"Host:%s:%hu\r\n"
"Upgrade:websocket\r\n"
"Connection:Upgrade\r\n"
"Sec-WebSocket-Key:AQIDBAUGBwgJCgsMDQ4PEC==\r\n"
"Sec-WebSocket-Version:13\r\n"
"\r\n",
		host, port);

	if (write(ws, buf, buflen) < 0) {
		error_message = strdup(strerror(errno));
		return false;
	}

#define HTTP_SWITCHING_PROTOCOLS "HTTP/1.1 101 "

	/* make sure buf always starts with the good prefix */
	memcpy(buf, HTTP_SWITCHING_PROTOCOLS, sizeof HTTP_SWITCHING_PROTOCOLS - 1);
	buflen = 0;

	for (;;) {
		ssize_t res;

		if (0 < (res = read(ws, buf, bufsiz - buflen))) {
			char *hdrend;

			buflen += (size_t)res;

			if (0 != memcmp(buf, HTTP_SWITCHING_PROTOCOLS, sizeof HTTP_SWITCHING_PROTOCOLS - 1)) {
				error_message = strdup("WebSocket connection refused");
				return false;
			}

			if (NULL != (hdrend = memmem(buf, buflen, "\r\n\r\n", 4))) {
				size_t const hdrlen = (hdrend + 4) - buf;
				memmove(buf, buf + hdrlen, (buflen -= hdrlen));
				return true;
			}
		} else if (EINTR == errno) {
			continue;
		} else {
			error_message = strdup(strerror(errno));
			return false;
		}
	}

#undef HTTP_SWITCHING_PROTOCOLS
}

static bool
connnect_to(char const *host, in_port_t port)
{
	struct addrinfo hints;
	struct addrinfo *addrs, *p;
	int ret;
	char szport[sizeof "65536"];

	assert("not connected" && !ws_isalive());

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = 0;

	sprintf(szport, "%hu", port);

	if ((ret = getaddrinfo(host, szport, &hints, &addrs))) {
		error_message = EAI_SYSTEM == ret
			? strdup(strerror(errno))
			: strdup(gai_strerror(ret));
		return false;
	}

	for (p = addrs; NULL != p; p = p->ai_next) {
		ws = socket(p->ai_family,
				p->ai_socktype
#ifdef SOCK_CLOEXEC
				| SOCK_CLOEXEC
#endif
				, p->ai_protocol);
		if (-1 == ws)
			continue;

		if (0 == connect(ws, p->ai_addr, p->ai_addrlen))
			break;

		close(ws), ws = -1;
	}

	freeaddrinfo(addrs);

	if (ws < 0) {
		error_message = strdup(strerror(errno));
		return false;
	}

	return true;
}

void
ws_open(char const *host, in_port_t port)
{
	free(error_message), error_message = NULL;

	if (!connnect_to(host, port)) {
		on_ws_close();
		return;
	}

	_buf = realloc(_buf, BUFPAD + (bufsiz = BUFSIZ));

	if (!http_upgrade(host, port)) {
		ws_close();
		return;
	}

#ifndef SOCK_CLOEXEC
	fcntl(ws, F_SETFD, O_CLOEXEC | fcntl(ws, F_GETFD));
#endif
	fcntl(ws, F_SETFL, O_NONBLOCK | fcntl(ws, F_GETFL));

	on_ws_open();
}

int
ws_write(char const *msg, size_t msglen)
{
	struct iovec iov[2];
	/* add 6 pad bytes to the left so we can read all 1, 2 and 8-long
	 * integers aligned */
	uint8_t hdr[6 + 2 + 8 + 4] __attribute__((aligned(8)));
	uint8_t *p = hdr + 6;

	iov[0].iov_base = p;
	iov[1].iov_base = (char *)msg;
	iov[1].iov_len = msglen;

	/* header */
	p[0] = UINT8_C(0x81)/*fin+text*/;
	p[1] = UINT8_C(0x80)/*mask*/;
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
		if (EAGAIN != errno)
			ws_close();
		return -1;
	}

	return 0;
}

/* process all websocket messages (frames) found in buffer */
static void
process_frames(void)
{
	for (;;) {
		uint64_t payloadlen, framelen;
		uint8_t hdrlen;

		if (buflen <= 2)
			return;

		/* server->client must not mask */
		switch ((uint8_t)buf[1]) {
		default:
			hdrlen = 2;
			payloadlen = (uint8_t)buf[1];
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
			switch ((uint8_t)buf[0]) {
			case UINT8_C(0x81): /* fin := 1, rsv{1,2,3} := 0, text frame == 1 */
				on_ws_message((char *)buf + hdrlen, payloadlen);
				memmove(buf, buf + framelen, (buflen -= framelen));
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
	for (;;) {
		ssize_t res;

		if (0 < (res = read(ws, buf + buflen, bufsiz - buflen))) {
			buflen += (size_t)res;
			process_frames();
		} else if (EAGAIN == errno) {
			return;
		} else {
			ws_close();
			return;
		}
	}
}
