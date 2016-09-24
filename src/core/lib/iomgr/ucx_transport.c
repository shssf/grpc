/*
* Copyright (C) Mellanox Technologies Ltd. 2016.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>

#include <grpc/support/log.h>
#include <grpc/support/alloc.h>
#include "grpc/support/string_util.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/iomgr/network_status_tracker.h"
#include "src/core/lib/iomgr/ucx_transport.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/ucx_timers.h"
#include "src/core/lib/support/string.h"

#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

typedef struct grpc_ucx_t {
    grpc_endpoint     base;
    grpc_fd          *em_fd;
    bool              finished_edge;
    size_t            slice_size;
    gpr_slice_buffer *incoming_buffer;
    grpc_closure     *read_cb;
    grpc_closure      read_closure;
    char             *peer_string;
} grpc_ucx;

typedef struct ucx_request_t {
    int completed;
} ucx_request;

int                   grpc_ucx_trace      = 0;    /* debug trace print control */
static ucp_context_h  ucx_context         = NULL; /* UCX library common context */

static int            ucx_fd_local        = 0;
static ucp_ep_h       ucx_ep              = NULL;
static ucp_worker_h   ucx_worker          = NULL;

static void send_handle(void *request, ucs_status_t status)
{
    ucx_request *req = (ucx_request *) request;
    req->completed = 1;
}

static void recv_handle(void *request, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    ucx_request *context = (ucx_request *) request;
    context->completed = 1;
}

static void request_init(void *request)
{
    ucx_request *ctx = (ucx_request *) request;
    ctx->completed = 0;
}

static void ucx_wait(ucp_worker_h ucp_worker, ucx_request *context)
{
    //UCX_TIMER_START(UCXTL_EPOLL_WAIT);
    while (context->completed == 0) {
        ucp_worker_progress(ucp_worker);
    }
    //UCX_TIMER_END(UCXTL_EPOLL_WAIT);
}

static void ucx_send_msg(void *buf, size_t len)
{
    ucx_request *request = 0;

    GPR_ASSERT(NULL != ucx_ep);
    UCX_TIMER_START(UCXTL_UCX);
    request = ucp_tag_send_nb(ucx_ep, buf, len, ucp_dt_make_contig(1), 1, send_handle);
    if (UCS_PTR_IS_ERR(request)) {
        gpr_log(GPR_DEBUG, "UCX ucx_send_msg unable to send message len=%lu", len);
        return;
    } else if (UCS_PTR_STATUS(request) != UCS_OK) {
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_send_msg message send postponed with request=%p", request);
        }
        ucx_wait(ucx_worker, request);
        ucp_request_release(request);
    }
    UCX_TIMER_END(UCXTL_UCX);
}

static size_t ucx_recv_msg(void *buf, size_t len)
{
    ucp_tag_message_h msg_tag;
    ucp_tag_recv_info_t info_tag;
    ucx_request *request = 0;

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_recv_msg buf=%p len=%lu", buf, len);
    }

    GPR_ASSERT(NULL != ucx_worker);

    UCX_TIMER_START(UCXTL_EPOLL_WAIT);
    do {
        /* if no message here -> do blocking receive */
        ucp_worker_progress(ucx_worker);
        msg_tag = ucp_tag_probe_nb(ucx_worker, 1, (ucp_tag_t)-1, 1, &info_tag);
    } while (msg_tag == NULL);
    UCX_TIMER_END(UCXTL_EPOLL_WAIT);

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_recv_msg TAG=%lu length=%lu len=%lu", info_tag.sender_tag, info_tag.length, len);
    }
    GPR_ASSERT(info_tag.length <= len);

    UCX_TIMER_START(UCXTL_UCX);

    request = ucp_tag_msg_recv_nb(ucx_worker, buf, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag, recv_handle);
    if (UCS_PTR_IS_ERR(request)) {
        gpr_log(GPR_DEBUG, "UCX ucx_recv_msg unable to receive UCX data message (%u)", UCS_PTR_STATUS(request));
        return 0;
    } else {
        ucx_wait(ucx_worker, request);
        ucp_request_release(request);
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_recv_msg data message was received after WAIT");
        }
    }
    UCX_TIMER_END(UCXTL_UCX);
    return info_tag.length;
}

static void ucx_internal_read(grpc_exec_ctx *exec_ctx, grpc_ucx *ucx)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_internal_read slice_len=%lu buf_len=%lu",
                GPR_SLICE_LENGTH(ucx->incoming_buffer->slices[0]),
                ucx->incoming_buffer->length);
    }

    ucp_tag_message_h msg_tag;
    ucp_tag_recv_info_t info_tag;

    GPR_ASSERT(NULL != ucx_ep);
    GPR_ASSERT(0 == ucx->incoming_buffer->length);

    UCX_TIMER_START(UCXTL_EPOLL_WAIT);
    ucp_worker_progress(ucx_worker);
    msg_tag = ucp_tag_probe_nb(ucx_worker, 1, (ucp_tag_t)-1, 0, &info_tag);
    UCX_TIMER_END(UCXTL_EPOLL_WAIT);
    if (NULL == msg_tag) {
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_internal_read -> nothing to receive -> grpc_fd_notify_on_read");
        }
        grpc_fd_notify_on_read(exec_ctx, ucx->em_fd, &ucx->read_closure);
        UCX_TIMER_END(UCXTL_ENDPOINT);
        return;
    }

    if (0 == info_tag.length) { /* 0 read size ==> end of stream */
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_internal_read -> end of stream -> grpc_exec_ctx_sched");
        }
        gpr_slice_buffer_reset_and_unref(ucx->incoming_buffer);
        grpc_closure *cb = ucx->read_cb;
        ucx->read_cb = NULL;
        ucx->incoming_buffer = NULL;
        grpc_error *err = GRPC_ERROR_CREATE("EOF");
        grpc_exec_ctx_sched(exec_ctx, cb, err, NULL);
        //TCP_UNREF(exec_ctx, tcp, "read");
        UCX_TIMER_END(UCXTL_ENDPOINT);
        return;
    }

    /* Receive slice by slice */
    size_t recv_slices_num = 0, ucx_bytes_read = 0;

    uint64_t ucx_timer1 = timer_nano();
    ucx_bytes_read = ucx_recv_msg(&recv_slices_num, sizeof(recv_slices_num));
    uint64_t ucx_timer2 = timer_nano() - ucx_timer1;
    ucx_timer[UCXTL_ENDPOINT] -= ucx_timer2;


    for (size_t i = 0; i < recv_slices_num; ++i) {
        gpr_slice_buffer_add(ucx->incoming_buffer, gpr_slice_malloc(ucx->slice_size));

        void *ptr = GPR_SLICE_START_PTR(ucx->incoming_buffer->slices[i]);
        size_t ptr_len = GPR_SLICE_LENGTH(ucx->incoming_buffer->slices[i]);

        uint64_t ucx_timer12 = timer_nano();
        size_t ucx_bytes_read_local = ucx_recv_msg(ptr, ptr_len);
        uint64_t ucx_timer22 = timer_nano() - ucx_timer12;
        ucx_timer[UCXTL_ENDPOINT] -= ucx_timer22;

        ucx->incoming_buffer->slices[i].data.refcounted.length = ucx_bytes_read_local;

        ucx_bytes_read += ucx_bytes_read_local;
    }
    ucx->incoming_buffer->length = ucx_bytes_read;

    if (grpc_ucx_trace) {
        if (1 < grpc_ucx_trace) {
            for (size_t i = 0; i < ucx->incoming_buffer->count; i++) {
                char *data = gpr_dump_slice(ucx->incoming_buffer->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
                gpr_log(GPR_DEBUG, "UCX READ(%lu) slice_len=%lu %s", ucx_bytes_read, GPR_SLICE_LENGTH(ucx->incoming_buffer->slices[i]), data);
                gpr_free(data);
            }
        }
        gpr_log(GPR_DEBUG, "UCX ucx_ib_read len=%lu", ucx_bytes_read);
    }

    grpc_closure *cb = ucx->read_cb;
    ucx->read_cb = NULL;
    ucx->incoming_buffer = NULL;
    grpc_error *error = GRPC_ERROR_NONE;
    grpc_exec_ctx_sched(exec_ctx, cb, error, NULL);
    UCX_TIMER_END(UCXTL_ENDPOINT);
}

static void ucx_read(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep, gpr_slice_buffer *incoming_buffer, grpc_closure *cb)
{
    grpc_ucx *ucx = (grpc_ucx *)ep;
    UCX_TIMER_START(UCXTL_ENDPOINT);
    GPR_ASSERT(ucx->read_cb == NULL);
    ucx->read_cb = cb;
    ucx->incoming_buffer = incoming_buffer;
    //TCP_REF(ucx, "read");
    if (ucx->finished_edge) {
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_read -> grpc_fd_notify_on_read");
        }
        ucx->finished_edge = false;
        grpc_fd_notify_on_read(exec_ctx, ucx->em_fd, &ucx->read_closure);
    } else {
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX ucx_read -> grpc_exec_ctx_sched");
        }
        grpc_exec_ctx_sched(exec_ctx, &ucx->read_closure, GRPC_ERROR_NONE, NULL);
    }
    UCX_TIMER_END(UCXTL_ENDPOINT);
}

static void ucx_handle_read(grpc_exec_ctx *exec_ctx, void *arg /* grpc_ucx */, grpc_error *error)
{
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_handle_read");
    }
    grpc_ucx *ucx = (grpc_ucx *)arg;
    GPR_ASSERT(!ucx->finished_edge);

    if (error != GRPC_ERROR_NONE) {
        gpr_slice_buffer_reset_and_unref(ucx->incoming_buffer);
        grpc_closure *cb = ucx->read_cb;
        ucx->read_cb = NULL;
        ucx->incoming_buffer = NULL;
        grpc_error *err = GRPC_ERROR_REF(error);
        grpc_exec_ctx_sched(exec_ctx, cb, err, NULL);
        //TCP_UNREF(exec_ctx, ucx, "read");
    } else {
        ucx_internal_read(exec_ctx, ucx);
    }
}

static void ucx_write(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep, gpr_slice_buffer *buf, grpc_closure *cb)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (1 < grpc_ucx_trace) {
        for (size_t i = 0; i < buf->count; i++) {
            char *data = gpr_dump_slice(buf->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
            gpr_log(GPR_DEBUG, "UCX WRITE(%lu) %p (peer=%s): %s", GPR_SLICE_LENGTH(buf->slices[i]), ucx, ucx->peer_string, data);
            gpr_free(data);
        }
    }

    GPR_TIMER_BEGIN("ucx_write", 0);

    /* Send slice by slice */
    uint64_t ucx_timer21 = timer_nano();
    ucx_send_msg(&buf->count, sizeof(buf->count));
    uint64_t ucx_timer212 = timer_nano() - ucx_timer21;
    ucx_timer[UCXTL_ENDPOINT] -= ucx_timer212;

    for (size_t i = 0; i < buf->count; i++) {
        void *ptr = GPR_SLICE_START_PTR(buf->slices[i]);
        size_t ptr_len = GPR_SLICE_LENGTH(buf->slices[i]);
        uint64_t ucx_timer1 = timer_nano();
        ucx_send_msg(ptr, ptr_len);
        uint64_t ucx_timer2 = timer_nano() - ucx_timer1;
        ucx_timer[UCXTL_ENDPOINT] -= ucx_timer2;
    }

    grpc_error *error = GRPC_ERROR_NONE;
    grpc_exec_ctx_sched(exec_ctx, cb, error, NULL);

    GPR_TIMER_END("ucx_write", 0);

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_wrote total bytes=%lu", buf->length);
    }
    UCX_TIMER_END(UCXTL_ENDPOINT);
}

static grpc_workqueue *ucx_get_workqueue(grpc_endpoint *ep)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_get_workqueue");
    }
    grpc_workqueue *tmp = grpc_fd_get_workqueue(ucx->em_fd);
    UCX_TIMER_END(UCXTL_ENDPOINT);
    return tmp;
}

static void ucx_add_to_pollset(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                               grpc_pollset *pollset)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_add_to_pollset fd=%d", grpc_fd_wrapped_fd(ucx->em_fd));
    }
    grpc_pollset_add_fd(exec_ctx, pollset, ucx->em_fd);
    UCX_TIMER_END(UCXTL_ENDPOINT);
}

static void ucx_add_to_pollset_set(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                   grpc_pollset_set *pollset_set)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_add_to_pollset_set fd=%d", grpc_fd_wrapped_fd(ucx->em_fd));
    }
    grpc_pollset_set_add_fd(exec_ctx, pollset_set, ucx->em_fd);
    UCX_TIMER_END(UCXTL_ENDPOINT);
}

static void ucx_shutdown(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep)
{
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_shutdown EP=%p", ep);
    }
    grpc_ucx *ucx = (grpc_ucx *)ep;
    grpc_fd_shutdown(exec_ctx, ucx->em_fd);
}

static void ucx_destroy(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep)
{
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_destroy EP=%p", ep);
    }
    grpc_network_status_unregister_endpoint(ep);
    gpr_free(ucx->peer_string);
    gpr_free(ucx);

    //TODO hangs ucp_worker_destroy(ucx_worker);
    ucp_cleanup(ucx_context);
}

static char *ucx_get_peer(grpc_endpoint *ep)
{
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)ep;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_get_peer: %s", ucx->peer_string);
    }
    char *tmp = gpr_strdup(ucx->peer_string);
    UCX_TIMER_END(UCXTL_ENDPOINT);
    return tmp;
}

static const grpc_endpoint_vtable vtable = {ucx_read,
                                            ucx_write,
                                            ucx_get_workqueue,
                                            ucx_add_to_pollset,
                                            ucx_add_to_pollset_set,
                                            ucx_shutdown,
                                            ucx_destroy,
                                            ucx_get_peer};

grpc_endpoint *grpc_ucx_create(grpc_fd *em_fd, size_t slice_size, const char *peer_string)
{
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX grpc_ucx_create fd=%d grpc_fd=%p slice_size=%lu peer=%s", grpc_fd_wrapped_fd(em_fd), em_fd, slice_size, peer_string);
    }
    UCX_TIMER_START(UCXTL_ENDPOINT);
    grpc_ucx *ucx = (grpc_ucx *)gpr_malloc(sizeof(grpc_ucx));
    ucx->base.vtable         = &vtable;
    ucx->peer_string         = gpr_strdup(peer_string);
    ucx->read_cb             = NULL;
    ucx->slice_size          = slice_size;
    ucx->finished_edge       = true;
    ucx->em_fd               = em_fd;
    ucx->read_closure.cb     = ucx_handle_read;
    ucx->read_closure.cb_arg = ucx;

  /* Tell network status tracker about new endpoint */
  grpc_network_status_register_endpoint(&ucx->base);
  UCX_TIMER_END(UCXTL_ENDPOINT);
  return &ucx->base;
}

static int ucx_fd()
{
    int epoll_fd = 0;
    ucs_status_t status;

    GPR_ASSERT(NULL != ucx_ep);
    UCX_TIMER_START(UCXTL_UCX);
    status = ucp_worker_get_efd(ucx_worker, &epoll_fd);
    UCX_TIMER_END(UCXTL_UCX);
    GPR_ASSERT(UCS_OK == status);

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_fd returned fd=%d", epoll_fd);
    }
    return epoll_fd;
}

static void ucx_init()
{
    ucp_params_t ucp_params;
    ucp_config_t *config;
    ucs_status_t status;

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_init");
    }

    if (NULL != ucx_context) {
        return;
    }
    GPR_ASSERT(NULL == ucx_worker);
    GPR_ASSERT(NULL == ucx_ep);

    //UCX_TIMER_START(UCXTL_UCX);

    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucp_config_read failed");
        return;
    }

    ucp_params.features = (UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP);
    ucp_params.request_size    = sizeof(ucx_request);
    ucp_params.request_init    = request_init;
    ucp_params.request_cleanup = NULL;

    status = ucp_init(&ucp_params, config, &ucx_context);

    //ucp_config_print(config, stdout, "ucp_config_print", UCS_CONFIG_PRINT_CONFIG);

    ucp_config_release(config);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucp_init failed");
        return;
    }
    //UCX_TIMER_END(UCXTL_UCX);
}

static void wait_fd(int epoll_fd)
{
    int ret = -1;
    //ucs_status_t status;
    int epoll_fd_local = 0;
    struct epoll_event ev;
    ev.data.u64 = 0;
    ev.data.u32 = 0;
    ev.data.ptr = 0;

    /* It is recommended to copy original fd */
    epoll_fd_local = epoll_create(1);

    ev.data.fd = epoll_fd;
    ev.events = EPOLLIN;
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX wait_fd add fd=%d to epoll_fd=%d with ptr=%p", epoll_fd, epoll_fd_local, ev.data.ptr);
    }
    if (epoll_ctl(epoll_fd_local, EPOLL_CTL_ADD, epoll_fd, &ev) < 0) {
        gpr_log(GPR_DEBUG, "UCX Couldn't add original socket %d to the new epoll: %m", epoll_fd);
        return;
    }
    do {
        ret = epoll_wait(epoll_fd_local, &ev, 1, -1);
    } while ((ret == -1) && (errno == EINTR));

    close(epoll_fd_local);
}

#define UCX_SOCK_SEND( _fd_, _data_, _size_, _msg_)               \
    do {                                                          \
        errno = 0;                                                \
        ssize_t ret = send(_fd_, _data_, _size_, 0);              \
        if (ret < 0 || ret != (int) _size_) {                     \
            gpr_log(GPR_DEBUG, "UCX failed to send " _msg_ " errno=%m"); \
            return;                                               \
        }                                                         \
    } while(0)

#define UCX_SOCK_RECV( _fd_, _data_, _size_, _msg_)               \
    do {                                                          \
        errno = 0;                                                \
        wait_fd(_fd_);                                            \
        ssize_t ret = recv(_fd_, _data_, _size_, 0);              \
        if (ret < 0 || ret != (int) _size_) {                     \
            gpr_log(GPR_DEBUG, "UCX failed to recv " _msg_ " errno=%m ret=%ld expected size=%ld", ret, _size_); \
            return;                                               \
        }                                                         \
    } while(0)

void ucx_connect(int tcp_fd, int is_server)
{
    size_t         ucx_worker_addr_len = 0;
    ucp_address_t *ucx_worker_addr     = NULL;
    size_t         ucx_peer_addr_len   = 0;
    ucp_address_t *ucx_peer_addr       = NULL;
    ucs_status_t   status;

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX connect fd=%d incoming errno(%d)=%m", tcp_fd, errno);
    }
    if (tcp_fd < 0 || (NULL != ucx_ep) || !GRPC_USE_UCX) {
        return;
    }

    if (!ucx_context) {
        ucx_init();
    }

    status = ucp_worker_create(ucx_context, UCS_THREAD_MODE_SINGLE, &ucx_worker);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucp_worker_create failed");
        return;
    }

    //a BUG? ucp_worker_proto_print(ucx_worker, stdout, "ucp_worker_proto_print", UCS_CONFIG_PRINT_CONFIG);

    status = ucp_worker_get_address(ucx_worker, &ucx_worker_addr, &ucx_worker_addr_len);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucp_worker_get_address failed");
        return;
    }
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX initialized with addr_len=%lu", ucx_worker_addr_len);
    }

    if (is_server) {
        UCX_SOCK_RECV(tcp_fd, &ucx_peer_addr_len, sizeof(ucx_peer_addr_len), "address length");
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX received address len=%lu", ucx_peer_addr_len);
        }

        ucx_peer_addr = malloc(ucx_peer_addr_len);
        if (!ucx_peer_addr) {
            gpr_log(GPR_DEBUG, "UCX failed memory allocation");
            return;
        }

        UCX_SOCK_RECV(tcp_fd, ucx_peer_addr, ucx_peer_addr_len, "address");

        UCX_SOCK_SEND(tcp_fd, &ucx_worker_addr_len, sizeof(ucx_worker_addr_len), "address length");
        UCX_SOCK_SEND(tcp_fd, ucx_worker_addr, ucx_worker_addr_len, "address");
    } else {
        UCX_SOCK_SEND(tcp_fd, &ucx_worker_addr_len, sizeof(ucx_worker_addr_len), "address length");
        UCX_SOCK_SEND(tcp_fd, ucx_worker_addr, ucx_worker_addr_len, "address");

        UCX_SOCK_RECV(tcp_fd, &ucx_peer_addr_len, sizeof(ucx_peer_addr_len), "address length");
        if (grpc_ucx_trace) {
            gpr_log(GPR_DEBUG, "UCX received address len=%lu", ucx_peer_addr_len);
        }

        ucx_peer_addr = malloc(ucx_peer_addr_len);
        if (!ucx_peer_addr) {
            gpr_log(GPR_DEBUG, "UCX failed memory allocation");
            return;
        }

        UCX_SOCK_RECV(tcp_fd, ucx_peer_addr, ucx_peer_addr_len, "address");
    }

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_connect addr=%p, addr_len=%lu, worker=%p, ep=%p", ucx_peer_addr, ucx_peer_addr_len, ucx_worker, ucx_ep);
    }
    status = ucp_ep_create(ucx_worker, ucx_peer_addr, &ucx_ep);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucp_ep_create failed with error: %s", ucs_status_string(status));
        return;
    }

    ucx_fd_local = ucx_fd();

    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX EP created FD=%d", ucx_fd_local);
    }

    free(ucx_peer_addr);
    ucp_worker_release_address(ucx_worker, ucx_worker_addr);
}

int ucx_get_fd()
{
    if (grpc_ucx_trace) {
        gpr_log(GPR_DEBUG, "UCX ucx_get_fd returned fd=%d", ucx_fd_local);
    }
    return ucx_fd_local;
}

void ucx_prepare_fd()
{
    ucs_status_t status;
    if (NULL == ucx_ep) {
        return;
    }
    UCX_TIMER_START(UCXTL_EPOLL_WAIT);
    status = ucp_worker_arm(ucx_worker);
    if (status != UCS_OK) {
        gpr_log(GPR_DEBUG, "UCX ucx_prepre_fd failed");
        return;
    }
    UCX_TIMER_END(UCXTL_EPOLL_WAIT);
}


uint64_t ucx_timer[UCXTL_SIZE];
uint64_t ucx_timer_mtx[UCXTL_SIZE];
