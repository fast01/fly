#include "fy_connection.h"
#include "fy_event.h"
#include "fy_logger.h"
#include "fy_time.h"
#include "fy_util.h"
#include "fy_conf.h"

#include "jc_type.h"

#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <assert.h>

extern fy_pool_t  *fy_mem_pool;

extern fy_task *null_task_list[];
extern fy_task  null_task;

static int fy_adidx_task_submit(fy_task *, void *);
static int fy_adidx_module_init(fy_module *, void *);
static int fy_adidx_module_conf(fy_module *, void *);

static int fy_adidx_conn_handler(fy_event *ev, void *request);
static int fy_adidx_read_handler(fy_event *ev, void *request);
static int fy_adidx_write_handler(fy_event *ev, void *request);

typedef struct {
    int32_t magic;
    int32_t size;
    char    data[];
} fy_adidx_hdr;

static fy_task fy_adidx_task = {
    FY_TASK_INIT("fy_adidx_task", null_task_list, fy_adidx_task_submit)
};

static fy_task *fy_adidx_tasks[] = {
    &fy_adidx_task,
    &null_task,
};

fy_module fy_api_adidx_module = {
    FY_MODULE_INIT("fy_api_adidx_module",
            fy_adidx_tasks,
            fy_adidx_module_init,
            fy_adidx_module_conf)
};

static fy_event_loop *fy_adidx_event_loop;

static fy_conn_pool *fy_adidx_conn_pool;
static int fy_adidx_conn_pool_size;
static const char *fy_adidx_server_addr;
static int fy_adidx_server_port;

static const char *fy_api_adidx_uri;

static int fy_adidx_module_conf(fy_module *m, void *ptr)
{
    fy_adidx_conn_pool_size = fy_atoi(fy_conf_get_param("adidx_conn_pool_size"));
    fy_adidx_server_addr = fy_conf_get_param("adidx_server_addr");
    fy_adidx_server_port = fy_atoi(fy_conf_get_param("adidx_server_port"));
    fy_api_adidx_uri = fy_conf_get_param("adidx_api_uri");
    if (fy_adidx_conn_pool_size == 0
            || fy_adidx_server_addr == NULL
            || fy_adidx_server_port == 0)
    {
        fy_log_error("adidx conf error\n");
        return -1;
    }
    return 0;
}

static int fy_adidx_module_init(fy_module *module, void *ev_loop)
{
    int             i;
    fy_connection  *conn;

    fy_adidx_event_loop = (fy_event_loop *)ev_loop;
    fy_adidx_conn_pool = fy_create_conn_pool(fy_mem_pool,
            fy_adidx_conn_pool_size, 1);

    if (fy_adidx_conn_pool == NULL) {
        fy_log_error("fy_adidx_module create conn pool error\n");
        return -1;
    }

    for (i = 0; i != fy_adidx_conn_pool_size; ++i) {
        conn = fy_pop_connection(fy_adidx_conn_pool);
        if (fy_create_nonblocking_conn(conn, 
                    fy_adidx_server_addr, fy_adidx_server_port) == -1)
        {
            fy_log_error("fy_adidx_module: fy_create_nonblocking_conn errno: %d\n", errno);
            return -1;
        }
        conn->revent->handler = fy_adidx_read_handler;
        conn->wevent->handler = fy_adidx_conn_handler;
        if (fy_event_add(conn, fy_adidx_event_loop, FY_EVOUT) == -1) {
            fy_log_error("fy_adidx_module: fy_event_add errno: %d\n", errno);
            return -1;
        }
    }

    return 0;
}

static int fy_adidx_conn_handler(fy_event *ev, void *request)
{
    fy_connection *conn;

    assert(request == NULL);

    conn = ev->conn;
    conn->revent->handler = fy_adidx_read_handler;
    conn->wevent->handler = NULL;

    fy_push_connection(fy_adidx_conn_pool, conn);
    fy_event_mod(conn, fy_adidx_event_loop, FY_EVIN);

    return 0;
}

static int fy_adidx_read_handler(fy_event *ev, void *request)
{
    ssize_t         s_size, recved;
    jc_json_t      *jsdata;
    fy_request     *r;
    fy_connection  *c;
    fy_adidx_hdr   *rephdr;

    r = (fy_request *)request;
    c = ev->conn;

    if (r == NULL) {
        if (c->fd != -1) {
            close(c->fd);
            c->fd = -1;
        }
        fy_push_err_conn(fy_adidx_conn_pool, c);
        fy_repair_conn_pool(fy_adidx_conn_pool, fy_adidx_event_loop,
                fy_adidx_conn_handler, fy_adidx_read_handler);
        return -1;
    }

    assert(r->info != NULL);

    while (1) {
        switch (r->info->recv_state) {
            case 0: /* state 0: alloc memory for magic and size */
                r->info->recv_buf = fy_pool_alloc(r->pool, sizeof(fy_adidx_hdr));
                if (r->info->recv_buf == NULL) {
                    goto error;
                }
                r->info->recv_buf_rpos = r->info->recv_buf_wpos = r->info->recv_buf;
                r->info->recv_state = 1;
                break;

            case 1: /* state 1: recv magic and size */
                recved = r->info->recv_buf_wpos - r->info->recv_buf_rpos;
                assert(recved <= sizeof(fy_adidx_hdr));
                if (recved == sizeof(fy_adidx_hdr)) {
                    r->info->recv_state = 2;
                    break;
                }
                s_size = sizeof(fy_adidx_hdr) - recved;
                s_size = recv(c->fd, r->info->recv_buf_wpos, s_size, 0);
                if (s_size == 0) {
                    goto error;
                }
                if (s_size == -1) {
                    if (errno == EAGAIN) {
                        fy_event_mod(c, fy_adidx_event_loop, FY_EVIN);
                        return 0;
                    }
                    goto error;
                }
                r->info->recv_buf_wpos += s_size;
                break;

            case 2: /* state 2: alloc memory for adidx data */
                rephdr = (fy_adidx_hdr *)r->info->recv_buf_rpos;
                if (rephdr->magic != 0xBEEF) {
                    fy_log_error("fy_adidx_read_handler read magic error: 0x%x\n", rephdr->magic);
                    goto error;
                }
                r->info->recv_buf = fy_pool_alloc(r->pool, rephdr->size + sizeof(fy_adidx_hdr));
                if (r->info->recv_buf == NULL) {
                    goto error;
                }
                r->info->recv_buf_wpos = fy_cpymem(r->info->recv_buf, r->info->recv_buf_rpos, sizeof(fy_adidx_hdr));
                r->info->recv_buf_rpos = r->info->recv_buf;
                r->info->recv_state = 3;
                break;

            case 3: /* state 3: recv rephdr data */
                rephdr = (fy_adidx_hdr *)r->info->recv_buf_rpos;
                recved = r->info->recv_buf_wpos - r->info->recv_buf_rpos - sizeof(fy_adidx_hdr);
                assert(recved <= rephdr->size);
                if (recved == rephdr->size) {
                    r->info->recv_state = 4;
                    break;
                }
                s_size = rephdr->size - recved;
                s_size = recv(c->fd, r->info->recv_buf_wpos, s_size, 0);
                if (s_size == 0) {
                    goto error;
                }
                if (s_size == -1) {
                    if (errno == EAGAIN) {
                        fy_event_mod(c, fy_adidx_event_loop, FY_EVIN);
                        return 0;
                    }
                    goto error;
                }
                r->info->recv_buf_wpos += s_size;
                break;

            case 4: /* state 4: accept */
                rephdr = (fy_adidx_hdr *)r->info->recv_buf_rpos;
                jsdata = jc_json_parse(rephdr->data);
                if (jsdata == NULL) {
                    fy_log_error("parse json response error, rep str: %s\n", rephdr->data);
                    goto error;
                }

                fy_push_connection(fy_adidx_conn_pool, c);

                assert(r->info->json_rc == NULL);

                jc_json_add_str(jsdata, "errormsg", "ok");
                jc_json_add_num(jsdata, "errorno", 0);
                jc_json_add_num(jsdata, "expiredtime", fy_cur_sec() + 60);

                r->info->json_rc = jsdata;
                fy_request_next_module(r);
                return 0;
        }
    }
    return 0;

error:
    if (c->fd != -1) {
        close(c->fd);
        c->fd = -1;
    }
    fy_push_err_conn(fy_adidx_conn_pool, c);
    fy_request_next_module(r);
    return -1;
}

static int fy_adidx_write_ready(void *request)
{
#define set_param(param, key) do { \
    if ((val = fy_fcgi_get_param(param, r)) != NULL) { \
        pw += snprintf(pw, pe - pw, "%s=%s&", key, val); \
        if (pw >= pe) { \
            goto error; \
        } \
    } \
} while (0)

    char           params[4096];
    char          *val, *pw, *pe;
    size_t         n;
    fy_request    *r;
    fy_adidx_hdr   reqhdr;

    assert(request != NULL);

    r = (fy_request *)request;

    pw = &params[0];
    pe = &params[4096];

    set_param("ZONE", "zone");
    set_param("REGION", "region");
    set_param("POSQUERY", "query");

    n = pw - &params[0];
    
    if (n == 0) {
        // no params
        fy_log_error("index no condition\n");
        goto error;
    }

    if ((r->info->send_buf = fy_pool_alloc(r->pool, n)) == NULL) {
        return -1;
    }
    r->info->send_buf_rpos = r->info->send_buf_wpos = r->info->send_buf;
    r->info->send_buf_end = r->info->send_buf + n;

    reqhdr.magic = 0xDEAD;
    reqhdr.size = n;

    r->info->send_buf_wpos = fy_cpymem(r->info->send_buf_wpos, &reqhdr, sizeof(reqhdr));
    r->info->send_buf_wpos = fy_cpymem(r->info->send_buf_wpos, params, n);

    return 0;

error:
    fy_log_error("index condition too much\n");
    return -1;

#undef set_param
}

static int fy_adidx_write_handler(fy_event *ev, void *request)
{
    ssize_t         s_size;
    fy_request     *r;
    fy_connection  *c;

    r = (fy_request *)request;
    c = ev->conn;

    s_size = send(c->fd, r->info->send_buf_rpos,
            r->info->send_buf_wpos - r->info->send_buf_rpos, 0);

    if (s_size == -1) {
        if (errno != EAGAIN) {
            close(c->fd);
            c->fd = -1;
            fy_push_err_conn(fy_adidx_conn_pool, c);
            fy_request_next_module(r);
        } else {
            fy_event_mod(ev->conn, fy_adidx_event_loop, FY_EVOUT);
        }
        return -1;
    }

    if (s_size != r->info->send_buf_wpos - r->info->send_buf_rpos) {
        r->info->send_buf_wpos += s_size;
        fy_event_mod(ev->conn, fy_adidx_event_loop, FY_EVOUT);
        return 0;
    }

    fy_event_mod(ev->conn, fy_adidx_event_loop, FY_EVIN);
    return 0;
}

static int fy_adidx_task_submit(fy_task *task, void *request)
{
    int             i, api_version;
    const char     *doc_uri;
    fy_request     *r;
    fy_connection  *conn;

    assert(request != NULL);

    r = (fy_request *)request;

    fy_repair_conn_pool(fy_adidx_conn_pool, fy_adidx_event_loop,
            fy_adidx_conn_handler, fy_adidx_read_handler);

    doc_uri = fy_fcgi_get_param("DOCUMENT_URI", r);
    if (doc_uri == NULL || strcmp(doc_uri, fy_api_adidx_uri) != 0) {
        fy_request_next_module(r);
        return 0;
    }

    if ((conn = fy_pop_connection(fy_adidx_conn_pool)) == NULL) {
        fy_request_next_module(r);
        return -1;
    }

    if (fy_adidx_write_ready(r) == -1) {
        fy_request_next_module(r);
        return -1;
    }

    conn->request = r;
    conn->revent->handler = fy_adidx_read_handler;
    conn->wevent->handler = fy_adidx_write_handler;

    fy_event_mod(conn, fy_adidx_event_loop, FY_EVOUT);
    return 0;
}
