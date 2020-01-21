/* Hardwired JSON-RPC over WebSocket implementation for Aria2c.
 *
 * Losely has something to do with the following specifications:
 * - [WebSocket](https://tools.ietf.org/html/rfc6455),
 * - [JSON-RPC](https://www.jsonrpc.org/specification).
 * */
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

#include "websocket.h"
#include "jeezson/jeezson.h"

int ws_fd;
int ws_fd_sflags;

#define HTTP_SWITCHING_PROTOCOLS "HTTP/1.1 101 "
static uint8_t hdrlen;
static unsigned char hdrbuf[2 + 8 + 4];
static char *msgbuf = NULL;
static size_t msgsize = 0;
static size_t msgallocsize = 0;
static size_t msglen = 0;

char const* ws_host;
in_port_t ws_port;

#define container_of(ptr, type, member) (type *)((char *)ptr - offsetof(type, member))
#define array_len(arr) (sizeof(arr) / sizeof *(arr))

static int ws_http_wait_upgrade(void);
static int ws_http_upgrade(void);

int ws_connect(void)
{
	struct addrinfo hints;
	struct addrinfo *addrs, *p;
	int res;
	char ws_port_str[sizeof "65536"];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = 0;

	sprintf(ws_port_str, "%hu", ws_port);

	if ((res = getaddrinfo(ws_host, ws_port_str, &hints, &addrs))) {
		fprintf(stderr, "could'n resolve %s:%s: %s\n",
				ws_host, ws_port_str,
				gai_strerror(res));
		return 1;
	}

	for (p = addrs; NULL != p; p = p->ai_next) {
		ws_fd = socket(p->ai_family,
				p->ai_socktype
#ifdef __linux
					| SOCK_CLOEXEC,
#endif
				p->ai_protocol);
		if (-1 == ws_fd)
			continue;

#ifndef __linux__
		{
			int flags = fcntl(ws_fd, F_GETFD);
			flags |= O_CLOEXEC;
			if (fcntl(ws_fd, F_SETFD, flags))
				fprintf(stderr, "fcntl: %s\n", strerror(errno));
		}
#endif

		ws_fd_sflags = fcntl(ws_fd, F_GETFL);

		if (0 == connect(ws_fd, p->ai_addr, p->ai_addrlen))
			break;

		close(ws_fd);
	}

	freeaddrinfo(addrs);

	if (NULL == p) {
		fprintf(stderr, "couldn't to connect to %s:%s\n",
				ws_host, ws_port_str);
		return 1;
	}

	ws_http_upgrade();

	return p == NULL;
}

int ws_write(char const *msg, size_t msglen)
{
	struct iovec iov[2];
	unsigned char hdr[6 + 2 + 8 + 4] __attribute__((aligned(8)));
	unsigned char *p = hdr + 6;

	iov[0].iov_base = p;
	iov[1].iov_base = (char *)msg;
	iov[1].iov_len = msglen;

	/* Header. */
	p[0] = 0x81U/*fin+text*/;
	p[1] = 0x80U/*mask*/;
	/* Payload length. */
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
	/* Mask. */
	memset(p, 0, 4);
	p += 4;

	iov[0].iov_len = p - (hdr + 6);

	while (-1 == writev(ws_fd, iov, array_len(iov))) {
		if (EINTR == errno)
			continue;
		fprintf(stderr, "write: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int writeall(void const *buf, size_t nbyte) {
	while (-1 == write(ws_fd, buf, nbyte)) {
		if (EINTR == errno)
			continue;
		fprintf(stderr, "write: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int ws_http_upgrade(void)
{
	int r;
	char buf[147 + 256];
	size_t len;

	len = (size_t)sprintf(buf,
"GET /jsonrpc HTTP/1.1\r\n"
"Host:%s:%hu\r\n"
"Upgrade:websocket\r\n"
"Connection:Upgrade\r\n"
"Sec-WebSocket-Key:AQIDBAUGBwgJCgsMDQ4PEC==\r\n"
"Sec-WebSocket-Version:13\r\n"
"\r\n",
		ws_host, ws_port);

	if (writeall(buf, len))
		return 1;

	return ws_http_wait_upgrade();
}


static int ws_http_wait_upgrade(void) {
	unsigned nstatus = 0;
	unsigned nchr = 0;

	for (;;) {
		char buf[1024];
		ssize_t len = 0;
		unsigned n;
		char *p;

	read_more:
		while (-1 == (len = read(ws_fd, buf, sizeof buf))) {
			if (EINTR == errno)
				continue;
			fprintf(stderr, "write: %s\n", strerror(errno));
			return -1;
		}
		if (0 == len)
			return 0;

		n = strlen(HTTP_SWITCHING_PROTOCOLS) - nstatus;
		if (n > len)
			n = len;
		if (0 != memcmp(buf, HTTP_SWITCHING_PROTOCOLS + nstatus, n))
			return 1;
		nstatus += n;

		for (p = buf;;++p) {
			switch (nchr) {
			case 0:
				if (NULL == (p = memchr(p, '\r', len - (p - buf))))
					goto read_more;
				nchr = 1;
				break;
			case 4: {
				len -= p - buf;
				if (len > 0)
					assert(0);
				return 0;
			}
			default:
				if (p < buf + len) {
					if (*p == "\r\n\r\n"[nchr])
						++nchr;
					else
						nchr = 0;
				} else {
					goto read_more;
				}
				break;
			}
		}
	}
}

int ws_shutdown(void) {
	int ret;

	if ((ret = writeall("\x80\x8a", 2)))
		return ret;
	shutdown(ws_fd, SHUT_WR);

	return ret;
}

static int ws_process_chunk(char *buf, size_t len) {
	char *p = buf;
	size_t remain = len;

	do {
		int need_more;
		size_t need;

		/* printf("remain=%d; hdrlen=%d\n", (int)len, hdrlen); */
		if (hdrlen != sizeof hdrbuf) {
			if (hdrlen >= 2) {
			parse_hdr:
				/* printf("header: %x%x\n", hdrbuf[0], hdrbuf[1]); */
				assert(!(hdrbuf[1] & 0x80));
				switch (hdrbuf[1] & 0x7f) {
				default:
					need = 2;
					break;
				case 126:
					need = 2 + sizeof(uint16_t);
					break;
				case 127:
					need = 2 + sizeof(uint64_t);
					break;
				}
			} else {
				need = 2;
			}

			need -= hdrlen;
			if ((need_more = remain < need))
				need = remain;
			/* printf("new hdrlen=%d need=%d need_more=%d remain=%d\n", hdrlen, (int)need, need_more, (int)remain); */
			memcpy(&hdrbuf[hdrlen], p, need);
			p += need, remain -= need;

			if (!need_more) {
				uint8_t payloadlen8;
				uint64_t payloadlen;

				if (hdrlen < 2) {
					hdrlen = 2;
					goto parse_hdr;
				}

				payloadlen8 = hdrbuf[1] & 0x7f;
				switch (payloadlen8) {
				default:
					payloadlen = payloadlen8;
					break;
				case 126: {
					uint16_t payloadlen16_be;
					memcpy(&payloadlen16_be, &hdrbuf[2], sizeof payloadlen16_be);
					payloadlen = be16toh(payloadlen16_be);
				}
					break;
				case 127: {
					uint64_t payloadlen64_be;
					memcpy(&payloadlen64_be, &hdrbuf[2], sizeof payloadlen64_be);
					payloadlen = be64toh(payloadlen64_be);
				}
					break;
				}

				/* printf("frame hdr parsed; hdrlen=%d paylen=%d %x %x\n", hdrlen, (int)payloadlen, hdrbuf[0], hdrbuf[1]); */
				if (hdrbuf[1] & 0x80)
				assert(!"mask not expected");

				assert(msgsize == msglen);
				msgsize = msglen + payloadlen;
				if (msgsize > msgallocsize) {
					msgbuf = realloc(msgbuf, (msgallocsize = msgsize));
					if (NULL == msgbuf) {
						ws_shutdown();
						break;
					}
				}

				hdrlen = sizeof hdrbuf;
			} else {
				hdrlen += need;
			}
		}

		if (hdrlen == sizeof hdrbuf) {
			need = msgsize - msglen;
			/* printf("msg len=%d size=%d; readable=%d\n", (int)msglen, (int)msgsize, (int)remain); */
			if ((need_more = remain < need))
				need = remain;
			/* printf("read=+%d\n", (int)need); */

			memcpy(msgbuf + msglen, p, need);
			msglen += need, p += need, remain -= need;

			if (!need_more) {
				int result;
				/* printf("msg !!! %*s \n", (int)msglen, msgbuf); */

				switch (hdrbuf[0] & 0x7f) {
				case 0x1:
					result = on_ws_message(msgbuf, msglen);
					break;
				case 0xa:
					result = ws_shutdown();
					break;
				default:
					/* printf("type=%x\n", hdrbuf[0]); */
					assert(0);
				}
				msgsize = 0;
				msglen = 0;
				hdrlen = 0;
				if (result)
					return result;
			}
		}
	} while (remain > 0);

	return 0;
}

int ws_read(void)
{
	int result = 0;

	if (fcntl(ws_fd, F_SETFL, ws_fd_sflags | O_NONBLOCK)) {
		fprintf(stderr, "fcntl: %s\n", strerror(errno));
		return -1;
	}

	for (;;) {
		char buf[8192];
		ssize_t len;

		if (-1 == (len = read(ws_fd, buf, sizeof buf))) {
			if (EAGAIN != errno) {
				result = -1;
				fprintf(stderr, "read: %s\n", strerror(errno));
			}
			break;
		}
		if (len == 0)
			break;

		if ((result = ws_process_chunk(buf, (size_t)len)))
			break;
	}

	if (fcntl(ws_fd, F_SETFL, ws_fd_sflags)) {
		fprintf(stderr, "fcntl: %s\n", strerror(errno));
		result = -1;
	}

	return result;
}
/* vi:set noet: */
