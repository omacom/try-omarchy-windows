#!/usr/bin/env python3
"""Run the in-process render server's worker teardown against real threads (#407).

The context worker thread, its socket transfers, the fence retire callback
and the DESTROY_CONTEXT, RESET and exit teardown are compiled from the patched
source against fake Venus queues and fake sockets. On Linux the harness runs
under ThreadSanitizer. Under MSYS2 the worker's socket transfers are also
compiled against real Winsock on the loopback pair proxy_socket_pair makes.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched virglrenderer source directory')
args = parser.parse_args()
server = (args.source / 'src/proxy/proxy_server.c').read_text(encoding='utf-8')
sockets = (args.source / 'src/proxy/proxy_socket.c').read_text(encoding='utf-8')
windows = os.name == 'nt' or sys.platform in ('msys', 'cygwin') or 'MSYSTEM' in os.environ


def between(text, start, end, after=0):
    i = text.index(start, after)
    return text[i:text.index(end, i)]


def case_body(name, first_line):
    # The statements of a server_main_thread case after its request is read.
    case = server.index(f'case {name}: {{')
    first = server.index(first_line, case)
    return server[first:server.index('         break;\n', first)]


structs = between(server, 'struct server_context_worker {', '/* global state for the retire_fence callback */')
transfers = between(server, 'static bool\nserver_recv_all(', 'static struct server_context_worker *\nserver_find_worker(')
find_retire = between(server, 'static struct server_context_worker *\nserver_find_worker(',
                      'static void\nserver_cb_debug_logger(')
worker_start = server.index('static int\nserver_context_worker_thread(void *arg)')
worker = server[worker_start:server.index('\n}\n', server.index('\ndone:\n', worker_start)) + 3]
destroy = between(server, 'static void\nserver_destroy_worker(', '/*\n * Server main thread')
destroy_context = case_body('RENDER_CLIENT_OP_DESTROY_CONTEXT', '         mtx_lock(&state->workers_mutex);')
reset = case_body('RENDER_CLIENT_OP_RESET', '         ')
exit_start = server.index('   /* Clean up remaining workers */')
server_exit = server[exit_start:server.index('   if (state->vkr_initialized)', exit_start)]
stop_events = 'stop_event' in structs

harness = r'''
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/list.h"
#include "server/render_protocol.h"

#define UNUSED __attribute__((unused))
typedef pthread_mutex_t mtx_t;
typedef pthread_t thrd_t;
#define mtx_lock pthread_mutex_lock
#define mtx_unlock pthread_mutex_unlock
static int thrd_join(thrd_t t, int *res) { (void)res; return pthread_join(t, NULL); }
static void proxy_log(const char *fmt, ...) { (void)fmt; }
static void nap(void) { sched_yield(); }
static void fail(const char *what) { fprintf(stderr, "%s\n", what); exit(1); }

/* Fake Win32 events and sockets, all behind one lock. A blocking recv waits
 * for data, the peer hanging up or the socket being closed, as on Windows;
 * shutdown is not offered because it does not wake recv there. Any call on a
 * socket that is already closed is recorded: by then the handle value may
 * belong to another socket. */
typedef void *HANDLE;
typedef HANDLE WSAEVENT;
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int BOOL;
typedef intptr_t SOCKET;
#define TRUE 1
#define FALSE 0
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define SOCKET_ERROR (-1)
#define WSA_IO_PENDING 997
#define WSA_INVALID_EVENT NULL
typedef struct { ULONG len; char *buf; } WSABUF;
typedef struct {
    HANDLE hEvent;
    bool pending, done, ok;
    DWORD got;
    char *buf;
    ULONG len;
} WSAOVERLAPPED, *LPOVERLAPPED;
struct fake_event { bool set; int owner; };

#define MAX_SOCKETS 8
/* Set when the server starts stopping a context's worker. */
static atomic_bool teardown[MAX_SOCKETS];
static pthread_mutex_t net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t net_cond = PTHREAD_COND_INITIALIZER;
static struct {
    char inbox[4096];
    size_t head, tail;
    bool peer_gone, recv_waiting;
    int closes;
    WSAOVERLAPPED *pending;
} net[MAX_SOCKETS];
static int stale_uses;
static _Thread_local int last_error;

static void stale(SOCKET s) { if (net[s].closes) stale_uses++; }
static size_t take(SOCKET s, char *buf, size_t len)
{
    size_t n = net[s].tail - net[s].head;
    if (n > len) n = len;
    memcpy(buf, net[s].inbox + net[s].head, n);
    net[s].head += n;
    return n;
}
static void complete(WSAOVERLAPPED *ov, bool ok, DWORD got)
{
    ov->pending = false; ov->done = true; ov->ok = ok; ov->got = got;
    ((struct fake_event *)ov->hEvent)->set = true;
    pthread_cond_broadcast(&net_cond);
}
static void settle(SOCKET s)
{
    WSAOVERLAPPED *ov = net[s].pending;
    if (!ov) return;
    if (net[s].tail > net[s].head) { net[s].pending = NULL; net[s].recv_waiting = false; complete(ov, true, (DWORD)take(s, ov->buf, ov->len)); }
    else if (net[s].peer_gone) { net[s].pending = NULL; net[s].recv_waiting = false; complete(ov, true, 0); }
}

/* Blocking calls the old worker used. */
static int fake_recv(SOCKET s, char *buf, int len, int flags)
{
    (void)flags;
    pthread_mutex_lock(&net_mutex);
    stale(s);
    net[s].recv_waiting = true;
    pthread_cond_broadcast(&net_cond);
    while (net[s].tail == net[s].head && !net[s].peer_gone && !net[s].closes)
        pthread_cond_wait(&net_cond, &net_mutex);
    net[s].recv_waiting = false;
    int ret = net[s].closes ? -1 : (int)take(s, buf, (size_t)len);
    pthread_mutex_unlock(&net_mutex);
    return ret;
}
static int fake_send(SOCKET s, const char *buf, int len, int flags)
{
    (void)buf; (void)flags;
    pthread_mutex_lock(&net_mutex);
    stale(s);
    int ret = net[s].closes ? -1 : len;
    pthread_mutex_unlock(&net_mutex);
    return ret;
}
static int closesocket(SOCKET s)
{
    pthread_mutex_lock(&net_mutex);
    net[s].closes++;
    atomic_store(&teardown[s], true);
    if (net[s].pending) { complete(net[s].pending, false, 0); net[s].pending = NULL; net[s].recv_waiting = false; }
    pthread_cond_broadcast(&net_cond);
    pthread_mutex_unlock(&net_mutex);
    return 0;
}
#define recv fake_recv
#define send fake_send

/* Overlapped calls the new worker uses. */
static int WSARecv(SOCKET s, WSABUF *b, DWORD n, DWORD *got, DWORD *flags, WSAOVERLAPPED *ov, void *routine)
{
    (void)n; (void)flags; (void)routine;
    if (got || !ov) fail("WSARecv: only overlapped calls are faked");
    pthread_mutex_lock(&net_mutex);
    stale(s);
    int ret = 0;
    if (net[s].closes) { last_error = 10038; ret = SOCKET_ERROR; }
    else {
        ov->buf = b->buf; ov->len = b->len; ov->pending = true; ov->done = false;
        net[s].pending = ov;
        settle(s);
        if (ov->pending) { net[s].recv_waiting = true; pthread_cond_broadcast(&net_cond); last_error = WSA_IO_PENDING; ret = SOCKET_ERROR; }
    }
    pthread_mutex_unlock(&net_mutex);
    return ret;
}
static int WSASend(SOCKET s, WSABUF *b, DWORD n, DWORD *got, DWORD flags, WSAOVERLAPPED *ov, void *routine)
{
    (void)n; (void)flags; (void)routine;
    if (got || !ov) fail("WSASend: only overlapped calls are faked");
    pthread_mutex_lock(&net_mutex);
    stale(s);
    int ret = 0;
    if (net[s].closes) { last_error = 10038; ret = SOCKET_ERROR; }
    else complete(ov, true, b->len);
    pthread_mutex_unlock(&net_mutex);
    return ret;
}
static int WSAGetLastError(void) { return last_error; }
static BOOL CancelIoEx(HANDLE h, WSAOVERLAPPED *ov)
{
    SOCKET s = (SOCKET)h;
    pthread_mutex_lock(&net_mutex);
    stale(s);
    BOOL found = net[s].pending == ov;
    if (found) { net[s].pending = NULL; net[s].recv_waiting = false; complete(ov, false, 0); }
    pthread_mutex_unlock(&net_mutex);
    return found;
}
static BOOL WSAGetOverlappedResult(SOCKET s, WSAOVERLAPPED *ov, DWORD *got, BOOL wait, DWORD *flags)
{
    (void)flags;
    pthread_mutex_lock(&net_mutex);
    while (wait && !ov->done)
        pthread_cond_wait(&net_cond, &net_mutex);
    if (net[s].pending == ov) fail("overlapped transfer left pending");
    *got = ov->got;
    BOOL ok = ov->done && ov->ok;
    pthread_mutex_unlock(&net_mutex);
    return ok;
}
static HANDLE new_event(int owner)
{
    struct fake_event *e = calloc(1, sizeof(*e));
    e->owner = owner;
    return e;
}
static BOOL SetEvent(HANDLE h)
{
    pthread_mutex_lock(&net_mutex);
    ((struct fake_event *)h)->set = true;
    atomic_store(&teardown[((struct fake_event *)h)->owner], true);
    pthread_cond_broadcast(&net_cond);
    pthread_mutex_unlock(&net_mutex);
    return TRUE;
}
static BOOL WSAResetEvent(HANDLE h)
{
    pthread_mutex_lock(&net_mutex);
    ((struct fake_event *)h)->set = false;
    pthread_mutex_unlock(&net_mutex);
    return TRUE;
}
static bool event_set(HANDLE h)
{
    pthread_mutex_lock(&net_mutex);
    bool set = ((struct fake_event *)h)->set;
    pthread_mutex_unlock(&net_mutex);
    return set;
}
static BOOL CloseHandle(HANDLE h) { free(h); return TRUE; }
static BOOL WSACloseEvent(HANDLE h) { free(h); return TRUE; }
static DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (ms) fail("WaitForSingleObject: only polls are faked");
    return event_set(h) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    if (all || ms != INFINITE) fail("WaitForMultipleObjects: only wait-any is faked");
    pthread_mutex_lock(&net_mutex);
    for (;;) {
        for (DWORD i = 0; i < n; i++) {
            if (((struct fake_event *)h[i])->set) {
                pthread_mutex_unlock(&net_mutex);
                return WAIT_OBJECT_0 + i;
            }
        }
        pthread_cond_wait(&net_cond, &net_mutex);
    }
}
static void deliver(int s, const void *data, size_t len)
{
    pthread_mutex_lock(&net_mutex);
    memcpy(net[s].inbox + net[s].tail, data, len);
    net[s].tail += len;
    settle(s);
    pthread_cond_broadcast(&net_cond);
    pthread_mutex_unlock(&net_mutex);
}
static void hang_up(int s)
{
    pthread_mutex_lock(&net_mutex);
    net[s].peer_gone = true;
    settle(s);
    pthread_cond_broadcast(&net_cond);
    pthread_mutex_unlock(&net_mutex);
}
/* The worker has read everything QEMU sent and waits for more. */
static void wait_idle(int s)
{
    pthread_mutex_lock(&net_mutex);
    while (!net[s].recv_waiting || net[s].tail != net[s].head)
        pthread_cond_wait(&net_cond, &net_mutex);
    pthread_mutex_unlock(&net_mutex);
}

static atomic_int eventfd_writes, unmapped;
static int write_eventfd(int fd, uint64_t val) { (void)fd; (void)val; atomic_fetch_add(&eventfd_writes, 1); return 0; }
static void UnmapViewOfFile(void *p) { (void)p; atomic_fetch_add(&unmapped, 1); }
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE 1
static atomic_uint seqnos[MAX_SOCKETS][64];
static void *mmap(void *addr, size_t size, int prot, int flags, int fd, long off)
{
    (void)addr; (void)size; (void)prot; (void)flags; (void)off;
    return seqnos[fd - 20];
}
'''
harness += structs + r'''
static struct server_thread_state *g_server_state;
''' + transfers + find_retire + r'''
static bool server_ensure_vkr_init(struct server_thread_state *state) { (void)state; return true; }

/* One Venus queue per context, as in vkr_queue.c: its sync thread retires
 * fences holding sync_thread.mutex, and vkr_queue_sync_thread_fini takes
 * that mutex, joins the thread, then retires the syncs it left behind. */
enum retire_mode { RETIRE_NONE, RETIRE_IN_FLIGHT, RETIRE_LEFTOVER };
struct fake_queue {
    uint32_t ctx_id;
    enum retire_mode mode;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool join;
    atomic_bool holding;
    pthread_t thread;
};
static struct fake_queue queues[MAX_SOCKETS];
static atomic_bool busy_command[MAX_SOCKETS], in_command[MAX_SOCKETS];

static void *queue_thread(void *arg)
{
    struct fake_queue *q = arg;
    pthread_mutex_lock(&q->mutex);
    if (q->mode == RETIRE_IN_FLIGHT) {
        /* A fence signals just as the context is torn down. */
        atomic_store(&q->holding, true);
        while (!atomic_load(&teardown[q->ctx_id]))
            nap();
        server_retire_fence_cb(q->ctx_id, 1, 7);
    }
    while (!q->join)
        pthread_cond_wait(&q->cond, &q->mutex);
    pthread_mutex_unlock(&q->mutex);
    return NULL;
}

static bool vkr_renderer_create_context(uint32_t ctx_id, uint32_t flags, uint32_t len, const char *name)
{
    (void)ctx_id; (void)flags; (void)len; (void)name;
    return true;
}
static bool vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
    (void)cmd; (void)size;
    if (atomic_load(&busy_command[ctx_id])) {
        /* Still working on a command when the server stops the context. */
        atomic_store(&in_command[ctx_id], true);
        pthread_mutex_lock(&net_mutex);
        while (!atomic_load(&teardown[ctx_id]))
            pthread_cond_wait(&net_cond, &net_mutex);
        pthread_mutex_unlock(&net_mutex);
    }
    return true;
}
static bool vkr_renderer_submit_fence(uint32_t ctx_id, uint32_t flags, uint32_t ring, uint64_t seqno)
{ (void)ctx_id; (void)flags; (void)ring; (void)seqno; return true; }
static bool vkr_renderer_create_resource(uint32_t ctx_id, uint32_t res_id, uint64_t blob_id, uint64_t size,
                                         uint32_t flags, enum virgl_resource_fd_type *type, int *fd,
                                         uint32_t *map_info, struct virgl_resource_vulkan_info *info)
{ (void)ctx_id; (void)res_id; (void)blob_id; (void)size; (void)flags; (void)type; (void)fd; (void)map_info; (void)info; return false; }
static bool vkr_renderer_import_resource(uint32_t ctx_id, uint32_t res_id, enum virgl_resource_fd_type type, int fd, uint64_t size)
{ (void)ctx_id; (void)res_id; (void)type; (void)fd; (void)size; return true; }
static void vkr_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id) { (void)ctx_id; (void)res_id; }
static void vkr_renderer_destroy_context(uint32_t ctx_id)
{
    struct fake_queue *q = &queues[ctx_id];
    pthread_mutex_lock(&q->mutex);
    q->join = true;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    pthread_join(q->thread, NULL);
    if (q->mode == RETIRE_LEFTOVER)
        server_retire_fence_cb(ctx_id, 1, 9);
}
''' + worker + destroy + r'''
static void *worker_main(void *w) { server_context_worker_thread(w); return NULL; }
static void destroy_context(struct server_thread_state *state, uint32_t ctx_id)
{
    do {
''' + destroy_context + r'''
    } while (0);
}
static void reset(struct server_thread_state *state)
{
    do {
''' + reset + r'''
    } while (0);
}
static void server_exit(struct server_thread_state *state)
{
''' + server_exit + r'''
}

/* Everything QEMU's proxy_context sends for a context: INIT, then commands. */
static void send_init(int s)
{
    struct render_context_op_header hdr = { .op = RENDER_CONTEXT_OP_INIT };
    uint32_t flags = 0, fd_count = 2;
    size_t shmem_size = 64 * sizeof(atomic_uint);
    int fds[2] = { 20 + s, 40 + s };
    deliver(s, &hdr, sizeof(hdr));
    deliver(s, &flags, sizeof(flags));
    deliver(s, &shmem_size, sizeof(shmem_size));
    deliver(s, &fd_count, sizeof(fd_count));
    deliver(s, fds, sizeof(fds));
}
static void send_command(int s)
{
    struct render_context_op_header hdr = { .op = RENDER_CONTEXT_OP_SUBMIT_CMD };
    struct { uint32_t size; char cmd[256]; } body = { .size = 16 };
    deliver(s, &hdr, sizeof(hdr));
    deliver(s, &body, sizeof(body));
}

static struct server_context_worker *start(struct server_thread_state *state,
                                           uint32_t ctx_id, enum retire_mode mode)
{
    struct fake_queue *q = &queues[ctx_id];
    memset(q, 0, sizeof(*q));
    q->ctx_id = ctx_id;
    q->mode = mode;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    pthread_create(&q->thread, NULL, queue_thread, q);
    if (mode == RETIRE_IN_FLIGHT)
        while (!atomic_load(&q->holding))
            nap();

    struct server_context_worker *w = calloc(1, sizeof(*w));
    w->ctx_id = ctx_id;
    w->socket_fd = (int)ctx_id;
    w->shmem_fd = -1;
    w->fence_eventfd = -1;
#ifdef STOP_EVENTS
    w->stop_event = new_event((int)ctx_id);
    w->io_event = new_event(0);
#endif
    pthread_create(&w->thread, NULL, worker_main, w);
    mtx_lock(&state->workers_mutex);
    list_addtail(&w->head, &state->workers);
    mtx_unlock(&state->workers_mutex);
    send_init((int)ctx_id);
    wait_idle((int)ctx_id);
    return w;
}

static void fresh(void)
{
    pthread_mutex_lock(&net_mutex);
    memset(net, 0, sizeof(net));
    for (int s = 0; s < MAX_SOCKETS; s++)
        atomic_store(&teardown[s], false);
    pthread_mutex_unlock(&net_mutex);
    memset(seqnos, 0, sizeof(seqnos));
    atomic_store(&eventfd_writes, 0);
    atomic_store(&unmapped, 0);
}

static void check_closed(int from, int to, const char *what)
{
    pthread_mutex_lock(&net_mutex);
    for (int s = from; s <= to; s++) {
        if (net[s].closes != 1) {
            fprintf(stderr, "%s: socket %d closed %d times\n", what, s, net[s].closes);
            exit(1);
        }
    }
    if (stale_uses) {
        fprintf(stderr, "%s: a socket was used after it was closed\n", what);
        exit(1);
    }
    pthread_mutex_unlock(&net_mutex);
}

int main(void)
{
    struct server_thread_state *state = calloc(1, sizeof(*state));
    pthread_mutex_init(&state->workers_mutex, NULL);
    pthread_mutex_init(&state->renderer_mutex, NULL);
    list_inithead(&state->workers);
    g_server_state = state;

    /* DESTROY_CONTEXT while a queue thread retires a fence (#407). The fence
     * of the dying context is dropped; proxy_context_destroy has already
     * stopped reading the timeline. */
    fresh();
    start(state, 2, RETIRE_IN_FLIGHT);
    destroy_context(state, 2);
    check_closed(2, 2, "destroy with fence in flight");
    if (atomic_load(&seqnos[2][1]) || atomic_load(&eventfd_writes) || atomic_load(&unmapped) != 1)
        fail("retire reached an unlinked worker");
    /* A second DESTROY_CONTEXT for the same id does nothing. */
    destroy_context(state, 2);
    check_closed(2, 2, "repeated destroy");

    /* The worker is still running a command when the destroy comes, with
     * another command queued behind it. It must stop without another call
     * on its socket once that socket may be closed. */
    fresh();
    start(state, 3, RETIRE_NONE);
    atomic_store(&busy_command[3], true);
    send_command(3);
    while (!atomic_load(&in_command[3]))
        nap();
    send_command(3);
    destroy_context(state, 3);
    check_closed(3, 3, "destroy while busy in a command");

    /* A linked worker still gets its fences. */
    fresh();
    start(state, 1, RETIRE_NONE);
    server_retire_fence_cb(1, 3, 5);
    if (atomic_load(&seqnos[1][3]) != 5 || atomic_load(&eventfd_writes) != 1)
        fail("live fence was not retired");
    destroy_context(state, 1);
    check_closed(1, 1, "idle destroy");

    /* vkr_queue_sync_thread_fini retires leftover syncs on the worker itself. */
    fresh();
    start(state, 4, RETIRE_LEFTOVER);
    destroy_context(state, 4);
    check_closed(4, 4, "destroy with leftover syncs");

    /* A worker that already exited because QEMU hung up. */
    fresh();
    start(state, 5, RETIRE_NONE);
    hang_up(5);
    while (!atomic_load(&unmapped))
        nap();
    destroy_context(state, 5);
    check_closed(5, 5, "destroy after hang-up");

    /* RESET and server exit with fences in flight on several contexts. */
    fresh();
    start(state, 6, RETIRE_IN_FLIGHT);
    start(state, 7, RETIRE_LEFTOVER);
    reset(state);
    check_closed(6, 7, "reset");
    fresh();
    start(state, 1, RETIRE_IN_FLIGHT);
    server_exit(state);
    check_closed(1, 1, "server exit");
    if (!list_is_empty(&state->workers))
        fail("workers left after teardown");
    puts("ok - Venus worker teardown: fence in flight, repeated destroy, busy worker, live fences, leftover syncs, hang-up, reset, exit, one close per socket and none used after");
    return 0;
}
'''
if stop_events:
    harness = '#define STOP_EVENTS 1\n' + harness

# The real worker transfers on the real loopback pair under Winsock.
pair = between(sockets, 'bool\nproxy_socket_pair(int out_fds[static 2])\n{\n   WSADATA', '\n}\n') + '\n}\n'
native = r'''
#include <winsock2.h>
#include <windows.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "util/list.h"
typedef HANDLE thrd_t;
typedef CRITICAL_SECTION mtx_t;
static void proxy_log(const char *fmt, ...) { (void)fmt; }
''' + structs + transfers + pair + r'''
static struct server_context_worker w;
static volatile int got = -1;
static char one;
static DWORD WINAPI reader(LPVOID arg) { (void)arg; got = server_worker_recv(&w, &one, 1); return 0; }
static int fail(const char *what) { fprintf(stderr, "%s\n", what); return 1; }
int main(void)
{
    int fds[2];
    if (!proxy_socket_pair(fds)) return fail("no socket pair");
    w.socket_fd = fds[0];
    w.stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    w.io_event = WSACreateEvent();

    /* Transfers in both directions. */
    char out[3] = {'a', 'b', 'c'}, in[3] = {0};
    if (send((SOCKET)fds[1], out, 3, 0) != 3 || !server_worker_recv(&w, in, 3) || memcmp(in, out, 3))
        return fail("worker recv failed");
    if (!server_worker_send(&w, out, 3) || recv((SOCKET)fds[1], in, 3, MSG_WAITALL) != 3)
        return fail("worker send failed");

    /* A stopped worker that was busy does not start another transfer: the
     * byte QEMU sends stays queued for whoever reads next. */
    if (send((SOCKET)fds[1], out, 1, 0) != 1) return fail("peer send failed");
    SetEvent(w.stop_event);
    if (server_worker_recv(&w, in, 1)) return fail("stopped worker received");
    ResetEvent(w.stop_event);
    if (!server_worker_recv(&w, in, 1) || in[0] != 'a') return fail("stopped worker consumed data");

    /* A worker blocked in recv wakes on stop_event while QEMU's end stays
     * open. */
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (WaitForSingleObject(t, 200) != WAIT_TIMEOUT) return fail("recv did not block");
    SetEvent(w.stop_event);
    if (WaitForSingleObject(t, 5000) != WAIT_OBJECT_0) return fail("stop_event did not wake recv");
    if (got) return fail("stopped recv reported data");

    closesocket((SOCKET)fds[0]);
    closesocket((SOCKET)fds[1]);
    puts("ok - Winsock worker transfers: recv and send, stop wakes a blocked recv with the peer open, a stopped worker starts no transfer");
    return 0;
}
'''

include = ['-I', str(args.source / 'src/mesa'), '-I', str(args.source / 'src'), '-I', str(args.source)]
with tempfile.TemporaryDirectory(prefix='tryomarchy-venus-worker-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    sanitize = [] if windows else ['-fsanitize=thread', '-g']
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Wno-unused-parameter', '-Werror', '-pthread', *sanitize, *include,
                    str(test / 'test.c'), '-o', str(binary)], check=True)
    try:
        subprocess.run([str(binary)], check=True, timeout=60,
                       env={**os.environ, 'TSAN_OPTIONS': 'halt_on_error=1'})
    except subprocess.TimeoutExpired:
        sys.exit('worker teardown deadlocked (#407)')
    if windows:
        if not stop_events:
            sys.exit('the worker has no stop event')
        (test / 'native.c').write_text(native, encoding='utf-8')
        native_binary = test / 'native.exe'
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function', '-Werror',
                        *include, str(test / 'native.c'), '-o', str(native_binary), '-lws2_32'], check=True)
        subprocess.run([str(native_binary)], check=True, timeout=30)
