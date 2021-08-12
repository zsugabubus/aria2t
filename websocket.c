#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
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
#define ARRAY_SIZE(array) (sizeof array / sizeof *array)

#define WS_FIN_TEXT UINT8_C(0x81)
#define WS_MASK_PRESENT UINT8_C(0x80)

static int ws = -1;

static uint8_t *buf;
static uint64_t frame_ptr,
       /* <= */ buf_size,
       /* <= */ buf_alloced;

int
ws_fileno(void)
{
	return ws;
}

int
ws_is_alive(void)
{
	return 0 <= ws;
}

void
ws_close(void)
{
	close(ws), ws = -1;
	on_ws_close();
}

static int
writev_all(int fd, struct iovec *iov, int nb_iov)
{
	for (;;) {
		ssize_t got = writev(fd, iov, nb_iov);
		if (got < 0)
			return -errno;
		else if (!got)
			return -EBADF;

		size_t written = got;

		while (iov[0].iov_len <= written) {
			written -= (iov++)->iov_len;
			if (!--nb_iov)
				return 0;
		}

		*(char **)&iov[0].iov_base += written;
		iov[0].iov_len  -= written;
	}

	return 0;
}

static int
ws_http_upgrade(char const *host, in_port_t port)
{
	static char const HTTP_SWITCHING_PROTOCOLS[] = "HTTP/1.1 101 ";

	int rc;

	buf_size = (size_t)sprintf((char *)buf,
"GET /jsonrpc HTTP/1.1\r\n"
"Host:%s:%hu\r\n"
"Upgrade:websocket\r\n"
"Connection:Upgrade\r\n"
"Sec-WebSocket-Key:AQIDBAUGBwgJCgsMDQ4PEC==\r\n"
"Sec-WebSocket-Version:13\r\n"
"\r\n",
		host, port);

	struct iovec iov = {
		.iov_base = buf,
		.iov_len = buf_size,
	};

	rc = writev_all(ws, &iov, 1);
	if (rc < 0) {
		show_error("%s", strerror(-rc));
		return rc;
	}

	/* Make sure buf always starts with the good prefix. */
	memcpy(buf, HTTP_SWITCHING_PROTOCOLS, strlen(HTTP_SWITCHING_PROTOCOLS));
	buf_size = 0;

	for (;;) {
		ssize_t got = read(ws, buf, buf_alloced - buf_size);
		if (got < 0) {
			rc = errno;
			show_error("%s", strerror(rc));
			return -rc;
		} else if (!got)
			return -EBADF;


		buf_size += (size_t)got;

		if (memcmp(buf, HTTP_SWITCHING_PROTOCOLS, strlen(HTTP_SWITCHING_PROTOCOLS))) {
			show_error("WebSocket connection refused");
			return -EINVAL;
		}

		uint8_t *hdr_end = memmem(buf, buf_size, "\r\n\r\n", 4);
		if (hdr_end) {
			size_t hdr_size = (hdr_end + 4) - buf;
			memmove(buf, buf + hdr_size, (buf_size -= hdr_size));
			return 0;
		}
	}
}

static int
ws_connect(char const *host, in_port_t port)
{
	int ret;
	struct addrinfo *info, hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};
	char port_str[sizeof "65536"];

	assert(!ws_is_alive() && "Already connected");

	sprintf(port_str, "%hu", port);

	if ((ret = getaddrinfo(host, port_str, &hints, &info))) {
		show_error("%s", EAI_SYSTEM == ret
				? strerror(errno)
				: gai_strerror(ret));
		return -1;
	}

	for (struct addrinfo *ai = info; ai; ai = ai->ai_next) {
		ws = socket(ai->ai_family,
				ai->ai_socktype
#ifdef SOCK_CLOEXEC
				| SOCK_CLOEXEC
#endif
				, ai->ai_protocol);
		if (ws < 0)
			continue;

		if (0 <= connect(ws, ai->ai_addr, ai->ai_addrlen))
			break;

		close(ws), ws = -1;
	}

	freeaddrinfo(info);

	if (ws < 0) {
		show_error(strerror(errno));
		return -1;
	}

	show_error(NULL);
	return 0;
}

void
ws_open(char const *host, in_port_t port)
{
	if (ws_connect(host, port) < 0) {
		on_ws_close();
		return;
	}

	buf = realloc(buf, (buf_alloced = BUFSIZ));

	if (ws_http_upgrade(host, port) < 0) {
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
ws_send(char const *msg, size_t msg_size)
{
	struct iovec iov[2];
	uint8_t header[2 /* Header. */ + 8 /* uint64_t length */ + 4 /* Mask. */];
	uint8_t *p = header;

	iov[0].iov_base = header;
	iov[1].iov_base = (uint8_t *)msg;
	iov[1].iov_len = msg_size;

	/* Header. */
	*p++ = WS_FIN_TEXT;

	/* Payload length. */
	if (msg_size < 126) {
		*p++ = WS_MASK_PRESENT | msg_size;
	} else if (msg_size <= UINT16_MAX) {
		*p++ = WS_MASK_PRESENT | 126;

		uint16_t size_be = htobe16(msg_size);
		memcpy(p, &size_be, sizeof size_be);
		p += sizeof size_be;
	} else {
		*p++ = WS_MASK_PRESENT | 127;

		uint64_t size_be = htobe64(msg_size);
		memcpy(p, &size_be, sizeof size_be);
		p += sizeof size_be;
	}

	/* Mask; We use 0, so XOR-ing is convenient. */
	memset(p, 0, sizeof(uint32_t));
	p += sizeof(uint32_t);

	iov[0].iov_len = p - header;

	int rc = writev_all(ws, iov, ARRAY_SIZE(iov));
	if (rc < 0) {
		ws_close();
		return rc;
	}

	return 0;
}

int
ws_recv(void)
{
	ssize_t got = read(ws, buf + buf_size, buf_alloced - buf_size);
	if (got < 0) {
		int rc = -errno;
		ws_close();
		return rc;
	} else if (!got)
		return 0;

	buf_size += (size_t)got;

	uint64_t frame_size;

	for (;;) {
		if (buf_size < frame_ptr + 2) {
			/* Size unknown, use a default value. */
			frame_size = UINT8_MAX;
			break;
		}

		uint64_t payload_size;
		uint8_t header_size = 2;

		uint8_t *payload;
		uint8_t *header = buf + frame_ptr;

		if (header[1] < UINT8_C(126)) {
			payload_size = header[1];
		} else if (UINT8_C(126) == header[1]) {
			if (buf_size < frame_ptr + (header_size += sizeof(uint16_t))) {
				frame_size = 2 + 2 + UINT8_MAX;
				break;
			}

			uint16_t size_be;
			memcpy(&size_be, header + 2, sizeof size_be);
			payload_size = be16toh(size_be);
		} else if (UINT8_C(127) == header[1]) {
			if (buf_size < frame_ptr + (header_size += sizeof(uint64_t))) {
				frame_size = 2 + 8 + UINT16_MAX;
				break;
			}

			uint64_t size_be;
			memcpy(&size_be, header + 2, sizeof size_be);
			payload_size = be64toh(size_be);
		} else {
			assert(!"Received frame has mask bit set.");
		}

		payload = header + header_size;
		frame_size = header_size + payload_size;

		/* Complete packet has been received. */
		if (buf_size < frame_ptr + frame_size)
			break;

		frame_ptr += frame_size;

		switch (header[0]) {
		case WS_FIN_TEXT:
			on_ws_message((char *)payload, payload_size);
			break;

		default:
			assert(!"Unexpected WebSocket frame type");
		}
	}

	if (frame_ptr && buf_alloced < frame_ptr + frame_size) {
		memmove(buf, buf + frame_ptr, buf_size - frame_ptr);
		buf_size -= frame_ptr;
		frame_ptr = 0;
	}

	if (buf_alloced < frame_size) {
		assert(!frame_ptr);

		uint8_t *p = realloc(buf, frame_size);
		if (!p)
			return -ENOMEM;

		buf = p;
		buf_alloced = frame_size;
	}

	return 0;
}
