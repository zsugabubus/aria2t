#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <ncurses.h>
#include <uv.h>
#include "rpc.h"
#include "jeezson/jeezson.h"

uv_loop_t *loop;

static struct json_node *nodes;
static size_t nnodes;
void shellout() {
    /* def_prog_mode_sp(client->scr);
    endwin();
    (shellprg = getenv("SHELL")) || (shellprg = "sh");
    system(shellprg);
    refresh(); */
}

void on_rpc_message(char *msg, size_t msglen){
    /* printf("goooot: %s\n", msg); */

    json_parse(msg, &nodes, &nnodes);
    json_debug(nodes, 0);
    /* json_debug(json_get(json_get(nodes, "result"), "version"), 0); */
}

void on_rpc_connect(void) {

    struct json_writer jw[1];
    json_writer_init(jw);
    json_write_beginobj(jw);

    json_write_key(jw, "jsonrpc");
    json_write_str(jw, "2.0");

    json_write_key(jw, "id");
    json_write_int(jw, 0);

    json_write_key(jw, "method");
    json_write_str(jw, "aria2.getGlobalStat");

    /* json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellActive");
    json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellWaiting");
    json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellStopped");
    json_write_key(jw, "params");
    json_write_beginarr(jw);
    json_write_str(jw, "token:secrat");
    json_write_endarr(jw); */

    json_write_key(jw, "params");
    json_write_beginarr(jw);
    json_write_str(jw, "token:secrat");
    json_write_endarr(jw);

    json_write_endobj(jw);

    rpc_write(jw->buf, jw->len);
    json_writer_term(jw);
    printf("> %s\n", jw->buf);

    jw->buf = NULL;
    json_writer_free(jw);
}

void periodic(uv_timer_t* handle) {
    struct json_writer jw[1];
    json_writer_init(jw);
    json_write_beginobj(jw);

    json_write_key(jw, "jsonrpc");
    json_write_str(jw, "2.0");

    json_write_key(jw, "id");
    json_write_int(jw, 0);

    json_write_key(jw, "method");
    json_write_str(jw, "aria2.getGlobalStat");

    /* json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellActive");
    json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellWaiting");
    json_write_key(jw, "methodName");
    json_write_str(jw, "aria2.tellStopped");
    json_write_key(jw, "params");
    json_write_beginarr(jw);
    json_write_str(jw, "token:secrat");
    json_write_endarr(jw); */

    json_write_key(jw, "params");
    json_write_beginarr(jw);
    json_write_str(jw, "token:secrat");
    json_write_endarr(jw);

    json_write_endobj(jw);

    rpc_write(jw->buf, jw->len);
    json_writer_term(jw);
    printf("> %s\n", jw->buf);

    jw->buf = NULL;
    json_writer_free(jw);
}

int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    int r;

    uv_timer_t timer_req;
    uv_tty_t tty_req;

    (void)initscr();
    /* No input buffering. */
    (void)raw();
    /* No input echo. */
    (void)noecho();
    /* Do not translate '\n's. */
    (void)nonl();
    /* We are async. */
    /* (void)nodelay(cp->win, TRUE); */
    /* Catch special keys. */
    /* (void)keypad(cp->win, TRUE); */
    /* 8-bit inputs. */
    /* (void)meta(cp->win, TRUE); */
    clear();
    /* mvaddch(6, 5, '0'); */

    loop = uv_default_loop();
    assert(loop != NULL);

    uv_tty_init(loop, &tty_req, 0);
    uv_tty_set_mode(&tty, UV_TTY_MODE_NORMAL);

    uv_timer_init(loop, &timer_req);
    uv_timer_start(&timer_req, periodic, 2000, 1000);

    r = rpc_init();
    assert(r == 0);

    rpc_host = "127.0.0.1";
    rpc_port = 6801;

    r = rpc_connect();
    assert(r == 0);

    r = uv_run(loop, UV_RUN_DEFAULT);
    assert(r == 0);

    r = uv_loop_close(loop);
    printf("end %s\n", uv_strerror(r));
    assert(!r);

    endwin();

    return EXIT_SUCCESS;
}
