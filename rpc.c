/* Hardwired JSON-RPC over WebSocket implementation for Aria2c.
 *
 * Losely has something to do with the following specifications:
 * - [WebSocket](https://tools.ietf.org/html/rfc6455),
 * - [JSON-RPC](https://www.jsonrpc.org/specification).
 * */
#include <stdlib.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
/* #include <sys/socket.h> */
#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <assert.h>
#include <stdio.h>

#include <uv.h>
#include "uv.h"
#include "rpc.h"
#include "jeezson/jeezson.h"

#define HTTP_SWITCHING_PROTOCOLS "HTTP/1.1 101 "
static union {
    union {
        uv_getaddrinfo_t getaddrinfo_req;
        uv_connect_t connect_req;
        uv_write_t http_write_req;
        uv_write_t ws_close_write_req;
        uv_shutdown_t ws_shutdown_req;
    } req;

    struct {
        char status_line[sizeof HTTP_SWITCHING_PROTOCOLS - 1];
        unsigned char nread;
        unsigned char ncrlfcrlf;
    } http_resp;
} u;
static uint8_t hdrlen;
static char hdrbuf[2 + 8 + 4];
static char *framebuf = NULL;
static size_t framesize;
static size_t framelen = 0;
static char *msgbuf = NULL;
static size_t msgsize = 0;
static size_t msgallocsize = 0;
static size_t msglen = 0;

static uv_tcp_t tcp;
#define tcp_stream (uv_stream_t *)&tcp

struct frame_write_context {
    uv_write_t write_req;
    uv_buf_t bufs[2];
    unsigned char header[2 + 2 + 4] __attribute__((aligned(2)));
};

static int s_ctx_free = 1;
struct frame_write_context s_ctx;

char const* rpc_host;
in_port_t rpc_port;

#define container_of(ptr, type, member) (type *)((char *)ptr - offsetof(type, member))
#define array_len(arr) (sizeof(arr) / sizeof *(arr))

/* https://github.com/libuv/libuv/blob/v1.x/samples/socks5-proxy/client.c */

static void on_ws_connect(void);
static void on_http_connect(uv_connect_t *req, int status);
static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void on_connect(uv_connect_t *req, int status);
static void on_host_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res);
static void on_http_wrote(uv_write_t *req, int status);
static void on_ws_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void on_ws_wrote(uv_write_t *req, int status);

static void on_ws_close_finished(uv_shutdown_t *_req, int status) {
    assert(status == 0);
}

static void on_ws_close_wrote(uv_write_t *req, int status) {
    assert(status == 0);
    uv_shutdown(&u.req.ws_shutdown_req, tcp_stream, on_ws_close_finished);
}

static void on_http_close(uv_handle_t *stream);

int rpc_init(void)
{
    printf("init\n");
    return uv_tcp_init(uv_loop, &tcp);
}

int rpc_connect(void)
{
    struct addrinfo ai;

    memset(&ai, 0, sizeof ai);
    ai.ai_family = PF_INET;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_protocol = IPPROTO_TCP;
    ai.ai_flags = 0;

    return uv_getaddrinfo(uv_loop, &u.req.getaddrinfo_req, on_host_resolved, rpc_host, NULL, &ai);
}

static void on_host_resolved(uv_getaddrinfo_t *_req, int status, struct addrinfo *ai) {
    assert(status == 0);

    ((struct sockaddr_in *)ai->ai_addr)->sin_port = htons(rpc_port);
    uv_tcp_connect(&u.req.connect_req, &tcp, (struct sockaddr const*)ai->ai_addr, on_http_connect);
    uv_freeaddrinfo(ai);
}

static void alloc_cb(uv_handle_t *_handle, size_t size, uv_buf_t *buf) {
    if (size > framesize || (size < framesize / 2 && size > 4096)) {
        framebuf = realloc(framebuf, (framesize = size));
        assert(framebuf);
    }

    buf->base = framebuf;
    buf->len = framesize;
}


int rpc_write(char *msg, size_t msglen)
{
    int r = -1;
    struct frame_write_context *ctx;
    unsigned char *p;
    size_t len;

    if (s_ctx_free) {
        s_ctx_free = 0;
        ctx = &s_ctx;
    } else {
        if (NULL == (ctx = malloc(sizeof *ctx)))
            goto err_alloc_ctx;
    }

    ctx->bufs[0].base = (char *)ctx->header;
    ctx->bufs[1].base = msg;
    ctx->bufs[1].len = msglen;

    p = ctx->header;
    /* Header. */
    p[0] = 0x81U/*fin+text*/;
    p[1] = 0x80U/*mask*/;
    /* Payload length. */
    if (msglen < 126) {
        p[1] |= msglen;
        p += 2;
    } else {
        assert(msglen <= UINT16_MAX);
        p[1] |= 126;
        *(uint16_t *)(p += 2) = htobe16((uint16_t)msglen);
        p += sizeof(uint16_t);
    }
    /* Mask. */
    memset(p, 0, 4);
    p += 4;

    ctx->bufs[0].len = (char *)p - ctx->bufs[0].base;

    if (0 == (r = uv_write(&ctx->write_req, tcp_stream, ctx->bufs, array_len(ctx->bufs), on_ws_wrote)))
        return 0;

    if (ctx == &s_ctx)
        s_ctx_free = 1;
    else
        free(ctx);
err_alloc_ctx:
    return r;
}

static void on_ws_wrote(uv_write_t *req, int status) {
    struct frame_write_context *ctx = container_of(req, struct frame_write_context, write_req);

    free(ctx->bufs[1].base);

    if (ctx == &s_ctx)
        s_ctx_free = 1;
    else
        free(ctx);
}

static void on_http_close(uv_handle_t *stream)
{ }

static void on_ws_close(uv_handle_t *stream)
{
    printf("bye!!\n");
    /* free(ws->parser.payload.base); */
}

static void on_http_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void on_http_connect(uv_connect_t *req, int status)
{
    int r;
    uv_buf_t buf;

    printf("ws: http: connect: %d %s\n", status, uv_strerror(status));
    assert(status == 0);

    buf.base = malloc(147 + 256);
    buf.len = sprintf(buf.base,
        "GET /jsonrpc HTTP/1.1\r\n"
        "Host:%s:%u\r\n"
        "Upgrade:websocket\r\n"
        "Connection:Upgrade\r\n"
        "Sec-WebSocket-Key:AQIDBAUGBwgJCgsMDQ4PEC==\r\n"
        "Sec-WebSocket-Version:13\r\n"
        "\r\n",
        rpc_host, rpc_port);
    r = uv_write(&u.req.http_write_req, tcp_stream, &buf, 1, on_http_wrote);
    assert(r == 0);
}

static void on_http_wrote(uv_write_t *_req, int status) {
    assert(status == 0);

    printf("http request sent\n");
    u.http_resp.nread = 0;
    u.http_resp.ncrlfcrlf = 0;
    uv_read_start(tcp_stream, alloc_cb, on_http_read);
}

static void on_http_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    printf("http read %ld %s\n", nread, uv_strerror(nread));
    if (nread > 0) {
        char *p;

        if (u.http_resp.nread < sizeof u.http_resp.status_line) {
            size_t need;
            printf("status read\n");
            need = sizeof u.http_resp.status_line - u.http_resp.nread;
            if (need > (size_t)nread)
                need = (size_t)nread;
            memcpy(u.http_resp.status_line + u.http_resp.nread,
                    buf->base,
                    need);
            u.http_resp.nread += need;
        }
        if (u.http_resp.nread == sizeof u.http_resp.status_line) {
            printf("status ln ok\n");
            if (0 != memcmp(u.http_resp.status_line, HTTP_SWITCHING_PROTOCOLS, sizeof HTTP_SWITCHING_PROTOCOLS - 1))
                assert(!"Couldn't switch protocols");
        }

        for (p = buf->base;;++p) {
            switch (u.http_resp.ncrlfcrlf) {
            case 0:
                if (NULL != (p = memchr(p, '\r', nread - (p - buf->base)))) {
                    u.http_resp.ncrlfcrlf = 1;
                } else
                    goto out_loop;
                break;
            case 4: {
                uv_buf_t wsbuf;

                on_ws_connect();

                wsbuf.base = &buf->base[nread];
                wsbuf.len = wsbuf.base - p;
                if (wsbuf.len > 0)
                    on_ws_read(tcp_stream, wsbuf.len, &wsbuf);
                goto out_loop;
            }
            default:

                if (p < buf->base + nread) {
                    if (*p == "\r\n\r\n"[u.http_resp.ncrlfcrlf])
                        ++u.http_resp.ncrlfcrlf;
                    else
                        u.http_resp.ncrlfcrlf = 0;
                } else
                    goto out_loop;

                break;
            }
        }
    out_loop:;

    } else if (nread < 0) {
        /* EOF or error. */
        uv_close((uv_handle_t *)stream, on_http_close);
        printf("http close\n");
    }
}


static void on_ws_connect(void)
{
    printf("ws: ws: connect\n");
    uv_read_stop(tcp_stream);
    uv_read_start(tcp_stream, alloc_cb, on_ws_read);

    on_rpc_connect();
}

int rpc_shutdown(void) {
    uv_buf_t buf;

    buf.len = 2;
    buf.base = "\x80\x8a";

    return uv_write(&u.req.ws_close_write_req, tcp_stream, &buf, 1, on_ws_close_wrote);
}

static void on_ws_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    if (nread > 0) {
        char *buffer;
        size_t remain, need;
        int need_more;

        printf("ws: read: (%u)%*s<<<<<<<<<\n", (unsigned)nread, (unsigned)nread, buf->base);

        buffer = buf->base, remain = nread;
        do {
            printf("remain=%d; hdrlen=%d\n", (int)nread, hdrlen);
            if (hdrlen != sizeof hdrbuf) {
                if (hdrlen >= 2) {
            parse_hdr:
                    printf("header: %x%x\n", hdrbuf[0], hdrbuf[1]);
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
                printf("new hdrlen=%d need=%d need_more=%d remain=%d\n", hdrlen, (int)need, need_more, (int)remain);
                memcpy(&hdrbuf[hdrlen], buffer, need);
                buffer += need, remain -= need;

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

                    printf("frame hdr parsed; hdrlen=%d paylen=%d %x %x\n", hdrlen, (int)payloadlen, hdrbuf[0], hdrbuf[1]);
                    if (hdrbuf[1] & 0x80)
                        assert(!"mask not expected");

                    assert(msgsize == msglen);
                    msgsize = msglen + payloadlen;
                    if (msgsize > msgallocsize) {
                        msgbuf = realloc(msgbuf, (msgallocsize = msgsize));
                        if (NULL == msgbuf) {
                            rpc_shutdown();
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
                printf("msg len=%d size=%d; readable=%d\n", (int)msglen, (int)msgsize, (int)remain);
                if ((need_more = remain < need))
                    need = remain;
                printf("read=+%d\n", (int)need);

                memcpy(msgbuf + msglen, buffer, need);
                msglen += need, buffer += need, remain -= need;

                if (!need_more) {
                printf("msg !!! %*s \n", (int)msglen, msgbuf);

                    switch (hdrbuf[0] & 0x7f) {
                    case 0x1:
                        on_rpc_message(msgbuf, msglen);
                        break;
                    case 0xa:
                        rpc_shutdown();
                        break;
                    default:
                        printf("type=%x\n", hdrbuf[0]);
                        assert(0);
                    }
                    msgsize = 0;
                    msglen = 0;
                    hdrlen = 0;
                }

            }
        } while (remain > 0);
    } else if (nread < 0) {
        /* EOF or error. */
    printf("!! close\n");
        uv_close((uv_handle_t *)stream, on_ws_close);
    }
    printf("<< finished chunk >>\n");
}
