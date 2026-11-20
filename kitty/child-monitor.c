/*
 * child-monitor.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "loop-utils.h"
#include "safe-wrappers.h"
#include "state.h"
#include "gl.h"
#include "threading.h"
#include "screen.h"
#include "monotonic.h"
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <stdatomic.h>
#include <limits.h>
extern PyTypeObject Screen_Type;

#if defined(__APPLE__) || defined(__OpenBSD__)
#define NO_SIGQUEUE 1
#endif

#ifdef DEBUG_EVENT_LOOP
#define EVDBG(...) timed_debug_print(__VA_ARGS__)
#else
#define EVDBG(...)
#endif

#define EXTRA_FDS 2
#define DISPLAY_POLL_INTERVAL ms_to_monotonic_t(250ll)
// An e-paper frame may share a wakeup up to 30 ms after it becomes eligible.
#define EINK_FRAME_TIMER_TOLERANCE ms_to_monotonic_t(30ll)
#ifndef MSG_NOSIGNAL
// Apple does not implement MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define USE_RENDER_FRAMES (global_state.has_render_frames && OPT(sync_to_monitor))

typedef struct {
    char *data;
    size_t sz;
    id_type peer_id;
    bool is_remote_control_peer;
} Message;

typedef struct {
    PyObject_HEAD

        PyObject *dump_callback,
        *update_screen, *death_notify;
    size_t count;
    bool shutting_down;
    pthread_t io_thread, talk_thread;

    int talk_fd, listen_fd;
    // Peer credentials can only be checked for UNIX sockets, so remember
    // which of the listening sockets are UNIX sockets.
    bool verify_talk_peer_uid, verify_listen_peer_uid;
    int benchmark_wakeup_fd;
    Message *messages;
    size_t messages_capacity, messages_count;
    LoopData io_loop_data;
    void (*parse_func)(void *, ParseData *, bool);
} ChildMonitor;


typedef struct {
    Screen *screen;
    bool needs_removal, child_died, close_requested;
    int fd;
    unsigned long id;
    pid_t pid;
    int exit_status;
} Child;

static const Child EMPTY_CHILD = {0};
#define screen_mutex(op, which) pthread_mutex_##op(&screen->which##_buf_lock);
#define children_mutex(op) pthread_mutex_##op(&children_lock);
#define talk_mutex(op) pthread_mutex_##op(&talk_lock);


// A child goes from add_queue to children, in the I/O thread, and from there to remove_queue, from which the main thread
// takes it to remove_notify; scratch is the main thread's copy of children. All of them grow as children are added,
// see reserve_child_slot(), so that a child can always move on once it has been added, without the memory for it
// having to be found in the I/O thread. Only children_fds, which poll() uses without the lock, is grown by the I/O
// thread, which polls it, see add_children(). Children are referred to by id, never by pointer or index, as these
// arrays move and the entries in children shift when one is removed.
static Child *children = NULL, *scratch = NULL, *add_queue = NULL, *remove_queue = NULL, *remove_notify = NULL;
static size_t children_capacity = 0, scratch_capacity = 0, add_queue_capacity = 0, remove_queue_capacity = 0, remove_notify_capacity = 0;
// Pending input of each child, as the I/O thread last saw it. Parallel to children and
// only used by the I/O thread, so it needs no lock.
typedef struct {
    ParserInputWake pending;
    // The chunk the main loop was last woken for. Its next parse accepts that chunk, so
    // more reads of it must not wake it again.
    uint64_t woken_chunk;
    // Bytes were committed since pending was queried
    bool stale;
} ChildInputWake;
// Grown with children_fds, in add_children(), the only place children are added to
static ChildInputWake *child_input_wake = NULL;
static size_t child_input_wake_capacity = 0;
static size_t add_queue_count = 0, remove_queue_count = 0;
static struct pollfd *children_fds = NULL;
static size_t children_fds_capacity = 0;
static pthread_mutex_t children_lock, talk_lock;
static bool kill_signal_received = false, reload_config_signal_received = false;
static ChildMonitor *the_monitor = NULL;

typedef struct {
    pid_t pid;
    int status;
} ReapedPID;

// A pid goes from monitored_pids to reaped_pids when it dies, so reserving room in both when one is monitored means
// one that dies can always be recorded, see monitor_pid()
static pid_t *monitored_pids = NULL;
static size_t monitored_pids_count = 0, monitored_pids_capacity = 0;
static ReapedPID *reaped_pids = NULL;
static size_t reaped_pids_count = 0, reaped_pids_capacity = 0;

// Makes *items, an array of entries of size bytes, room for at least needed entries, the new ones zeroed. Returns
// false, changing nothing, when that much memory cannot be had.
static bool
grow_array(void **items, size_t *capacity, size_t needed, size_t size) {
    if (needed <= *capacity) return true;
    size_t new_capacity = *capacity < 16 ? 16 : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / size) return false;
    void *grown = realloc(*items, new_capacity * size);
    if (!grown) return false;
    memset((char *)grown + *capacity * size, 0, (new_capacity - *capacity) * size);
    *items = grown;
    *capacity = new_capacity;
    return true;
}
#define GROW(name, needed) grow_array((void **)&name, &name##_capacity, needed, sizeof(name[0]))

// Room for one more child in every array it can be in, with the children lock held. The number of children in all
// of them together grows only here.
static bool
reserve_child_slot(size_t count) {
    if (count > SIZE_MAX - add_queue_count - remove_queue_count - 1) return false;
    const size_t total = count + add_queue_count + remove_queue_count + 1;
    return GROW(add_queue, add_queue_count + 1) && GROW(children, total) && GROW(remove_queue, total) && GROW(scratch, total) &&
           GROW(remove_notify, total);
}


// Main thread functions {{{

#define FREE_CHILD(x)     \
    Py_CLEAR((x).screen); \
    x = EMPTY_CHILD;

#define XREF_CHILD(x, OP) OP(x.screen);
#define INCREF_CHILD(x) XREF_CHILD(x, Py_INCREF)
#define DECREF_CHILD(x) XREF_CHILD(x, Py_DECREF)

// The first eligible wakeup and the latest acceptable wakeup. A negative
// value means there is no deadline.
static monotonic_t wakeup_wait = -1, wakeup_deadline = -1;

static void
set_wakeup_wait(monotonic_t wait, monotonic_t tolerance) {
    if (wait < 0) return;
    const monotonic_t deadline = wait + MIN(tolerance, MONOTONIC_T_MAX - wait);
    if (wakeup_wait < 0 || wait < wakeup_wait) wakeup_wait = wait;
    if (wakeup_deadline < 0 || deadline < wakeup_deadline) wakeup_deadline = deadline;
}

static void
set_maximum_wait(monotonic_t wait) {
    set_wakeup_wait(wait, 0);
}

#define KITTY_HANDLED_SIGNALS SIGINT, SIGHUP, SIGTERM, SIGCHLD, SIGUSR1, SIGUSR2, 0

static void
mask_variadic_signals(int sentinel, ...) {
    sigset_t signals;
    sigemptyset(&signals);
    va_list valist;
    va_start(valist, sentinel);
    while (true) {
        int sig = va_arg(valist, int);
        if (sig == sentinel) break;
        sigaddset(&signals, sig);
    }
    va_end(valist);
#ifdef HAS_SIGNAL_FD
    sigprocmask(SIG_BLOCK, &signals, NULL);
#else
    struct sigaction act = {.sa_handler = SIG_IGN, .sa_flags = SA_RESTART, .sa_mask = signals};
    va_start(valist, sentinel);
    while (true) {
        int sig = va_arg(valist, int);
        if (sig == sentinel) break;
        sigaction(sig, &act, NULL);
    }
    va_end(valist);
#endif
}

static PyObject *
mask_kitty_signals_process_wide(PyObject *self UNUSED, PyObject *a UNUSED) {
    mask_variadic_signals(0, KITTY_HANDLED_SIGNALS);
    Py_RETURN_NONE;
}

static PyObject *
new_childmonitor_object(PyTypeObject *type, PyObject *args, PyObject UNUSED *kwds) {
    ChildMonitor *self;
    PyObject *dump_callback, *death_notify;
    int talk_fd = -1, listen_fd = -1;
    int ret;

    if (the_monitor) {
        PyErr_SetString(PyExc_RuntimeError, "Can have only a single ChildMonitor instance");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "OO|ii", &death_notify, &dump_callback, &talk_fd, &listen_fd)) return NULL;
    // the fds polled before any child is added, the wakeup and signal fds
    if (!GROW(children_fds, EXTRA_FDS + 1)) return PyErr_NoMemory();
    if ((ret = pthread_mutex_init(&children_lock, NULL)) != 0) {
        PyErr_Format(PyExc_RuntimeError, "Failed to create children_lock mutex: %s", strerror(ret));
        return NULL;
    }
    if ((ret = pthread_mutex_init(&talk_lock, NULL)) != 0) {
        PyErr_Format(PyExc_RuntimeError, "Failed to create talk_lock mutex: %s", strerror(ret));
        return NULL;
    }
    self = (ChildMonitor *)type->tp_alloc(type, 0);
    if (!init_loop_data(&self->io_loop_data, KITTY_HANDLED_SIGNALS)) return PyErr_SetFromErrno(PyExc_OSError);
    self->talk_fd = talk_fd;
    self->listen_fd = listen_fd;
    // Only processes running as the same user as us are allowed to talk to us
    // over UNIX sockets. In particular, the single instance socket is an
    // abstract UNIX socket on Linux, which any process on the system can
    // connect to.
    self->verify_talk_peer_uid = talk_fd > -1 && peer_credentials_are_available(talk_fd);
    self->verify_listen_peer_uid = listen_fd > -1 && peer_credentials_are_available(listen_fd);
    self->benchmark_wakeup_fd = -1;
    if (self == NULL) return PyErr_NoMemory();
    self->death_notify = death_notify;
    Py_INCREF(death_notify);
    if (dump_callback != Py_None) {
        self->dump_callback = dump_callback;
        Py_INCREF(dump_callback);
        self->parse_func = parse_worker_dump;
    } else self->parse_func = parse_worker;
    self->count = 0;
    children_fds[0].fd = self->io_loop_data.wakeup_read_fd;
    children_fds[1].fd = self->io_loop_data.signal_read_fd;
    children_fds[0].events = POLLIN;
    children_fds[1].events = POLLIN;
    children_fds[2].events = POLLIN;
    the_monitor = self;

    return (PyObject *)self;
}

static void
dealloc(ChildMonitor *self) {
    if (self->messages) {
        for (size_t i = 0; i < self->messages_count; i++) free(self->messages[i].data);
        free(self->messages);
        self->messages = NULL;
        self->messages_count = 0;
        self->messages_capacity = 0;
    }
    pthread_mutex_destroy(&children_lock);
    pthread_mutex_destroy(&talk_lock);
    Py_CLEAR(self->dump_callback);
    Py_CLEAR(self->death_notify);
    while (remove_queue_count) {
        remove_queue_count--;
        FREE_CHILD(remove_queue[remove_queue_count]);
    }
    while (add_queue_count) {
        add_queue_count--;
        FREE_CHILD(add_queue[add_queue_count]);
    }
    free_loop_data(&self->io_loop_data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
handled_signals(ChildMonitor *self, PyObject *args UNUSED) {
    PyObject *ans = PyTuple_New(self->io_loop_data.num_handled_signals);
    if (ans) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(ans); i++) { PyTuple_SET_ITEM(ans, i, PyLong_FromLong((long)self->io_loop_data.handled_signals[i])); }
    }
    return ans;
}

static void
wakeup_io_loop(ChildMonitor *self, bool in_signal_handler) {
    wakeup_loop(&self->io_loop_data, in_signal_handler, "io_loop");
}

static void *io_loop(void *data);
static void *talk_loop(void *data);
static void send_response_to_peer(id_type peer_id, const char *msg, size_t msg_sz, bool is_async_response);
static void wakeup_talk_loop(bool);
static bool add_peer_to_injection_queue(int peer_fd, int pipe_fd);
static int start_talk_thread(ChildMonitor *);
static bool talk_thread_started = false;

static bool
simple_read_from_pipe(int fd, void *data, size_t sz) {
    // read a small amount of data to a pipe handling only EINTR
    while (true) {
        ssize_t ret = read(fd, data, sz);
        if (ret == -1 && errno == EINTR) continue;
        return ret == (ssize_t)sz;
    }
}


static PyObject *
inject_peer(PyObject *s, PyObject *a) {
#define inject_peer_doc "inject_peer(fd) -> Start communication with a peer over the specified file descriptor"
    ChildMonitor *self = (ChildMonitor *)s;
    if (!PyLong_Check(a)) {
        PyErr_SetString(PyExc_TypeError, "peer fd must be an int");
        return NULL;
    }
    long fd = PyLong_AsLong(a);
    if (fd < 0) {
        PyErr_Format(PyExc_ValueError, "Invalid peer fd: %ld", fd);
        return NULL;
    }
    if ((errno = start_talk_thread(self)) != 0) return PyErr_SetFromErrno(PyExc_OSError);
    int fds[2] = {0};
    if (!self_pipe(fds, false)) {
        safe_close(fd, __FILE__, __LINE__);
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    if (!add_peer_to_injection_queue(fd, fds[1])) {
        safe_close(fd, __FILE__, __LINE__);
        safe_close(fds[0], __FILE__, __LINE__);
        safe_close(fds[1], __FILE__, __LINE__);
        PyErr_SetString(PyExc_RuntimeError, "Too many peers waiting to be injected");
        return NULL;
    }
    wakeup_talk_loop(false);
    id_type peer_id = 0;
    bool ok = simple_read_from_pipe(fds[0], &peer_id, sizeof(peer_id));
    safe_close(fds[0], __FILE__, __LINE__);
    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to read peer id from self pipe");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(peer_id);
}

static PyObject *
start(PyObject *s, PyObject *a UNUSED) {
#define start_doc "start() -> Start the I/O thread"
    ChildMonitor *self = (ChildMonitor *)s;
    int ret;
    if (self->talk_fd > -1 || self->listen_fd > -1) {
        if ((errno = start_talk_thread(self)) != 0) return PyErr_SetFromErrno(PyExc_OSError);
    }
    ret = pthread_create(&self->io_thread, NULL, io_loop, self);
    if (ret != 0) return PyErr_Format(PyExc_OSError, "Failed to start I/O thread with error: %s", strerror(ret));

    Py_RETURN_NONE;
}

static PyObject *
wakeup(ChildMonitor *self, PyObject *args UNUSED) {
#define wakeup_doc "wakeup() -> wakeup the ChildMonitor I/O thread, forcing it to exit from poll() if it is waiting there."
    wakeup_io_loop(self, false);
    Py_RETURN_NONE;
}

static PyObject *
add_child(ChildMonitor *self, PyObject *args) {
#define add_child_doc "add_child(id, pid, fd, screen) -> Add a child."
    children_mutex(lock);
    if (!reserve_child_slot(self->count)) {
        children_mutex(unlock);
        PyErr_SetString(PyExc_MemoryError, "Out of memory adding a child");
        return NULL;
    }
    add_queue[add_queue_count] = EMPTY_CHILD;
#define A(attr) &add_queue[add_queue_count].attr
    if (!PyArg_ParseTuple(args, "kiiO", A(id), A(pid), A(fd), A(screen))) {
        children_mutex(unlock);
        return NULL;
    }
#undef A
    INCREF_CHILD(add_queue[add_queue_count]);
    add_queue_count++;
    children_mutex(unlock);
    wakeup_io_loop(self, false);
    Py_RETURN_NONE;
}

// Data queued for a child (keyboard input, pastes, replies to queries) is never refused for being too much: the queue
// grows for as long as memory lasts. Only writers that can wait and retry, such as drag and drop transfers, pause
// once this much is pending, so that they do not queue a whole transfer in memory ahead of a slow child.
static const size_t write_buf_flow_control_threshold = 100 * 1024 * 1024;

typedef enum { WRITE_BUF_OK, WRITE_BUF_OVER_THRESHOLD, WRITE_BUF_NO_MEMORY } WriteBufSpace;

static WriteBufSpace
ensure_write_buf_space(Screen *screen, size_t sz, size_t threshold) {
    if (screen->write_buf_sz - screen->write_buf_used >= sz) return WRITE_BUF_OK;
    const size_t pending = screen->write_buf_used - screen->write_buf_start;
    size_t needed;
    if (__builtin_add_overflow(pending, sz, &needed)) return WRITE_BUF_NO_MEMORY;
    if (needed > threshold) return WRITE_BUF_OVER_THRESHOLD;
    if (screen->write_buf_start) {
        memmove(screen->write_buf, screen->write_buf + screen->write_buf_start, pending);
        screen->write_buf_start = 0;
        screen->write_buf_used = pending;
    }
    if (screen->write_buf_sz < needed) {
        // Grow geometrically so that a stream of small writes to a child that is slow to read costs amortized O(1)
        size_t new_sz = screen->write_buf_sz;
        if (__builtin_mul_overflow(new_sz, (size_t)2, &new_sz) || new_sz < needed) new_sz = needed;
        uint8_t *buf = PyMem_RawRealloc(screen->write_buf, new_sz);
        if (!buf && new_sz > needed) buf = PyMem_RawRealloc(screen->write_buf, (new_sz = needed));
        if (!buf) return WRITE_BUF_NO_MEMORY;
        screen->write_buf = buf;
        screen->write_buf_sz = new_sz;
    }
    return WRITE_BUF_OK;
}

// Returns the buffer to its usual size once a burst of data has been written out; keeps the larger one if that fails
static void
shrink_write_buf(Screen *screen) {
    if (screen->write_buf_sz > BUFSIZ && screen->write_buf_used < BUFSIZ) {
        uint8_t *buf = PyMem_RawRealloc(screen->write_buf, BUFSIZ);
        if (buf) {
            screen->write_buf = buf;
            screen->write_buf_sz = BUFSIZ;
        }
    }
}

#define schedule_write_to_child_generic(id, num, va_start, get_next_arg, va_end, found, out_of_memory) \
    ChildMonitor *self = the_monitor;                                                                  \
    const char *data;                                                                                  \
    size_t szval, sz = 0;                                                                              \
    bool sz_overflow = false;                                                                          \
    va_start(ap, num);                                                                                 \
    for (unsigned int i = 0; i < num; i++) {                                                           \
        get_next_arg(ap);                                                                              \
        if (__builtin_add_overflow(sz, szval, &sz)) sz_overflow = true;                                \
    }                                                                                                  \
    va_end(ap);                                                                                        \
    children_mutex(lock);                                                                              \
    for (size_t i = 0; i < self->count; i++) {                                                         \
        if (children[i].id == id) {                                                                    \
            Screen *screen = children[i].screen;                                                       \
            screen_mutex(lock, write);                                                                 \
            if (sz_overflow || ensure_write_buf_space(screen, sz, SIZE_MAX) != WRITE_BUF_OK) {         \
                out_of_memory = true;                                                                  \
                screen_mutex(unlock, write);                                                           \
                break;                                                                                 \
            }                                                                                          \
            found = true;                                                                              \
            va_start(ap, num);                                                                         \
            for (unsigned int i = 0; i < num; i++) {                                                   \
                get_next_arg(ap);                                                                      \
                memcpy(screen->write_buf + screen->write_buf_used, data, szval);                       \
                screen->write_buf_used += szval;                                                       \
            }                                                                                          \
            va_end(ap);                                                                                \
            shrink_write_buf(screen);                                                                  \
            if (screen->write_buf_used) wakeup_io_loop(self, false);                                   \
            screen_mutex(unlock, write);                                                               \
            break;                                                                                     \
        }                                                                                              \
    }                                                                                                  \
    children_mutex(unlock);

void
schedule_write_to_child_if_possible(id_type id, const char *data, size_t sz, bool *found, bool *too_much_data, size_t keep_space) {
    children_mutex(lock);
    ChildMonitor *self = the_monitor;
    *found = false;
    *too_much_data = false;
    size_t threshold = write_buf_flow_control_threshold > keep_space ? write_buf_flow_control_threshold - keep_space : 0;
    for (size_t i = 0; i < self->count; i++) {
        if (children[i].id == id) {
            Screen *screen = children[i].screen;
            screen_mutex(lock, write);
            // Out of memory is reported like being over the threshold: the caller waits and tries again
            if (ensure_write_buf_space(screen, sz, threshold) != WRITE_BUF_OK) {
                *too_much_data = true;
                screen_mutex(unlock, write);
                break;
            }
            *found = true;
            memcpy(screen->write_buf + screen->write_buf_used, data, sz);
            screen->write_buf_used += sz;
            shrink_write_buf(screen);
            if (screen->write_buf_used) wakeup_io_loop(self, false);
            screen_mutex(unlock, write);
            break;
        }
    }
    children_mutex(unlock);
}

bool
schedule_write_to_child(id_type id, unsigned num, ...) {
    va_list ap;
    bool out_of_memory = false, found = false;
#define get_next_arg(ap)             \
    data = va_arg(ap, const char *); \
    szval = va_arg(ap, size_t);
    schedule_write_to_child_generic(id, num, va_start, get_next_arg, va_end, found, out_of_memory);
#undef get_next_arg
    if (out_of_memory) log_error("Out of memory queueing data to be written to child with id: %llu, dropping it", id);
    return found;
}

bool
schedule_write_to_child_python(id_type id, const char *prefix, PyObject *ap, const char *suffix) {
    if (!PyTuple_Check(ap)) return false;
    bool has_prefix = prefix && prefix[0], has_suffix = suffix && suffix[0];
    const size_t extra = (has_prefix ? 1 : 0) + (has_suffix ? 1 : 0);
    size_t num = PyTuple_GET_SIZE(ap) + extra;
    Py_ssize_t pidx;
#define py_start(ap, num) pidx = 0;
#define py_end(ap) pidx = 0;
#define get_next_arg(ap)                                                                                     \
    {                                                                                                        \
        size_t pidxf = pidx++;                                                                               \
        if (pidxf == 0 && has_prefix) {                                                                      \
            data = prefix;                                                                                   \
            szval = strlen(prefix);                                                                          \
        } else {                                                                                             \
            if (has_prefix) pidxf--;                                                                         \
            if (has_suffix && pidxf >= (size_t)PyTuple_GET_SIZE(ap)) {                                       \
                data = suffix;                                                                               \
                szval = strlen(suffix);                                                                      \
            } else {                                                                                         \
                PyObject *t = PyTuple_GET_ITEM(ap, pidxf);                                                   \
                if (PyBytes_Check(t)) {                                                                      \
                    data = PyBytes_AS_STRING(t);                                                             \
                    szval = PyBytes_GET_SIZE(t);                                                             \
                } else {                                                                                     \
                    Py_ssize_t usz;                                                                          \
                    data = PyUnicode_AsUTF8AndSize(t, &usz);                                                 \
                    szval = usz;                                                                             \
                    if (!data) fatal("Failed to convert object to bytes in schedule_write_to_child_python"); \
                }                                                                                            \
            }                                                                                                \
        }                                                                                                    \
    }
    bool found = false, out_of_memory = false;
    schedule_write_to_child_generic(id, num, py_start, get_next_arg, py_end, found, out_of_memory);
    if (out_of_memory) log_error("Out of memory queueing data to be written to child with id: %llu, dropping it", id);
    return found;
#undef py_start
#undef py_end
#undef get_next_arg
}

static PyObject *
needs_write(ChildMonitor UNUSED *self, PyObject *args) {
#define needs_write_doc "needs_write(id, data) -> Queue data to be written to child."
    unsigned long id;
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "ky*", &id, &buf)) return NULL;
    if (schedule_write_to_child(id, 1, buf.buf, (size_t)buf.len)) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static PyObject *
shutdown_monitor(ChildMonitor *self, PyObject *a UNUSED) {
#define shutdown_monitor_doc "shutdown_monitor() -> Shutdown the monitor loop."
    self->shutting_down = true;
    wakeup_talk_loop(false);
    wakeup_io_loop(self, false);
    int ret = pthread_join(self->io_thread, NULL);
    if (ret != 0) return PyErr_Format(PyExc_OSError, "Failed to join() I/O thread with error: %s", strerror(ret));
    if (talk_thread_started) {
        ret = pthread_join(self->talk_thread, NULL);
        if (ret != 0) return PyErr_Format(PyExc_OSError, "Failed to join() talk thread with error: %s", strerror(ret));
    }
    talk_thread_started = false;
    Py_RETURN_NONE;
}

static PyObject *
set_wakeup_fd(ChildMonitor *self, PyObject *args) {
#define set_wakeup_fd_doc "set_wakeup_fd(fd) -> Set a fd to write to instead of waking the main loop (for benchmarking)."
    int fd;
    if (!PyArg_ParseTuple(args, "i", &fd)) return NULL;
    self->benchmark_wakeup_fd = fd;
    Py_RETURN_NONE;
}

static bool parse_input(ChildMonitor *self);

static PyObject *
parse_input_once(ChildMonitor *self, PyObject *a UNUSED) {
#define parse_input_once_doc "parse_input_once() -> Call parse_input once. Returns True if any input was consumed."
    if (parse_input(self)) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static bool
do_parse(ChildMonitor *self, Screen *screen, monotonic_t now, bool flush) {
    ParseData pd = {.dump_callback = self->dump_callback, .now = now};
    const bool had_url_highlight = screen->url_ranges.count > 0;
    self->parse_func(screen, &pd, flush);
    if (had_url_highlight && !screen->url_ranges.count) link_hover_cleared_by_output(screen);
    if (pd.input_read) {
        if (pd.write_space_created) wakeup_io_loop(self, false);
        if (screen->paused_rendering.expires_at) set_maximum_wait(MAX(0, screen->paused_rendering.expires_at - now));
    }
    // Bytes held until input_delay has passed. The I/O thread wakes the main loop when they are ready.
    screen->input_held = !pd.input_read && pd.has_pending_input;
    screen->input_parsed = pd.input_read;
    return pd.input_read;
}

static void
mark_os_windows_with_held_input(void) {
    // Painting an OS window whose only new output is held draws the old frame, and the
    // turn that parses the held bytes paints again because it ignores repaint_delay.
    // A window that parsed new output this turn is painted anyway.
    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        OSWindow *w = global_state.os_windows + i;
        bool held = false, parsed = false;
        for (size_t t = 0; t < w->num_tabs; t++) {
            Tab *tab = w->tabs + t;
            for (size_t c = 0; c < tab->num_windows; c++) {
                Screen *screen = tab->windows[c].render_data.screen;
                if (!screen) continue;
                held |= screen->input_held;
                parsed |= screen->input_parsed;
            }
        }
        w->input_held = held && !parsed;
    }
}

static bool
parse_input(ChildMonitor *self) {
    // Parse all available input that was read in the I/O thread.
    size_t count = 0, remove_count = 0;
    bool input_read = false, reload_config_called = false, input_held = false;
    monotonic_t now = monotonic();
    children_mutex(lock);
    while (remove_queue_count) {
        remove_queue_count--;
        remove_notify[remove_count] = remove_queue[remove_queue_count];
        INCREF_CHILD(remove_notify[remove_count]);
        remove_count++;
        FREE_CHILD(remove_queue[remove_queue_count]);
    }

    if (UNLIKELY(kill_signal_received || reload_config_signal_received)) {
        if (kill_signal_received) {
            global_state.quit_request = IMPERATIVE_CLOSE_REQUESTED;
            global_state.quit_cause = QUIT_CAUSE_SIGNAL;
            global_state.has_pending_closes = true;
            if (self->benchmark_wakeup_fd < 0) request_tick_callback();
            kill_signal_received = false;
        } else if (reload_config_signal_received) {
            reload_config_signal_received = false;
            reload_config_called = true;
        }
    }
    // Parse on signal turns too. The I/O thread does not wake the main loop again
    // for input it already woke it for, so skipping it here would stall it.
    count = self->count;
    for (size_t i = 0; i < count; i++) {
        scratch[i] = children[i];
        INCREF_CHILD(scratch[i]);
    }
    children_mutex(unlock);

    Message *msgs = NULL;
    size_t msgs_count = 0;
    talk_mutex(lock);
    if (UNLIKELY(self->messages_count)) {
        msgs = malloc(sizeof(Message) * self->messages_count);
        if (msgs) {
            memcpy(msgs, self->messages, sizeof(Message) * self->messages_count);
            msgs_count = self->messages_count;
        }
        memset(self->messages, 0, sizeof(Message) * self->messages_capacity);
        self->messages_count = 0;
    }
    talk_mutex(unlock);

    if (msgs_count) {
        for (size_t i = 0; i < msgs_count; i++) {
            Message *msg = msgs + i;
            PyObject *resp = NULL;
            if (msg->data) {
                resp = PyObject_CallMethod(
                    global_state.boss,
                    "peer_message_received",
                    "y#KO",
                    msg->data,
                    (int)msg->sz,
                    msg->peer_id,
                    msg->is_remote_control_peer ? Py_True : Py_False);
                free(msg->data);
                if (!resp) PyErr_Print();
            }
            if (resp) {
                if (PyBytes_Check(resp)) send_response_to_peer(msg->peer_id, PyBytes_AS_STRING(resp), PyBytes_GET_SIZE(resp), false);
                else if (resp == Py_None) send_response_to_peer(msg->peer_id, NULL, 0, false);
                else if (resp == Py_True) send_response_to_peer(msg->peer_id, NULL, 0, true);
                Py_CLEAR(resp);
            } else send_response_to_peer(msg->peer_id, NULL, 0, false);
        }
        free(msgs);
        msgs = NULL;
    }

    while (remove_count) {
        // must be done while no locks are held, since the locks are non-recursive and
        // the python function could call into other functions in this module
        remove_count--;
        if (remove_notify[remove_count].screen && do_parse(self, remove_notify[remove_count].screen, now, true)) input_read = true;
        PyObject *t = PyObject_CallFunction(
            self->death_notify,
            "kOiO",
            remove_notify[remove_count].id,
            remove_notify[remove_count].child_died ? Py_True : Py_False,
            remove_notify[remove_count].exit_status,
            remove_notify[remove_count].close_requested ? Py_True : Py_False);
        if (t == NULL) PyErr_Print();
        else Py_DECREF(t);
        FREE_CHILD(remove_notify[remove_count]);
    }

    for (size_t i = 0; i < count; i++) {
        if (!scratch[i].needs_removal) {
            if (do_parse(self, scratch[i].screen, now, false)) input_read = true;
            else if (scratch[i].screen->input_held) input_held = true;
        } else scratch[i].screen->input_held = scratch[i].screen->input_parsed = false;
        DECREF_CHILD(scratch[i]);
    }
    if (input_held) mark_os_windows_with_held_input();
    else
        for (size_t i = 0; i < global_state.num_os_windows; i++) global_state.os_windows[i].input_held = false;
    if (reload_config_called) { call_boss(load_config_file, NULL); }
    return input_read;
}

// The process the window runs, 0 when it runs none
pid_t
child_pid_for_window(id_type window_id) {
    pid_t ans = 0;
    if (!the_monitor || !window_id) return 0;
    children_mutex(lock);
    for (size_t i = 0; i < the_monitor->count && !ans; i++) {
        if (children[i].id == window_id) ans = children[i].pid;
    }
    for (size_t i = 0; i < add_queue_count && !ans; i++) {
        if (add_queue[i].id == window_id) ans = add_queue[i].pid;
    }
    children_mutex(unlock);
    return ans;
}

static bool
mark_child_for_close(ChildMonitor *self, id_type window_id) {
    bool found = false;
    children_mutex(lock);
    for (size_t i = 0; i < self->count; i++) {
        if (children[i].id == window_id) {
            children[i].needs_removal = true;
            children[i].close_requested = true;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < add_queue_count; i++) {
            if (add_queue[i].id == window_id) {
                add_queue[i].needs_removal = true;
                add_queue[i].close_requested = true;
                found = true;
                break;
            }
        }
    }
    children_mutex(unlock);
    wakeup_io_loop(self, false);
    return found;
}


static PyObject *
mark_for_close(ChildMonitor *self, PyObject *args) {
#define mark_for_close_doc "Mark a child to be removed from the child monitor"
    id_type window_id;
    if (!PyArg_ParseTuple(args, "K", &window_id)) return NULL;
    if (mark_child_for_close(self, window_id)) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static bool
pty_resize(int fd, struct winsize *dim) {
    while (true) {
        if (ioctl(fd, TIOCSWINSZ, dim) == -1) {
            if (errno == EINTR) continue;
            if (errno != EBADF && errno != ENOTTY) {
                log_error("Failed to resize tty associated with fd: %d with error: %s", fd, strerror(errno));
                return false;
            }
        }
        break;
    }
    return true;
}

static PyObject *
resize_pty(ChildMonitor *self, PyObject *args) {
#define resize_pty_doc "Resize the pty associated with the specified child"
    unsigned long window_id;
    struct winsize dim;
    int fd = -1;
    if (!PyArg_ParseTuple(args, "kHHHH", &window_id, &dim.ws_row, &dim.ws_col, &dim.ws_xpixel, &dim.ws_ypixel)) return NULL;
    children_mutex(lock);
#define FIND(queue, count)                   \
    {                                        \
        for (size_t i = 0; i < count; i++) { \
            if (queue[i].id == window_id) {  \
                fd = queue[i].fd;            \
                break;                       \
            }                                \
        }                                    \
    }
    FIND(children, self->count);
    if (fd == -1) FIND(add_queue, add_queue_count);
    if (fd != -1) {
        if (!pty_resize(fd, &dim)) PyErr_SetFromErrno(PyExc_OSError);
    } else log_error("Failed to send resize signal to child with id: %lu (children count: %zu) (add queue: %zu)", window_id, self->count, add_queue_count);
    children_mutex(unlock);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

bool
set_iutf8(int UNUSED fd, bool UNUSED on) {
#ifdef IUTF8
    struct termios attrs;
    if (tcgetattr(fd, &attrs) != 0) return false;
    if (on) attrs.c_iflag |= IUTF8;
    else attrs.c_iflag &= ~IUTF8;
    if (tcsetattr(fd, TCSANOW, &attrs) != 0) return false;
#endif
    return true;
}

static PyObject *
pyset_iutf8(ChildMonitor *self, PyObject *args) {
    id_type window_id;
    int on;
    PyObject *found = Py_False;
    if (!PyArg_ParseTuple(args, "Kp", &window_id, &on)) return NULL;
    children_mutex(lock);
    for (size_t i = 0; i < self->count; i++) {
        if (children[i].id == window_id) {
            found = Py_True;
            if (!set_iutf8(children[i].fd, on & 1)) PyErr_SetFromErrno(PyExc_OSError);
            break;
        }
    }
    children_mutex(unlock);
    if (PyErr_Occurred()) return NULL;
    Py_INCREF(found);
    return found;
}

#undef FREE_CHILD
#undef INCREF_CHILD
#undef DECREF_CHILD

static bool
cursor_needs_render(Window *w) {
    return memcmp(&w->render_data.screen->last_rendered.cursor, &w->render_data.screen->cursor_render_info, sizeof(CursorRenderInfo)) != 0;
}

static bool
collect_cursor_info(CursorRenderInfo *ans, Window *w, monotonic_t now, OSWindow *os_window) {
    WindowRenderData *rd = &w->render_data;
    const Cursor *cursor;
    if (screen_is_overlay_active(rd->screen)) {
        // Do not force the cursor to be visible here for the sake of some programs that prefer it hidden
        cursor = &(rd->screen->overlay_line.original_line.cursor);
        ans->x = rd->screen->overlay_line.cursor_x;
        ans->y = rd->screen->overlay_line.ynum;
    } else {
        cursor = rd->screen->paused_rendering.expires_at ? &rd->screen->paused_rendering.cursor : rd->screen->cursor;
        ans->x = cursor->x;
        ans->y = cursor->y;
    }
    ans->is_visible = false;
    ans->multicursor_count = 0;
    ans->cursor_opacity = 1;
    ans->text_blink_opacity = 1;
    // Use the scroll position of the rendered frame so the cursor matches the frozen text while rendering is paused
    if (!rendered_scrolled_by(rd->screen)) {
        ans->multicursor_count = screen_multi_cursor_count(rd->screen);
        ans->is_visible = screen_is_cursor_visible(rd->screen);
    }
    if (!ans->is_visible && ans->multicursor_count == 0 && !rd->screen->sgr_blink_was_used) return cursor_needs_render(w);
    monotonic_t time_since_start_blink = now - os_window->cursor_blink_zero_time;
    // Blinking ghosts on e-paper, where the cursor and blinking text stay shown
    const bool allow_blinking = OPT(cursor_blink_interval) > 0 && !os_window_is_eink(os_window);
    const bool blink_has_ceased = OPT(cursor_stop_blinking_after) != 0 && time_since_start_blink > OPT(cursor_stop_blinking_after);
    const bool cursor_blinking = !cursor->non_blinking && os_window->is_focused;
    float blink_opacity = 1.f;
    if (allow_blinking && !blink_has_ceased && (cursor_blinking || rd->screen->sgr_blink_was_used)) {
        if (animation_is_valid(OPT(animation.cursor))) {
            monotonic_t duration = OPT(cursor_blink_interval) * 2;
            monotonic_t time_into_cycle = time_since_start_blink % duration;
            double frac_into_cycle = (double)time_into_cycle / (double)duration;
            blink_opacity = (float)apply_easing_curve(OPT(animation.cursor), frac_into_cycle, duration);
            set_maximum_wait(ANIMATION_SAMPLE_WAIT);
        } else {
            monotonic_t n = time_since_start_blink / OPT(cursor_blink_interval);
            blink_opacity = 1 - n % 2;
            set_maximum_wait((n + 1) * OPT(cursor_blink_interval) - time_since_start_blink);
        }
    }
    ans->text_blink_opacity = blink_opacity;
    ans->cursor_opacity = cursor_blinking ? blink_opacity : 1.0f;
    ans->shape = cursor->shape ? cursor->shape : OPT(cursor_shape);
    ans->is_focused = os_window->is_focused;
    return cursor_needs_render(w);
}

static void
change_menubar_title(PyObject *title UNUSED) {
#ifdef __APPLE__
    static PyObject *current_title = NULL;
    if (title != current_title) {
        current_title = title;
        if (title && OPT(macos_show_window_title_in) & MENUBAR) update_menu_bar_title(title);
    }
#endif
}

static void
sync_ime_position_with_cursor(OSWindow *os_window, Window *w) {
    // Key presses update the IME position but cursor motion caused by child
    // output, such as the echo of text committed by the IME, does not. Without
    // this the next composition starts with a stale anchor and its candidate
    // window flashes at the old position before jumping to the cursor.
    Screen *screen = w->render_data.screen;
    OverlayLine *ol = &screen->overlay_line;
    if (screen_is_overlay_active(screen)) {
        // During a composition the IME is anchored to the pre-edit text, see
        // render_overlay_line(), so resync with the cursor once it ends.
        ol->last_ime_cursor.valid = false;
        return;
    }
    const index_type x = screen->cursor->x, y = screen->cursor->y;
    if (ol->last_ime_cursor.valid && ol->last_ime_cursor.x == x && ol->last_ime_cursor.y == y) return;
    ol->last_ime_cursor.x = x;
    ol->last_ime_cursor.y = y;
    ol->last_ime_cursor.valid = true;
    // Redundant updates are dropped by the platform backends
    update_ime_state_in_os_window(os_window, w, 0);
}

// A screen whose VAO could not be made for want of memory gets another try each time it is about to be drawn, and
// all of its data is sent to the GPU once it has one. Returns whether it has one.
static bool
retry_cell_vao(ssize_t *vao_idx, Screen *screen) {
    if (*vao_idx >= 0) return true;
    if (!ensure_cell_vao(vao_idx)) return false;
    screen->reload_all_gpu_data = true;
    return true;
}

static bool
prepare_to_render_os_window(
    OSWindow *os_window,
    monotonic_t now,
    unsigned int *active_window_id,
    color_type *active_window_bg,
    unsigned int *num_visible_windows,
    bool *all_windows_have_same_bg,
    bool scan_for_animated_images) {
#define TD os_window->tab_bar_render_data
    bool needs_render = os_window->needs_render;
    os_window->needs_render = false;
    bool was_previously_rendered_with_layers = os_window->needs_layers;
    Tab *tab = os_window->tabs + os_window->active_tab;
    double font_scale = 1.;
    for (unsigned i = 0; i < tab->num_windows; i++) {
        Window *w = tab->windows + i;
        Screen *screen = w->render_data.screen;
        if (w->visible && screen) font_scale = MAX(font_scale, MAX(screen->magnification.scale, screen->desktop_magnification));
    }
    update_magnified_fonts(os_window, font_scale);
    if (update_os_window_colorspace(os_window)) needs_render = true;
    // A window in a wide gamut is converted to it as a whole, after everything is drawn
    os_window->needs_layers =
        (!global_state.supports_framebuffer_srgb || effective_os_window_alpha(os_window) < 1.f || os_window->live_resize.in_progress ||
         (background_image_for_os_window(os_window) != NULL) || os_window->shader_anim.has_active_shaders || os_window->wide_gamut || os_window->spatial.mode ||
         os_window->output.encode_in_shader || output_dither_levels(os_window, NULL));
    if (TD.screen && os_window->num_tabs && !os_window->has_too_few_tabs) {
        if (!os_window->tab_bar_data_updated) {
            call_boss(update_tab_bar_data, "K", os_window->id);
            os_window->tab_bar_data_updated = true;
        }
        // we never render a cursor in the tab bar
        CursorRenderInfo *cri = &TD.screen->cursor_render_info;
        zero_at_ptr(cri);
        cri->x = TD.screen->cursor->x;
        cri->y = TD.screen->cursor->y;
        if (retry_cell_vao(&TD.vao_idx, TD.screen) && send_cell_data_to_gpu(TD.vao_idx, TD.screen, os_window)) needs_render = true;
        os_window->needs_layers = os_window->needs_layers || screen_needs_rendering_in_layers(os_window, NULL, TD.screen);
    }
    if (OPT(mouse_hide.hide_wait) > 0 && !is_mouse_hidden(os_window)) {
        if (now - os_window->last_mouse_activity_at >= OPT(mouse_hide.hide_wait)) hide_mouse(os_window);
        else set_maximum_wait(OPT(mouse_hide.hide_wait) - now + os_window->last_mouse_activity_at);
    }
    *active_window_bg = os_window_background(os_window);
    *all_windows_have_same_bg = true;
    *num_visible_windows = 0;
    color_type first_window_bg = 0;
    os_window->needs_layers = os_window->needs_layers || (os_window_has_cursor_trail(os_window) && tab->cursor_trail.needs_render);
    for (unsigned int i = 0; i < tab->num_windows; i++) {
        Window *w = tab->windows + i;
#define WD w->render_data
        if (w->visible && WD.screen) {
            screen_check_pause_rendering(WD.screen, now);
            *num_visible_windows += 1;
            color_type window_bg =
                colorprofile_to_color(
                    WD.screen->color_profile, WD.screen->color_profile->overridden.default_bg, WD.screen->color_profile->configured.default_bg)
                    .rgb;
            if (*num_visible_windows == 1) first_window_bg = window_bg;
            if (first_window_bg != window_bg) *all_windows_have_same_bg = false;
            if (w->last_drag_scroll_at > 0) {
                if (now - w->last_drag_scroll_at >= ms_to_monotonic_t(20ll)) {
                    if (drag_scroll(w, os_window)) {
                        w->last_drag_scroll_at = now;
                        set_maximum_wait(ms_to_monotonic_t(20ll));
                        needs_render = true;
                    } else w->last_drag_scroll_at = 0;
                } else set_maximum_wait(now - w->last_drag_scroll_at);
            }
            bool is_active_window = i == tab->active_window;
            if (is_active_window) {
                *active_window_id = w->id;
                if (collect_cursor_info(&WD.screen->cursor_render_info, w, now, os_window)) needs_render = true;
                WD.screen->cursor_render_info.is_focused = os_window->is_focused;
                if (global_state.ime_used && os_window->is_focused) sync_ime_position_with_cursor(os_window, w);
                set_os_window_title_from_window(w, os_window);
                *active_window_bg = window_bg;
                {
                    // Fire events for shell integration command and alternate screen changes
                    // in the active window that happened since the last check. This is just
                    // a few integer comparisons per frame. The check time includes the
                    // change times seen, as input can be parsed after now was captured.
                    const Screen *scr = WD.screen;
                    const monotonic_t since = os_window->screen_state_checked_at;
                    monotonic_t latest = now;
#define CHECK(field, event)                                                         \
    if (scr->field > since) {                                                       \
        os_window->shader_anim_event_registry |= (1u << SHADER_ANIM_EVENT_##event); \
        latest = MAX(latest, scr->field);                                           \
    }
                    CHECK(command_state.started_at, COMMAND_START);
                    CHECK(command_state.finished_at, COMMAND_FINISH);
                    CHECK(alternate_screen.entered_at, ALTERNATE_SCREEN_ENTER);
                    CHECK(alternate_screen.exited_at, ALTERNATE_SCREEN_EXIT);
#undef CHECK
                    os_window->screen_state_checked_at = MAX(since, latest);
                }
                if (os_window_has_cursor_trail(os_window)) {
                    if (os_window->last_active_tab != os_window->active_tab && os_window->last_active_tab < os_window->num_tabs) {
                        tab->cursor_trail = os_window->tabs[os_window->last_active_tab].cursor_trail;
                        tab->cursor_trail.needs_render = true;
                        tab->cursor_trail.updated_at = now;
                        os_window->cursor_blink_zero_time = now;
                    }
                    bool was_rendering = tab->cursor_trail.needs_render;
                    if (update_cursor_trail(&tab->cursor_trail, w, now, os_window)) {
                        needs_render = true;
                        // A max wait of zero causes key input processing to be
                        // slow so handle the case of OPT(repaint_delay) == 0, see https://github.com/kovidgoyal/kitty/pull/8066
                        set_maximum_wait(MAX(OPT(repaint_delay), ms_to_monotonic_t(1ll)));
                    } else if (OPT(cursor_trail) > now - WD.screen->cursor->position_changed_by_client_at) {
                        // If update_cursor_trail failed due to time threshold, the trail animation
                        // should be evaluated again shortly. Schedule next update when enough time
                        // has passed since the cursor was last moved.
                        set_maximum_wait(OPT(cursor_trail) - now + WD.screen->cursor->position_changed_by_client_at);
                    }
                    if (tab->cursor_trail.target_updated) os_window->shader_anim_event_registry |= (1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_MOVE);
                    if (was_rendering && !tab->cursor_trail.needs_render) os_window->shader_anim_event_registry |= (1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_STOP);
                }
            } else {
                if (WD.screen->cursor_render_info.render_even_when_unfocused) {
                    if (collect_cursor_info(&WD.screen->cursor_render_info, w, now, os_window)) needs_render = true;
                    WD.screen->cursor_render_info.is_focused = false;
                } else {
                    if (WD.screen->sgr_blink_was_used) {
                        if (collect_cursor_info(&WD.screen->cursor_render_info, w, now, os_window)) needs_render = true;
                        WD.screen->cursor_render_info.is_focused = false;
                    } else {
                        WD.screen->cursor_render_info.text_blink_opacity = 1;
                    }
                    WD.screen->cursor_render_info.cursor_opacity = 0;
                }
            }
            // Image animations ghost on e-paper, where images stay on the frame they show
            if (scan_for_animated_images && !os_window_is_eink(os_window)) {
                monotonic_t min_gap;
                if (scan_active_animations(WD.screen->grman, now, &min_gap, true)) needs_render = true;
                if (min_gap < MONOTONIC_T_MAX) {
                    global_state.check_for_active_animated_images = true;
                    set_maximum_wait(min_gap);
                }
            }
            if (retry_cell_vao(&WD.vao_idx, WD.screen) && send_cell_data_to_gpu(WD.vao_idx, WD.screen, os_window)) needs_render = true;
            if (WD.screen->magnification.scale > 1. || WD.screen->desktop_magnification > 1. || WD.screen->reported_raster_scale) {
                const monotonic_t wait = DISPLAY_POLL_INTERVAL - (now - WD.screen->raster_resize_at);
                if (WD.screen->raster_resize_at && wait > 0) set_maximum_wait(wait);
                else if (screen_update_reported_raster_scale(WD.screen)) {
                    WD.screen->raster_resize_at = now;
                    call_boss(on_raster_resolution_change, "K", w->id);
                }
            }
            // Must be after send_cell_data_to_gpu() as that updates the graphics layers, which
            // determine whether there are images that need rendering in layers. Otherwise, a newly
            // placed image is not drawn until something else causes a re-render, see #10875
            os_window->needs_layers = os_window->needs_layers || screen_needs_rendering_in_layers(os_window, w, WD.screen);
            // Also after send_cell_data_to_gpu(), as HDR images count as HDR content
            if (OPT(hdr) == HDR_AUTO && !os_window->hdr_content) os_window->hdr_content = screen_has_hdr_content(WD.screen);
            if (WD.screen->start_visual_bell_at | WD.screen->start_drag_overlay_at) needs_render = true;
            // An indeterminate progress bar is animated, except on e-paper
            if (OPT(progress_bar) != PROGRESS_BAR_HIDDEN && WD.screen->progress_state == PROGRESS_STATE_INDETERMINATE && !os_window_is_eink(os_window))
                needs_render = true;
            // Prepare window title bar screen data for GPU
            WindowRenderData *trd = &w->window_title_render_data;
            if (trd->screen && trd->geometry.bottom > trd->geometry.top && trd->geometry.right > trd->geometry.left) {
                trd->screen->cursor_render_info.is_visible = false;
                if (retry_cell_vao(&trd->vao_idx, trd->screen) && send_cell_data_to_gpu(trd->vao_idx, trd->screen, os_window)) needs_render = true;
            }
        }
    }
    if (OPT(cursor_stop_blinking_after) > 0) {
        monotonic_t time_since_last_activity = now - os_window->cursor_blink_zero_time;
        bool blink_has_ceased = time_since_last_activity > OPT(cursor_stop_blinking_after);
        if (blink_has_ceased && !os_window->user_is_idle) {
            os_window->user_is_idle = true;
            os_window->shader_anim_event_registry |= (1u << SHADER_ANIM_EVENT_USER_IDLE);
        } else if (!blink_has_ceased && OPT(cursor_blink_interval) <= 0 && os_window->shader_anim.has_active_shaders) {
            // cursor blinking is disabled so collect_cursor_info won't schedule a wakeup for
            // this deadline; do it here so user-idle fires on time
            set_maximum_wait(OPT(cursor_stop_blinking_after) - time_since_last_activity);
        }
    }
    {
        ShaderAnimEventMask events = os_window->shader_anim_event_registry;
        if (os_window->active_tab != os_window->last_active_tab)
            events |= (1u << SHADER_ANIM_EVENT_TAB_CHANGE) | (1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_IN) | (1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_OUT);
        if (*active_window_id && *active_window_id != os_window->last_active_window_id)
            events |= (1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_IN) | (1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_OUT);
        const ShaderAnimState before = os_window->shader_anim;
        // update_custom_shader_animations() consumes the user events
        const bool had_events = events || os_window->shader_user_events.pending;
        monotonic_t min_step = update_custom_shader_animations(events, now, os_window);
        os_window->shader_anim_event_registry = 0;
        if (os_window->shader_anim.has_active_shaders) os_window->needs_layers = true;
        if (custom_shader_needs_render(&before, &os_window->shader_anim, had_events, now)) needs_render = true;
        // Wake up for the next animation frame and for the moment a duration
        // bounded animation expires. Static groups (animation_step 0) have
        // min_step == MONOTONIC_T_MAX and so schedule no periodic wakeup.
        if (min_step < MONOTONIC_T_MAX) set_maximum_wait(min_step);
        if (os_window->shader_anim.next_end_at < MONOTONIC_T_MAX) set_maximum_wait(os_window->shader_anim.next_end_at - now);
    }
    return needs_render || was_previously_rendered_with_layers != os_window->needs_layers;
}

static void
thumbnail_callback(OSWindow *os_window) {
#define tc global_state.thumbnail_callback
    Region region = {.right = os_window->spatial.mode ? os_window->spatial.output_width : os_window->viewport_width,
        .bottom = os_window->spatial.mode ? os_window->spatial.output_height : os_window->viewport_height};
    if (tc.window && !os_window->spatial.mode) {
        Window *w = window_for_window_id(tc.window);
        if (!w) return;
        region.left = w->render_data.geometry.left;
        region.top = w->render_data.geometry.top;
        region.right = w->render_data.geometry.right;
        region.bottom = w->render_data.geometry.bottom;
    } else {
        if (!tc.include_tab_bar && !os_window->spatial.mode) {
            Region central = {0}, tab_bar = {0};
            os_window_regions(os_window, &central, &tab_bar);
            if (tab_bar.bottom > tab_bar.top) region = central;
        }
    }
    unsigned vw = region.right - region.left, vh = region.bottom - region.top;
    unsigned thumb_w, thumb_h;
    if (tc.no_scaling) {
        thumb_w = vw;
        thumb_h = vh;
    } else {
        thumb_w = (unsigned)(vw * tc.scale);
        thumb_h = (unsigned)(vh * tc.scale);
        if (thumb_w > tc.max_width) {
            thumb_w = tc.max_width;
            double scale = 300. / vw;
            thumb_h = (unsigned)(vh * scale + 0.5f);
        }
    }
    RAII_PyObject(pixels, PyBytes_FromStringAndSize(NULL, (Py_ssize_t)4 * thumb_w * thumb_h));
    if (pixels && global_state.boss) {
        take_screenshot_of_rectangular_region(os_window, region, (unsigned char *)PyBytes_AS_STRING(pixels), &thumb_w, &thumb_h, tc.no_scaling);
        _PyBytes_Resize(&pixels, (Py_ssize_t)4 * thumb_w * thumb_h);
        PyObject *r = PyObject_CallMethod(global_state.boss, tc.callback, "KKOII", os_window->id, tc.window, pixels, thumb_w, thumb_h);
        if (!r) PyErr_Print();
        else Py_DECREF(r);
    }
#undef tc
}

static void
render_prepared_os_window(
    OSWindow *os_window,
    unsigned int active_window_id,
    color_type active_window_bg,
    unsigned int num_visible_windows,
    bool all_windows_have_same_bg,
    monotonic_t now) {
    Tab *tab = os_window->tabs + os_window->active_tab;
    const unsigned eyes = os_window->spatial.mode ? 2 : 1;
    if (os_window->spatial.mode) {
        bind_framebuffer_for_output(0);
        blank_canvas(1.f, 0, false);
    }
    for (unsigned eye = 0; eye < eyes; eye++) {
    os_window->spatial.eye = eyes == 1 ? 0 : eye == 0 ? -1 : 1;
    set_gpu_viewport(os_window->viewport_width, os_window->viewport_height);
    setup_os_window_for_rendering(os_window, tab, NULL, true, now);
    BorderRects *br = &tab->border_rects;
    // A tab whose VAO could not be made for want of memory gets another try, its borders are drawn once it has one
    if (br->vao_idx < 0 && ensure_border_vao(&br->vao_idx)) br->is_dirty = true;
    draw_borders(br->vao_idx, br->num_border_rects, br->rect_buf, br->is_dirty, active_window_bg, num_visible_windows, all_windows_have_same_bg, os_window);
    br->is_dirty = false;
    if (TD.screen && os_window->num_tabs && !os_window->has_too_few_tabs) draw_cells(&TD, os_window, true, true, false, NULL, NULL, now);
    unsigned int num_of_visible_windows = 0;
    Window *active_window = NULL;
    for (unsigned int i = 0; i < tab->num_windows; i++) {
        if (tab->windows[i].visible) num_of_visible_windows++;
    }
    for (unsigned int i = 0; i < tab->num_windows; i++) {
        Window *w = tab->windows + i;
        if (w->visible && WD.screen) {
            bool is_active_window = i == tab->active_window;
            if (is_active_window) active_window = w;
            draw_cells(&WD, os_window, is_active_window, false, num_of_visible_windows == 1, w, NULL, now);
            if (WD.screen->start_visual_bell_at | WD.screen->start_drag_overlay_at) set_maximum_wait(ANIMATION_SAMPLE_WAIT);
            if (OPT(progress_bar) != PROGRESS_BAR_HIDDEN && WD.screen->progress_state == PROGRESS_STATE_INDETERMINATE) set_maximum_wait(ANIMATION_SAMPLE_WAIT);
            WindowRenderData *trd = &w->window_title_render_data;
            if (trd->screen && trd->geometry.right > trd->geometry.left && trd->geometry.bottom > trd->geometry.top)
                draw_cells(trd, os_window, i == tab->active_window, true, false, NULL, w, now);
        }
    }
    draw_rounded_borders(br, active_window_bg, num_visible_windows, all_windows_have_same_bg, os_window);
    setup_os_window_for_rendering(os_window, tab, active_window, false, now);
    }
    os_window->spatial.eye = 0;
    if (global_state.thumbnail_callback.os_window == os_window->id) {
        thumbnail_callback(os_window);
        global_state.thumbnail_callback.os_window = 0;
    }
    swap_window_buffers(os_window);
    // The frame was handed to the display system just now, not when this pass began drawing it
    monotonic_t handed_at = 0;
    for (unsigned int i = 0, period_us = 0; i < tab->num_windows; i++) {
        Window *w = tab->windows + i;
        if (w->visible && WD.screen && WD.screen->frame_feedback.pending) {
            if (!period_us) period_us = os_window_refresh_period_us(os_window);
            if (!handed_at) handed_at = monotonic();
            screen_frame_presented(WD.screen, period_us, handed_at);
        }
    }
    os_window->last_active_tab = os_window->active_tab;
    os_window->last_num_tabs = os_window->num_tabs;
    os_window->last_active_window_id = active_window_id;
    os_window->focused_at_last_render = os_window->is_focused;
    if (os_window->redraw_count) os_window->redraw_count--;
    if (USE_RENDER_FRAMES) request_frame_render(os_window);
#undef WD
#undef TD
}

static bool
no_render_frame_received_recently(OSWindow *w, monotonic_t now, monotonic_t max_wait) {
    bool ans = now - w->last_render_frame_received_at > max_wait;
    if (ans && global_state.debug_rendering) {
        if (global_state.is_wayland) {
            log_error("No render frame received in %.2f seconds", monotonic_t_to_s_double(max_wait));
        } else {
            log_error("No render frame received in %.2f seconds, re-requesting", monotonic_t_to_s_double(max_wait));
        }
    }
    return ans;
}

bool
render_os_window(OSWindow *w, monotonic_t now, bool scan_for_animated_images) {
    if (!w->num_tabs) return false;
    if (!should_os_window_be_rendered(w) && global_state.thumbnail_callback.os_window != w->id) {
        update_os_window_title(w);
        if (w->is_focused) change_menubar_title(w->window_title);
        return false;
    }
    const monotonic_t wait = eink_frame_wait(w, now);
    if (wait) {
        set_wakeup_wait(wait, EINK_FRAME_TIMER_TOLERANCE);
        return false;
    }
    if (!w->keep_rendering_till_swap && USE_RENDER_FRAMES && w->render_state != RENDER_FRAME_READY) {
        if (w->render_state == RENDER_FRAME_NOT_REQUESTED || no_render_frame_received_recently(w, now, ms_to_monotonic_t(250ll))) request_frame_render(w);
        if (w->id != global_state.thumbnail_callback.os_window) {
            // dont respect render frames soon after a resize on Wayland as they cause flicker because
            // we want to fill the newly resized buffer ASAP, not at compositors convenience
            if (!global_state.is_wayland || (monotonic() - w->viewport_resized_at) > s_double_to_monotonic_t(1)) { return false; }
        }
    }
    w->render_calls++;
    make_os_window_context_current(w);
    if (!w->linear_output_checked) check_os_window_output(w);
    const float previous_headroom = effective_hdr_headroom(w);
    w->hdr_enabled_by_default = glfwGetWindowHDRDefault(w->handle);
    w->hdr_headroom = glfwGetWindowHDRHeadroom(w->handle);
    w->hdr_content = OPT(cursor_brightness) > 1;
    bool needs_render = w->redraw_count > 0 || w->live_resize.in_progress || global_state.thumbnail_callback.os_window == w->id;
    if (effective_hdr_headroom(w) != previous_headroom) needs_render = true;
    if (w->viewport_size_dirty) {
        set_gpu_viewport(w->viewport_width, w->viewport_height);
        w->viewport_size_dirty = false;
        needs_render = true;
    }
    // The depth of the display is taken in like its size: when it changes, the window gets what suits the new display
    monotonic_t depth_wait;
    if (update_output_depth(w, glfwIsWindowOutputDeep(w->handle), now, &depth_wait)) needs_render = true;
    set_maximum_wait(depth_wait);
#ifdef __APPLE__
    // A floating point surface costs twice what an 8 bit one does, so a window has one only while it is of use
    const bool want_float = os_window_wants_float_surface(w);
    if (want_float != w->output.is_float && w->output.refused_switch != (want_float ? 2 : 1)) {
        if (glfwCocoaSetWindowFloatSurface && glfwCocoaSetWindowFloatSurface(w->handle, want_float)) {
            check_os_window_output(w);
            os_window_surface_switched(w);
            bind_shader_globals_to_current_context();
            set_gpu_viewport(w->viewport_width, w->viewport_height);
            w->viewport_size_dirty = false;
            w->output.refused_switch = 0;
            debug_rendering("The window %llu now has %s surface\n", w->id, want_float ? "a floating point" : "an 8 bit");
        } else w->output.refused_switch = want_float ? 2 : 1;
        needs_render = true;
    }
#endif
    unsigned int active_window_id = 0, num_visible_windows = 0;
    bool all_windows_have_same_bg;
    color_type active_window_bg = 0;
    if (!w->fonts_data) {
        log_error("No fonts data found for window id: %llu", w->id);
        return false;
    }
    if (prepare_to_render_os_window(w, now, &active_window_id, &active_window_bg, &num_visible_windows, &all_windows_have_same_bg, scan_for_animated_images))
        needs_render = true;
    const bool hdr_active = hdr_should_be_active(w);
    if (hdr_active != w->hdr_active) {
        w->hdr_active = hdr_active;
        glfwSetWindowHDRActive(w->handle, hdr_active);
        needs_render = true;
    }
    // Where SDR white lies in the framebuffer can depend on how bright the contents get, so a change of it needs a new frame
    const float hdr_white_level = glfwSetWindowHDRRange(w->handle, hdr_active ? effective_hdr_headroom(w) : 1.f);
    if (hdr_white_level != w->hdr_white_level) {
        w->hdr_white_level = hdr_white_level;
        needs_render = true;
    }
    if (w->last_active_window_id != active_window_id || w->last_active_tab != w->active_tab || w->focused_at_last_render != w->is_focused) needs_render = true;
    if (w->render_calls < 3 && background_image_for_os_window(w) != NULL) needs_render = true;
    if (needs_render) render_prepared_os_window(w, active_window_id, active_window_bg, num_visible_windows, all_windows_have_same_bg, now);
    if (w->is_focused) change_menubar_title(w->window_title);
    return needs_render;
}

// Returns the nominal wait for the next display poll. Output transitions can
// still schedule a stricter deadline through set_maximum_wait().
static monotonic_t
poll_display_info(OSWindow *os_window, monotonic_t now) {
    // Output color changes invalidate application rendering just as a resize
    // does, including for applications that did not subscribe to OSC events.
    const monotonic_t interval = DISPLAY_POLL_INTERVAL;
    bool wanted = false;
    for (unsigned t = 0; t < os_window->num_tabs && !wanted; t++) {
        Tab *tab = os_window->tabs + t;
        for (unsigned i = 0; i < tab->num_windows && !wanted; i++) wanted = tab->windows[i].render_data.screen != NULL;
    }
    if (!wanted) return -1;
    if (now - os_window->display_info_at < interval) return interval - (now - os_window->display_info_at);
    // The desktop magnifier is shared across windows. Sample it once on the
    // existing display wakeup and consume the result in every pane.
    static monotonic_t magnification_sampled_at;
    static double desktop_magnification = 1.;
    if (global_state.boss && (!magnification_sampled_at || now - magnification_sampled_at >= interval)) {
        magnification_sampled_at = now;
        RAII_PyObject(result, PyObject_CallMethod(global_state.boss, "desktop_zoom_factor", NULL));
        if (result) {
            double factor = PyFloat_AsDouble(result);
            if (isfinite(factor) && factor >= 1.) desktop_magnification = factor;
        }
        if (PyErr_Occurred()) PyErr_Print();
    }
    bool zoom_changed = false;
    for (unsigned t = 0; t < os_window->num_tabs; t++) {
        Tab *tab = os_window->tabs + t;
        for (unsigned i = 0; i < tab->num_windows; i++) {
            Screen *screen = tab->windows[i].render_data.screen;
            if (screen && screen->desktop_magnification != desktop_magnification) {
                screen->desktop_magnification = desktop_magnification;
                zoom_changed = true;
            }
        }
    }
    if (zoom_changed) {
        os_window->needs_render = true;
        set_maximum_wait(0);
    }
    // The depth of the display can change without anything being drawn
    if (os_window->handle) {
        monotonic_t depth_wait;
        const bool depth_changed = update_output_depth(os_window, glfwIsWindowOutputDeep(os_window->handle), now, &depth_wait);
        set_maximum_wait(depth_wait);
        if (depth_changed && os_window->display_info_at) {
            // The window takes in the change when it is next drawn, perhaps with another surface, and that is what
            // the next report is of, so that programs get one event with the outcome
            os_window->needs_render = true;
            os_window->display_info_at = now;
            return interval;
        }
    }
    update_display_info(os_window, now, false);
    return interval;
}

// Takes in what is now known of the display of the window, telling the programs that asked when it changed. explicit is
// for a change the user made, such as of eink_mode, which is acted on at once rather than once it has lasted.
void
update_display_info(OSWindow *os_window, monotonic_t now, bool explicit) {
    DisplayInfo d, *o = &os_window->display_info;
    os_window_display_info(os_window, &d);
    log_window_monitor(os_window, &d);
    // What Silkie does for an e-paper display follows the window from monitor to monitor
    monotonic_t eink_wait;
    const bool eink_changed = update_eink_state(os_window, &d, explicit, now, &eink_wait);
    set_maximum_wait(eink_wait);
    if (eink_changed) {
        debug_rendering("OS window %llu is now treated as on %s%s\n", os_window->id, os_window_is_eink(os_window) ? "e-paper" : "an ordinary display",
            os_window_is_gray(os_window) ? " that shows only gray" : "");
        // No dithering on e-paper, and the cursor and text that were blinking stay shown
        os_window->needs_render = true;
        d.output_dither = output_dither_bits(os_window);
    }
    if (!explicit && os_window->eink.known && os_window->eink.seen != os_window->eink.acted) {
        // A move that changes what is done is reported once it is acted on, with what it changed, so that programs get
        // one event with the outcome, as for a change of depth
        os_window->display_info_at = now;
        return;
    }
    const bool color_changed = os_window->display_info_at && (d.kind != o->kind || d.color != o->color || d.gamut != o->gamut || d.bits != o->bits ||
        d.red_bits != o->red_bits || d.green_bits != o->green_bits || d.blue_bits != o->blue_bits || d.gray_bits != o->gray_bits ||
        strcmp(d.precision, o->precision) || d.headroom != o->headroom || d.output_bits != o->output_bits || strcmp(d.output_type, o->output_type) ||
        d.output_dither != o->output_dither);
    const bool changed = color_changed || (os_window->display_info_at && (d.period_us != o->period_us || d.scale != o->scale || d.ppi != o->ppi ||
        d.monitor != o->monitor || d.appearance != o->appearance || d.eink_scheme != o->eink_scheme));
    *o = d;
    os_window->display_info_at = now;
    if (changed) {
        for (unsigned t = 0; t < os_window->num_tabs; t++) {
            Tab *tab = os_window->tabs + t;
            for (unsigned i = 0; i < tab->num_windows; i++) if (tab->windows[i].render_data.screen) screen_display_changed(tab->windows[i].render_data.screen, &d);
        }
    }
    if (eink_changed) call_boss(on_os_window_eink_change, "KOO", os_window->id, os_window_is_eink(os_window) ? Py_True : Py_False,
        os_window_is_gray(os_window) ? Py_True : Py_False);
    if (color_changed) call_boss(on_display_color_change, "K", os_window->id);
}

static monotonic_t
render(monotonic_t now, bool input_read) {
    EVDBG("input_read: %d, check_for_active_animated_images: %d\n", input_read, global_state.check_for_active_animated_images);
    static monotonic_t last_render_at = MONOTONIC_T_MIN;
    monotonic_t time_since_last_render = last_render_at == MONOTONIC_T_MIN ? OPT(repaint_delay) : now - last_render_at;
    if (!input_read && time_since_last_render < OPT(repaint_delay) && !global_state.thumbnail_callback.os_window) {
        set_maximum_wait(OPT(repaint_delay) - time_since_last_render);
        return -1;
    }

    const bool scan_for_animated_images = global_state.check_for_active_animated_images;
    global_state.check_for_active_animated_images = false;
    call_boss(cache_process_data, "O", Py_True);
    bool waiting_for_frame = false;
    monotonic_t display_poll_wait = -1;

    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        OSWindow *w = global_state.os_windows + i;
#ifdef __APPLE__
        // E-paper also uses the main timer to paint resize events deferred by the frame cap.
        if (w->live_resize.in_progress && !os_window_is_eink(w)) continue;
#endif
        // Held bytes are not on this window yet. Painting it draws the old frame, and the
        // turn that parses them paints again because it ignores repaint_delay.
        if (w->input_held && w->id != global_state.thumbnail_callback.os_window) {
            if (scan_for_animated_images) global_state.check_for_active_animated_images = true;
            continue;
        }
        // A window waiting for a render frame cannot paint now, so it must not delay the
        // paint when the frame arrives, otherwise the next output paints it mid-burst
        if (!w->keep_rendering_till_swap && USE_RENDER_FRAMES && w->render_state != RENDER_FRAME_READY && should_os_window_be_rendered(w))
            waiting_for_frame = true;
        if (!render_os_window(w, now, scan_for_animated_images)) {
            // since we didn't scan the window for animations, force a rescan on next wakeup/render frame
            if (scan_for_animated_images) global_state.check_for_active_animated_images = true;
        }
        const monotonic_t poll_wait = poll_display_info(w, now);
        if (poll_wait >= 0 && (display_poll_wait < 0 || poll_wait < display_poll_wait)) display_poll_wait = poll_wait;
        if (w->keep_rendering_till_swap) {
            debug_rendering("Re-rendering window %llu on the %u attempt since swap did not happen\n", w->id, w->keep_rendering_till_swap);
            set_maximum_wait(OPT(repaint_delay));
            w->needs_render = true;
            w->keep_rendering_till_swap--;
        }
    }
    if (!waiting_for_frame) last_render_at = now;
    call_boss(cache_process_data, "O", Py_False);
    return display_poll_wait;
#undef TD
}


typedef struct {
    int fd;
    uint8_t *buf;
    size_t sz;
} ThreadWriteData;

static ThreadWriteData *
alloc_twd(size_t sz) {
    ThreadWriteData *data = calloc(1, sizeof(ThreadWriteData));
    if (data != NULL) {
        data->sz = sz;
        data->buf = malloc(sz);
        if (data->buf == NULL) {
            free(data);
            data = NULL;
        }
    }
    return data;
}

static void
free_twd(ThreadWriteData *x) {
    if (x != NULL) free(x->buf);
    free(x);
}

static PyObject *
sig_queue(PyObject *self UNUSED, PyObject *args) {
    int pid, signal, value;
    if (!PyArg_ParseTuple(args, "iii", &pid, &signal, &value)) return NULL;
#ifdef NO_SIGQUEUE
    if (kill(pid, signal) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
#else
    union sigval v;
    v.sival_int = value;
    if (sigqueue(pid, signal, v) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
#endif
    Py_RETURN_NONE;
}

// With the children lock held
static bool
add_monitored_pid(pid_t pid) {
    const size_t total = monitored_pids_count + reaped_pids_count;
    if (total == SIZE_MAX || !GROW(monitored_pids, monitored_pids_count + 1) || !GROW(reaped_pids, total + 1)) return false;
    monitored_pids[monitored_pids_count++] = pid;
    return true;
}

// With the children lock held
static void
record_reaped_pid(pid_t pid, int status) {
    for (ssize_t i = monitored_pids_count - 1; i >= 0; i--) {
        if (pid == monitored_pids[i]) {
            // room for it was reserved when it was monitored
            if (reaped_pids_count < reaped_pids_capacity) {
                reaped_pids[reaped_pids_count].status = status;
                reaped_pids[reaped_pids_count++].pid = pid;
            }
            remove_i_from_array(monitored_pids, (size_t)i, monitored_pids_count);
        }
    }
}

static PyObject *
monitor_pid(PyObject *self UNUSED, PyObject *args) {
    int pid;
    bool ok = true;
    if (!PyArg_ParseTuple(args, "i", &pid)) return NULL;
    children_mutex(lock);
    if (!add_monitored_pid(pid)) {
        PyErr_SetString(PyExc_MemoryError, "Out of memory monitoring a pid");
        ok = false;
    }
    children_mutex(unlock);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

static void
report_reaped_pids(void) {
    // All of them are reported, the boss is called without the lock held. Without memory to copy them they are left
    // for the next time.
    ReapedPID *pids = NULL;
    size_t count = 0;
    children_mutex(lock);
    if (reaped_pids_count) {
        pids = malloc(reaped_pids_count * sizeof(ReapedPID));
        if (pids) {
            memcpy(pids, reaped_pids, reaped_pids_count * sizeof(ReapedPID));
            count = reaped_pids_count;
            reaped_pids_count = 0;
        }
    }
    children_mutex(unlock);
    for (size_t n = 0; n < count; n++) { call_boss(on_monitored_pid_death, "li", (long)pids[n].pid, pids[n].status); }
    free(pids);
}

static void *
thread_write(void *x) {
    ThreadWriteData *data = (ThreadWriteData *)x;
    set_thread_name("KittyWriteStdin");
    int flags = fcntl(data->fd, F_GETFL, 0);
    if (flags == -1) {
        free_twd(data);
        return 0;
    }
    flags &= ~O_NONBLOCK;
    fcntl(data->fd, F_SETFL, flags);
    size_t pos = 0;
    while (pos < data->sz) {
        errno = 0;
        ssize_t nbytes = write(data->fd, data->buf + pos, data->sz - pos);
        if (nbytes < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        if (nbytes == 0) break;
        pos += nbytes;
    }
    if (pos < data->sz) { log_error("Failed to write all data to STDIN of child process with error: %s", strerror(errno)); }
    safe_close(data->fd, __FILE__, __LINE__);
    free_twd(data);
    return 0;
}

PyObject *
cm_thread_write(PyObject UNUSED *self, PyObject *args) {
    static pthread_t thread;
    int fd;
    Py_ssize_t sz;
    const char *buf;
    if (!PyArg_ParseTuple(args, "is#", &fd, &buf, &sz)) return NULL;
    ThreadWriteData *data = alloc_twd(sz);
    if (data == NULL) return PyErr_NoMemory();
    data->fd = fd;
    memcpy(data->buf, buf, data->sz);
    int ret = pthread_create(&thread, NULL, thread_write, data);
    if (ret != 0) {
        safe_close(fd, __FILE__, __LINE__);
        free_twd(data);
        return PyErr_Format(PyExc_OSError, "Failed to start write thread with error: %s", strerror(ret));
    }
    pthread_detach(thread);
    Py_RETURN_NONE;
}

static void
python_timer_callback(id_type timer_id, void *data) {
    PyObject *callback = (PyObject *)data;
    unsigned long long id = timer_id;
    PyObject *ret = PyObject_CallFunction(callback, "K", id);
    if (ret == NULL) PyErr_Print();
    else Py_DECREF(ret);
    // Timers are dispatched outside the main loop tick, so ensure any state
    // changed by the callback is processed and rendered
    request_tick_callback();
}

static void
python_timer_cleanup(id_type timer_id UNUSED, void *data) {
    if (data) Py_DECREF((PyObject *)data);
}

static PyObject *
add_python_timer(PyObject *self UNUSED, PyObject *args) {
    PyObject *callback;
    double interval;
    int repeats = 1;
    if (!PyArg_ParseTuple(args, "Od|p", &callback, &interval, &repeats)) return NULL;
    // The timer owns a reference to callback, released by python_timer_cleanup
    Py_INCREF(callback);
    unsigned long long timer_id =
        add_main_loop_timer(s_double_to_monotonic_t(interval), repeats ? true : false, python_timer_callback, callback, python_timer_cleanup);
    // On failure the cleanup function is never called, so release the reference here
    if (!timer_id) Py_DECREF(callback);
    return Py_BuildValue("K", timer_id);
}

static PyObject *
remove_python_timer(PyObject *self UNUSED, PyObject *args) {
    unsigned long long timer_id;
    if (!PyArg_ParseTuple(args, "K", &timer_id)) return NULL;
    remove_main_loop_timer(timer_id);
    Py_RETURN_NONE;
}


static void
process_pending_resizes(monotonic_t now) {
    global_state.has_pending_resizes = false;
    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        OSWindow *w = global_state.os_windows + i;
        if (w->live_resize.in_progress) {
            bool update_viewport = false;
            if (w->live_resize.from_os_notification) {
                if (w->live_resize.os_says_resize_complete) update_viewport = true;
                else {
                    // prevent a "hang" if the OS never sends a resize complete event
                    // also reflow the screen when the user pauses resizing so the user can see what the resized
                    // screen will look like.
                    if ((now - w->live_resize.last_resize_event_at) > OPT(resize_debounce_time).on_pause) update_viewport = true;
                    else {
                        global_state.has_pending_resizes = true;
                        set_maximum_wait(s_double_to_monotonic_t(0.05));
                    }
                }
            } else {
                monotonic_t debounce_time = OPT(resize_debounce_time).on_end;
                // if more than one resize event has occurred, wait at least 0.2 secs
                // before repainting, to avoid rapid transitions between the cells banner
                // and the normal screen
                if (now - w->live_resize.last_resize_event_at >= debounce_time) update_viewport = true;
                else {
                    global_state.has_pending_resizes = true;
                    set_maximum_wait(debounce_time - now + w->live_resize.last_resize_event_at);
                }
            }
            if (update_viewport) {
                update_os_window_viewport(w, true);
                change_live_resize_state(w, false);
                zero_at_ptr(&w->live_resize);
                // because the window size should be hidden even if update_os_window_viewport does nothing
                // On Wayland some compositors require two redraws after a
                // resize to actually render correctly (Run kitty -1 --wait-for-os-window-close in sway to reproduce)
                w->redraw_count = global_state.is_wayland ? 2 : 1;
            }
        }
    }
}

static void
close_os_window(ChildMonitor *self, OSWindow *os_window) {
    int w = os_window->window_width, h = os_window->window_height;
    if (os_window->before_fullscreen.is_set && is_os_window_fullscreen(os_window)) {
        w = os_window->before_fullscreen.w;
        h = os_window->before_fullscreen.h;
    }
    int x = 0, y = 0;
    if (os_window->handle && !global_state.is_wayland) glfwGetWindowPos(os_window->handle, &x, &y);
    bool is_layer_shell = os_window->is_layer_shell;
    bool was_maximized = false;
    if (os_window->handle && !is_layer_shell) {
        if (os_window->before_fullscreen.is_set && is_os_window_fullscreen(os_window)) {
            was_maximized = os_window->before_fullscreen.was_maximized;
        } else {
            was_maximized = glfwGetWindowAttrib(os_window->handle, GLFW_MAXIMIZED) != 0;
        }
    }
    destroy_os_window(os_window);
    call_boss(on_os_window_closed, "KiiiiOO", os_window->id, x, y, w, h, was_maximized ? Py_True : Py_False, is_layer_shell ? Py_True : Py_False);
    for (size_t t = 0; t < os_window->num_tabs; t++) {
        Tab *tab = os_window->tabs + t;
        for (size_t w = 0; w < tab->num_windows; w++) mark_child_for_close(self, tab->windows[w].id);
    }
    remove_os_window(os_window->id);
}

static bool
process_pending_closes(ChildMonitor *self) {
    if (global_state.quit_request == CONFIRMABLE_CLOSE_REQUESTED) { call_boss(quit, ""); }
    if (global_state.quit_request == IMPERATIVE_CLOSE_REQUESTED) {
        for (size_t w = 0; w < global_state.num_os_windows; w++) global_state.os_windows[w].close_request = IMPERATIVE_CLOSE_REQUESTED;
    }
    bool has_open_windows = false;
    for (size_t w = global_state.num_os_windows; w > 0; w--) {
        OSWindow *os_window = global_state.os_windows + w - 1;
        switch (os_window->close_request) {
            case NO_CLOSE_REQUESTED: has_open_windows = true; break;
            case CONFIRMABLE_CLOSE_REQUESTED:
                os_window->close_request = CLOSE_BEING_CONFIRMED;
                call_boss(confirm_os_window_close, "K", os_window->id);
                if (os_window->close_request == IMPERATIVE_CLOSE_REQUESTED) {
                    close_os_window(self, os_window);
                } else has_open_windows = true;
                break;
            case CLOSE_BEING_CONFIRMED: has_open_windows = true; break;
            case IMPERATIVE_CLOSE_REQUESTED: close_os_window(self, os_window); break;
        }
    }
    global_state.has_pending_closes = false;
#ifdef __APPLE__
    if (!OPT(macos_quit_when_last_window_closed)) {
        if (!has_open_windows && global_state.quit_request != IMPERATIVE_CLOSE_REQUESTED) has_open_windows = true;
    }
#endif
    return !has_open_windows;
}

#ifdef __APPLE__
// If we create new OS windows during wait_events(), using global menu actions
// via the mouse causes a crash because of the way autorelease pools work in
// glfw/cocoa. So we use a flag instead.
static bool cocoa_pending_actions[NUM_COCOA_PENDING_ACTIONS] = {0};
static bool has_cocoa_pending_actions = false;
typedef struct cocoa_list {
    char **items;
    size_t count, capacity;
} cocoa_list;
typedef struct {
    char *wd;
    cocoa_list open_urls, untracked_notifications;
} CocoaPendingActionsData;
static CocoaPendingActionsData cocoa_pending_actions_data = {0};

static void
cocoa_append_to_pending_list(cocoa_list *array, const char *item) {
    ensure_space_for(array, items, char *, array->count + 1, capacity, 8, false);
    array->items[array->count++] = strdup(item);
}

static void
cocoa_free_pending_list(cocoa_list *array) {
    for (size_t i = 0; i < array->count; i++) free(array->items[i]);
    free(array->items);
    zero_at_ptr(array);
}

static void
cocoa_free_actions_data(void) {
    if (cocoa_pending_actions_data.wd) {
        free(cocoa_pending_actions_data.wd);
        cocoa_pending_actions_data.wd = NULL;
    }
    cocoa_free_pending_list(&cocoa_pending_actions_data.open_urls);
    cocoa_free_pending_list(&cocoa_pending_actions_data.untracked_notifications);
}

void
set_cocoa_pending_action(CocoaPendingAction action, const char *data) {
    if (data) {
        switch (action) {
            case LAUNCH_URLS: cocoa_append_to_pending_list(&cocoa_pending_actions_data.open_urls, data); break;
            case COCOA_NOTIFICATION_UNTRACKED: cocoa_append_to_pending_list(&cocoa_pending_actions_data.untracked_notifications, data); break;
            default:
                if (cocoa_pending_actions_data.wd) free(cocoa_pending_actions_data.wd);
                cocoa_pending_actions_data.wd = strdup(data);
                break;
        }
    }
    cocoa_pending_actions[action] = true;
    has_cocoa_pending_actions = true;
    // The main loop may be blocking on the event queue, if e.g. unfocused.
    // Unjam it so the pending action is processed right now.
    wakeup_main_loop();
}

static void
process_cocoa_pending_actions(void) {
    if (cocoa_pending_actions[PREFERENCES_WINDOW]) { call_boss(edit_config_file, NULL); }
    if (cocoa_pending_actions[OPEN_SETTINGS]) { call_boss(open_settings, NULL); }
    if (cocoa_pending_actions[NEW_OS_WINDOW]) { call_boss(new_os_window, NULL); }
    if (cocoa_pending_actions[CLOSE_OS_WINDOW]) { call_boss(close_os_window, NULL); }
    if (cocoa_pending_actions[CLOSE_TAB]) { call_boss(close_tab, NULL); }
    if (cocoa_pending_actions[NEW_TAB]) { call_boss(new_tab, NULL); }
    if (cocoa_pending_actions[NEXT_TAB]) { call_boss(next_tab, NULL); }
    if (cocoa_pending_actions[PREVIOUS_TAB]) { call_boss(previous_tab, NULL); }
    if (cocoa_pending_actions[DETACH_TAB]) { call_boss(detach_tab, NULL); }
    if (cocoa_pending_actions[NEW_WINDOW]) { call_boss(new_window, NULL); }
    if (cocoa_pending_actions[CLOSE_WINDOW]) { call_boss(close_window, NULL); }
    if (cocoa_pending_actions[KILL_WINDOW]) { call_boss(kill_window, NULL); }
    if (cocoa_pending_actions[RESET_TERMINAL]) { call_boss(clear_terminal, "sO", "reset", Py_True); }
    if (cocoa_pending_actions[CLEAR_TERMINAL_AND_SCROLLBACK]) { call_boss(clear_terminal, "sO", "to_cursor", Py_True); }
    if (cocoa_pending_actions[CLEAR_SCROLLBACK]) { call_boss(clear_terminal, "sO", "scrollback", Py_True); }
    if (cocoa_pending_actions[CLEAR_SCREEN]) { call_boss(clear_terminal, "sO", "to_cursor_scroll", Py_True); }
    if (cocoa_pending_actions[CLEAR_LAST_COMMAND]) { call_boss(clear_terminal, "sO", "last_command", Py_True); }
    if (cocoa_pending_actions[RELOAD_CONFIG]) { call_boss(load_config_file, NULL); }
    if (cocoa_pending_actions[TOGGLE_MACOS_SECURE_KEYBOARD_ENTRY]) { call_boss(toggle_macos_secure_keyboard_entry, NULL); }
    if (cocoa_pending_actions[MACOS_CYCLE_THROUGH_OS_WINDOWS]) { call_boss(macos_cycle_through_os_windows, NULL); }
    if (cocoa_pending_actions[MACOS_CYCLE_THROUGH_OS_WINDOWS_BACKWARDS]) { call_boss(macos_cycle_through_os_windows_backwards, NULL); }
    if (cocoa_pending_actions[SEARCH_SCROLLBACK]) { call_boss(search_scrollback_in_active, NULL); }
    if (cocoa_pending_actions[TOGGLE_FULLSCREEN]) { call_boss(toggle_fullscreen, NULL); }
    if (cocoa_pending_actions[OPEN_KITTY_WEBSITE]) { call_boss(open_kitty_website, NULL); }
    if (cocoa_pending_actions[HIDE]) { call_boss(hide_macos_app, NULL); }
    if (cocoa_pending_actions[HIDE_OTHERS]) { call_boss(hide_macos_other_apps, NULL); }
    if (cocoa_pending_actions[MINIMIZE]) { call_boss(minimize_macos_window, NULL); }
    if (cocoa_pending_actions[QUIT]) { call_boss(quit, NULL); }
    if (cocoa_pending_actions[PASTE_FROM_CLIPBOARD]) { call_boss(paste_from_clipboard, NULL); }
    if (cocoa_pending_actions[COPY_OR_NOOP]) { call_boss(copy_or_noop, NULL); }
    if (cocoa_pending_actions_data.wd) {
        if (cocoa_pending_actions[NEW_OS_WINDOW_WITH_WD]) { call_boss(new_os_window_with_wd, "sO", cocoa_pending_actions_data.wd, Py_True); }
        if (cocoa_pending_actions[NEW_TAB_WITH_WD]) { call_boss(new_tab_with_wd, "sO", cocoa_pending_actions_data.wd, Py_True); }
        if (cocoa_pending_actions[USER_MENU_ACTION]) { call_boss(user_menu_action, "s", cocoa_pending_actions_data.wd); }
        free(cocoa_pending_actions_data.wd);
        cocoa_pending_actions_data.wd = NULL;
    }
    for (unsigned cpa = 0; cpa < cocoa_pending_actions_data.open_urls.count; cpa++) {
        if (cocoa_pending_actions_data.open_urls.items[cpa]) {
            call_boss(launch_urls, "s", cocoa_pending_actions_data.open_urls.items[cpa]);
            free(cocoa_pending_actions_data.open_urls.items[cpa]);
            cocoa_pending_actions_data.open_urls.items[cpa] = NULL;
        }
    }
    cocoa_pending_actions_data.open_urls.count = 0;

    for (unsigned cpa = 0; cpa < cocoa_pending_actions_data.untracked_notifications.count; cpa++) {
        if (cocoa_pending_actions_data.untracked_notifications.items[cpa]) {
            cocoa_report_live_notifications(cocoa_pending_actions_data.untracked_notifications.items[cpa]);
            free(cocoa_pending_actions_data.untracked_notifications.items[cpa]);
            cocoa_pending_actions_data.untracked_notifications.items[cpa] = NULL;
        }
    }
    cocoa_pending_actions_data.untracked_notifications.count = 0;


    memset(cocoa_pending_actions, 0, sizeof(cocoa_pending_actions));
    has_cocoa_pending_actions = false;
}
#endif

static void process_global_state(void *data);

static void
do_state_check(id_type timer_id UNUSED, void *data) {
    EVDBG("State check timer fired");
    process_global_state(data);
}

static id_type state_check_timer = 0;

static void
process_global_state(void *data) {
    EVDBG("Processing global state");
    ChildMonitor *self = data;
    wakeup_wait = wakeup_deadline = -1;
    bool input_read = false;

    monotonic_t now = monotonic();
    if (global_state.has_pending_resizes) {
        process_pending_resizes(now);
        input_read = true;
    }
    if (parse_input(self)) input_read = true;
    // take in what the scrollback compressor thread has finished
    historybuf_retire_all();
    // Find: search the scrollback a slice at a time, see search.c
    const monotonic_t search_wait = screen_search_tick_all(now, ms_to_monotonic_t(8));
    if (search_wait >= 0) set_maximum_wait(search_wait);
    const monotonic_t display_poll_wait = render(now, input_read);
#ifdef __APPLE__
    // Allow macOS to coalesce monitor checks with other system activity.
    set_wakeup_wait(display_poll_wait, ms_to_monotonic_t(100ll));
    if (has_cocoa_pending_actions) {
        process_cocoa_pending_actions();
        set_maximum_wait(0); // apply the actions' side effects immediately
    }
#else
    set_maximum_wait(display_poll_wait);
#endif
    report_reaped_pids();
    bool should_quit = false;
    if (global_state.has_pending_closes) should_quit = process_pending_closes(self);
    if (should_quit) {
        stop_main_loop();
    } else if (wakeup_wait == 0) request_tick_callback();
    const monotonic_t tolerance = wakeup_deadline - wakeup_wait;
    EVDBG("State check wait %.3f s, tolerance %.3f s\n", monotonic_t_to_s_double(wakeup_wait), monotonic_t_to_s_double(tolerance));
    update_main_loop_timer(state_check_timer, MAX(0, wakeup_wait), tolerance, !should_quit && wakeup_wait > 0);
}

static PyObject *
main_loop(ChildMonitor *self, PyObject *a UNUSED) {
#define main_loop_doc "The main thread loop"
    state_check_timer = add_main_loop_timer(1000, true, do_state_check, self, NULL);
    // the scrollback compressor wakes the loop when it has finished work to take in
    historybuf_set_wakeup(wakeup_main_loop);
    run_main_loop(process_global_state, self);
    historybuf_set_wakeup(NULL);
#ifdef __APPLE__
    cocoa_free_actions_data();
#endif
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

// }}}

// I/O thread functions {{{

// Moves the children added since the last time to those polled, with the children lock held. Room for them in children
// was reserved when they were added; the room in children_fds, which poll() uses without the lock, is made here, in
// the thread that polls. Without memory for it a child stays queued, and is tried again the next time round
static void
add_children(ChildMonitor *self) {
    if (add_queue_count && (!GROW(children_fds, EXTRA_FDS + self->count + add_queue_count) || !GROW(child_input_wake, self->count + add_queue_count))) {
        static bool reported = false;
        if (!reported) log_error("Out of memory starting to read from new children, will try again");
        reported = true;
        return;
    }
    for (; add_queue_count > 0;) {
        add_queue_count--;
        children[self->count] = add_queue[add_queue_count];
        child_input_wake[self->count] = (ChildInputWake){0};
        add_queue[add_queue_count] = EMPTY_CHILD;
        children_fds[EXTRA_FDS + self->count].fd = children[self->count].fd;
        children_fds[EXTRA_FDS + self->count].events = POLLIN;
        self->count++;
    }
}


static void
hangup(pid_t pid) {
    errno = 0;
    pid_t pgid = getpgid(pid);
    if (errno == ESRCH) return;
    if (errno != 0) {
        perror("Failed to get process group id for child");
        return;
    }
    if (killpg(pgid, SIGHUP) != 0) {
        if (errno != ESRCH) perror("Failed to kill child");
    }
}


static void
cleanup_child(ssize_t i) {
    safe_close(children[i].fd, __FILE__, __LINE__);
    hangup(children[i].pid);
}


static size_t
remove_marked_children(ChildMonitor *self, bool cleanup) {
    // Returns the number of removed children whose close the main thread did not request
    size_t unrequested = 0;
    if (self->count > 0) {
        size_t count = 0;
        for (ssize_t i = self->count - 1; i >= 0; i--) {
            if (children[i].needs_removal) {
                count++;
                if (!children[i].close_requested) unrequested++;
                if (cleanup) cleanup_child(i);
                remove_queue[remove_queue_count] = children[i];
                remove_queue_count++;
                children[i] = EMPTY_CHILD;
                children_fds[EXTRA_FDS + i].fd = -1;
                size_t num_to_right = self->count - 1 - i;
                if (num_to_right > 0) {
                    memmove(children + i, children + i + 1, num_to_right * sizeof(Child));
                    memmove(children_fds + EXTRA_FDS + i, children_fds + EXTRA_FDS + i + 1, num_to_right * sizeof(struct pollfd));
                    memmove(child_input_wake + i, child_input_wake + i + 1, num_to_right * sizeof(ChildInputWake));
                }
            }
        }
        self->count -= count;
    }
    return unrequested;
}

// remove_queue has room for every child, reserved when it was added. Returns the number of removed children whose
// close the main thread did not request
static size_t
remove_children(ChildMonitor *self) {
    return remove_marked_children(self, true);
}


#ifdef __APPLE__
// The macOS pty driver hands out data in chunks of at most 1024 bytes, so with
// a single read per poll() a child producing bulk output costs us a poll() and
// a wakeup of the I/O thread per kilobyte. Instead, when a read returns a full
// chunk, which means the child might have more data for us, keep reading until it runs
// out or PTY_GATHER_MAX bytes have been read. Each read is handed to the parser
// as soon as it completes, so the parser thread runs in parallel with the
// gathering and interactive latency is unchanged. The cap keeps a single busy
// child from monopolising the I/O thread, and must be small enough that the
// main loop is still woken often enough to keep the parser fed.
#define PTY_READ_CHUNK 1024u
#define PTY_GATHER_MAX (64u * 1024u)
#define pty_has_more_data(total, len) ((total) < PTY_GATHER_MAX && (size_t)(len) >= PTY_READ_CHUNK)
#else
// Other platforms return more data in a single read() so this is unlikely to help and may harm instead.
#define pty_has_more_data(total, len) (false)
#endif

static bool
read_bytes(int fd, Screen *screen) {
    size_t total = 0;
    bool child_alive = true, read_more = true;

    while (read_more) {
        size_t available_buffer_space;
        uint8_t *buf = vt_parser_create_write_buffer(screen->vt_parser, &available_buffer_space);
        if (!available_buffer_space) break;
        ssize_t len;
        while ((len = read(fd, buf, available_buffer_space)) < 0) {
            if (errno == EINTR) continue;
            // poll() told us data was ready, so wait for it, but only before anything has been read
            if (errno == EAGAIN && !total) continue;
            if (errno != EAGAIN && errno != EIO) perror("Call to read() from child fd failed");
            if (errno != EAGAIN) child_alive = false;
            break;
        }
        if (len > 0) {
            total += len;
            // a short read means the child has no more data queued for us
            read_more = pty_has_more_data(total, len);
        } else {
            if (len == 0) child_alive = false; // EOF
            len = 0;                           // the write buffer must be committed even when nothing was read
            read_more = false;
        }
        vt_parser_commit_write(screen->vt_parser, len);
    }
    return child_alive;
}


typedef struct {
    bool kill_signal, child_died, reload_config;
} SignalSet;

static bool
handle_signal(const siginfo_t *siginfo, void *data) {
    SignalSet *ss = data;
    switch (siginfo->si_signo) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP: ss->kill_signal = true; break;
        case SIGCHLD: ss->child_died = true; break;
        case SIGUSR1: ss->reload_config = true; break;
        case SIGUSR2: log_error("Received SIGUSR2: %d\n", siginfo->si_value.sival_int); break;
        default: break;
    }
    return true;
}

static void
mark_child_for_removal(ChildMonitor *self, pid_t pid, int exit_status) {
    children_mutex(lock);
    for (size_t i = 0; i < self->count; i++) {
        if (children[i].pid == pid) {
            children[i].needs_removal = true;
            children[i].exit_status = exit_status;
            children[i].child_died = true;
            break;
        }
    }
    children_mutex(unlock);
}

static void
mark_monitored_pids(pid_t pid, int status) {
    children_mutex(lock);
    record_reaped_pid(pid, status);
    children_mutex(unlock);
}

static void
reap_children(ChildMonitor *self, bool enable_close_on_child_death) {
    int status;
    pid_t pid;
    (void)self;
    while (true) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid == -1) {
            if (errno != EINTR) break;
        } else if (pid > 0) {
            if (enable_close_on_child_death) mark_child_for_removal(self, pid, status);
            mark_monitored_pids(pid, status);
        } else break;
    }
}

#ifdef KITTY_PRINT_BYTES_SENT_TO_CHILD
static void
print_text(const unsigned char *text, ssize_t sz) {
    for (ssize_t i = 0; i < sz; i++) {
        unsigned char ch = text[i];
        if (32 <= ch && ch < 127) {
            if (ch == '\\') fprintf(stderr, "%c", ch);
            fprintf(stderr, "%c", ch);
        } else fprintf(stderr, "\\x%02x", ch);
    }
}
#endif


static void
write_to_child(int fd, Screen *screen) {
    ssize_t ret = 0;
    screen_mutex(lock, write);
    while (screen->write_buf_start < screen->write_buf_used) {
        ret = write(fd, screen->write_buf + screen->write_buf_start, screen->write_buf_used - screen->write_buf_start);
#ifdef KITTY_PRINT_BYTES_SENT_TO_CHILD
        fprintf(stderr, "Wrote: %zd bytes: ", ret);
#endif
        if (ret > 0) {
#ifdef KITTY_PRINT_BYTES_SENT_TO_CHILD
            print_text(screen->write_buf + screen->write_buf_start, ret);
#endif
            screen->write_buf_start += ret;
        } else if (ret == 0) {
            // could mean anything, ignore
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EWOULDBLOCK || errno == EAGAIN) break;
            perror("Call to write() to child fd failed, discarding data.");
            screen->write_buf_start = screen->write_buf_used;
        }
#ifdef KITTY_PRINT_BYTES_SENT_TO_CHILD
        fprintf(stderr, "\n");
#endif
    }
    const size_t pending = screen->write_buf_used - screen->write_buf_start;
    if (!pending) screen->write_buf_start = screen->write_buf_used = 0;
    else if (screen->write_buf_start > pending) {
        // compact lazily to keep large pastes linear
        memmove(screen->write_buf, screen->write_buf + screen->write_buf_start, pending);
        screen->write_buf_start = 0;
        screen->write_buf_used = pending;
    }
    screen_mutex(unlock, write);
}

static bool
input_needs_wake(const ChildInputWake *c) {
    return c->pending.chunk && c->pending.chunk != c->woken_chunk && (c->pending.small_pending || c->pending.large_ready);
}

static ParserInputWake
pending_input_wake_info(ChildMonitor *self, monotonic_t now) {
    ParserInputWake info = {0};
    for (size_t i = 0; i < self->count; i++) {
        ChildInputWake *c = child_input_wake + i;
        // Only this thread commits bytes, so a child with nothing pending at its last
        // query and no reads since still has nothing pending.
        if (c->stale || c->pending.chunk) {
            c->pending = vt_parser_input_wake(children[i].screen->vt_parser, now, children[i].screen->low_latency);
            c->stale = false;
        }
        if (input_needs_wake(c)) {
            if (c->pending.small_pending) info.small_pending = true;
            if (c->pending.large_ready) info.large_ready = true;
        }
        monotonic_t held_until = c->pending.large_held_until;
        if (held_until && (!info.large_held_until || held_until < info.large_held_until)) info.large_held_until = held_until;
    }
    return info;
}

static void
remember_woken_input(ChildMonitor *self) {
    // Uses the state the wake was decided on, so a chunk that arrives after the
    // main thread has parsed is not marked as woken. Must run before the main loop
    // is woken, so that a small chunk which grows in the meantime is still accepted.
    for (size_t i = 0; i < self->count; i++) {
        ChildInputWake *c = child_input_wake + i;
        if (!input_needs_wake(c)) continue;
        if (c->pending.small_pending) vt_parser_accept_input(children[i].screen->vt_parser, c->pending.chunk);
        c->woken_chunk = c->pending.chunk;
    }
}

static void *
io_loop(void *data) {
    // The I/O thread loop
    size_t i;
    int ret;
    bool has_more, child_data_received, has_pending_wakeups = false, last_wakeup_was_early = false;
    // A signal or a removed child that needs the main loop. Latched until WAKEUP, since
    // signalfd is drained on receipt and a removed child is no longer polled.
    bool wake_latched = false;
    monotonic_t last_main_loop_wakeup_at = -1, now = -1, pending_wake_at = 0;
    Screen *screen;
    ChildMonitor *self = (ChildMonitor *)data;
    set_thread_name("KittyChildMon");
#define WAKEUP                                                                               \
    {                                                                                        \
        remember_woken_input(self);                                                          \
        if (self->benchmark_wakeup_fd >= 0) {                                                \
            static const char _wakeup_byte = 1;                                              \
            ssize_t _wakeup_ret UNUSED = write(self->benchmark_wakeup_fd, &_wakeup_byte, 1); \
        } else wakeup_main_loop();                                                           \
        last_main_loop_wakeup_at = now;                                                      \
        last_wakeup_was_early = false;                                                       \
        wake_latched = false;                                                                \
    }

    while (LIKELY(!self->shutting_down)) {
        children_mutex(lock);
        // A child that exited is no longer polled, so its held output has no deadline here.
        // The main thread flush-parses the remove queue. It does not need a wake for a
        // child it closed itself, the SIGCHLD from the hangup wakes it.
        if (remove_children(self)) {
            wake_latched = true;
            has_pending_wakeups = true;
            pending_wake_at = 0;
        }
        add_children(self);
        children_mutex(unlock);
        child_data_received = false;
        for (i = 0; i < self->count + EXTRA_FDS; i++) children_fds[i].revents = 0;
        for (i = 0; i < self->count; i++) {
            screen = children[i].screen;
            /* printf("i:%lu id:%lu fd: %d read_buf_sz: %lu write_buf_used: %lu\n", i, children[i].id, children[i].fd, screen->read_buf_sz,
             * screen->write_buf_used); */
            children_fds[EXTRA_FDS + i].events = vt_parser_has_space_for_input(screen->vt_parser) ? POLLIN : 0;
            screen_mutex(lock, write);
            children_fds[EXTRA_FDS + i].events |= (screen->write_buf_used ? POLLOUT : 0);
            screen_mutex(unlock, write);
        }
        if (has_pending_wakeups) {
            now = monotonic();
            monotonic_t time_delta = pending_wake_at - now;
            if (time_delta > 0) ret = poll(children_fds, self->count + EXTRA_FDS, monotonic_t_to_ms_ceil(time_delta));
            else ret = 0;
        } else {
            ret = poll(children_fds, self->count + EXTRA_FDS, -1);
        }
        if (ret > 0) {
            if (children_fds[0].revents & POLLIN) drain_fd(children_fds[0].fd); // wakeup
            if (children_fds[1].revents & POLLIN) {
                SignalSet ss = {0};
                wake_latched = true;
                read_signals(children_fds[1].fd, handle_signal, &ss);
                if (ss.kill_signal || ss.reload_config) {
                    children_mutex(lock);
                    if (ss.kill_signal) kill_signal_received = true;
                    if (ss.reload_config) reload_config_signal_received = true;
                    children_mutex(unlock);
                }
                if (ss.child_died) reap_children(self, OPT(close_on_child_death));
            }
            for (i = 0; i < self->count; i++) {
                if (children_fds[EXTRA_FDS + i].revents & (POLLIN | POLLHUP)) {
                    child_data_received = true;
                    child_input_wake[i].stale = true;
                    has_more = read_bytes(children_fds[EXTRA_FDS + i].fd, children[i].screen);
                    if (!has_more) {
                        // child is dead. The next iteration queues it and wakes the main thread.
                        children_mutex(lock);
                        children[i].needs_removal = true;
                        children_mutex(unlock);
                    }
                }
                if (children_fds[EXTRA_FDS + i].revents & POLLOUT) { write_to_child(children[i].fd, children[i].screen); }
                if (children_fds[EXTRA_FDS + i].revents & POLLNVAL) {
                    // fd was closed
                    children_mutex(lock);
                    children[i].needs_removal = true;
                    children_mutex(unlock);
                    log_error("The child %lu had its fd unexpectedly closed", children[i].id);
                }
            }
#ifdef DEBUG_POLL_EVENTS
            for (i = 0; i < self->count + EXTRA_FDS; i++) {
#define P(w) \
    if (children_fds[i].revents & w) printf("i:%lu %s\n", i, #w);
                P(POLLIN);
                P(POLLPRI);
                P(POLLOUT);
                P(POLLERR);
                P(POLLHUP);
                P(POLLNVAL);
#undef P
            }
#endif
        } else if (ret < 0) {
            if (errno != EAGAIN && errno != EINTR) { perror("Call to poll() failed"); }
        }
        // we only wakeup the main loop after input_delay as wakeup is an expensive operation
        // on some platforms, such as cocoa. Small pending input, typically the echo of typed
        // characters, wakes immediately, but at most once per input_delay so that continuous
        // streams of small writes are still coalesced.
        if (child_data_received || wake_latched || has_pending_wakeups) {
            now = monotonic();
            ParserInputWake pending = pending_input_wake_info(self, now);
            // rate_open is the gap since the previous wakeup. A large chunk the
            // parser will accept now (large_ready) wakes inside that gap, once per chunk.
            // A large chunk still inside input_delay waits until new_input_at + input_delay,
            // stored in wait_until, rather than until the next rate-limit slot.
            bool rate_open = now - last_main_loop_wakeup_at > OPT(input_delay);
            bool wake_now = pending.large_ready;
            bool early_small = false;
            bool need_rate_slot = false;
            // The main thread does not arm a timer for a held chunk, this deadline is the only one
            monotonic_t wait_until = pending.large_held_until;
            if (pending.small_pending) {
                if (rate_open) wake_now = true;
                else if (!last_wakeup_was_early) {
                    wake_now = true;
                    // This wake is the one early small wake unless a large chunk
                    // was ready anyway. Marking that would swallow the next echo.
                    early_small = !pending.large_ready;
                } else need_rate_slot = true;
            }
            // A latched wake uses the same rate limit as a small write that already woke early.
            if (wake_latched) {
                if (rate_open) wake_now = true;
                else need_rate_slot = true;
            }
            if (wake_now) {
                WAKEUP;
                if (early_small) last_wakeup_was_early = true;
            } else if (need_rate_slot) {
                monotonic_t rate_at = last_main_loop_wakeup_at + OPT(input_delay);
                if (!wait_until || rate_at < wait_until) wait_until = rate_at;
            }
            has_pending_wakeups = wait_until != 0;
            pending_wake_at = wait_until;
        }
    }
#undef WAKEUP
    children_mutex(lock);
    for (i = 0; i < self->count; i++) children[i].needs_removal = true;
    remove_children(self);
    children_mutex(unlock);
    return 0;
}
// }}}

// {{{ Talk thread functions

typedef struct {
    id_type id;
    size_t num_of_unresponded_messages_sent_to_main_thread, fd_array_idx;
    bool finished_reading, waiting_for_async_response;
    int fd;
    struct {
        char *data;
        size_t capacity, used, command_end;
        bool finished;
    } read;
    struct {
        char *data;
        size_t capacity, used;
        bool failed;
    } write;
    bool is_remote_control_peer;
} Peer;
static id_type peer_id_counter = 0;

typedef struct {
    size_t num_peers, peers_capacity;
    Peer *peers;
    LoopData loop_data;
} TalkData;
static TalkData talk_data = {0};

static int
start_talk_thread(ChildMonitor *self) {
    if (talk_thread_started) return 0;
    if (!init_loop_data(&talk_data.loop_data, 0)) return errno;
    int ret = pthread_create(&self->talk_thread, NULL, talk_loop, self);
    talk_thread_started = ret == 0;
    return ret;
}

typedef struct pollfd PollFD;
#define PEER_LIMIT 256
#define nuke_socket(s)                     \
    {                                      \
        shutdown(s, SHUT_RDWR);            \
        safe_close(s, __FILE__, __LINE__); \
    }

static id_type
add_peer(int peer, bool is_remote_control_peer) {
    id_type ans = 0;
    if (talk_data.num_peers < PEER_LIMIT) {
        ensure_space_for(&talk_data, peers, Peer, talk_data.num_peers + 8, peers_capacity, 8, false);
        Peer *p = talk_data.peers + talk_data.num_peers++;
        memset(p, 0, sizeof(Peer));
        p->fd = peer;
        // MSG_DONTWAIT still blocks on macOS
        int flags = fcntl(peer, F_GETFL);
        if (flags != -1) fcntl(peer, F_SETFL, flags | O_NONBLOCK);
        p->id = ++peer_id_counter;
        if (!p->id) p->id = ++peer_id_counter;
        ans = p->id;
        p->is_remote_control_peer = is_remote_control_peer;
    } else {
        log_error("Too many peers want to talk, ignoring one.");
        nuke_socket(peer);
    }
    return ans;
}

static atomic_int injected_accept_failures = 0; // testing only
static bool accept_error_logged = false;

static PyObject *
fail_next_accepts(PyObject *s UNUSED, PyObject *a) {
#define fail_next_accepts_doc "fail_next_accepts(n) -> make the next n talk thread accepts fail for testing"
    long n = PyLong_AsLong(a);
    if (n == -1 && PyErr_Occurred()) return NULL;
    if (n > INT_MAX) n = INT_MAX;
    const int prev = n >= 0 ? atomic_exchange(&injected_accept_failures, (int)n) : atomic_load(&injected_accept_failures);
    return PyLong_FromLong(prev);
}

static bool
accept_peer(int listen_fd, bool shutting_down, bool is_remote_control_peer, bool verify_peer_uid) {
    int peer = -1, err = 0;
    int injected = atomic_load_explicit(&injected_accept_failures, memory_order_relaxed);
    while (UNLIKELY(injected > 0) && !atomic_compare_exchange_weak(&injected_accept_failures, &injected, injected - 1));
    if (UNLIKELY(injected > 0)) err = EMFILE;
    if (!err) {
        peer = accept(listen_fd, NULL, NULL);
        if (peer == -1) err = errno;
    }
    if (UNLIKELY(peer == -1)) {
        if (err == EINTR) return true;
        if (shutting_down || err == EBADF || err == EINVAL || err == ENOTSOCK) return false;
        if (!accept_error_logged) log_error("accept() on talk socket failed with error: %s", strerror(err));
        accept_error_logged = true;
        if (err != ECONNABORTED && err != EAGAIN && err != EWOULDBLOCK) {
            // the pending connection was not consumed, avoid spinning on a readable listener
            struct timespec ts = {.tv_nsec = 50 * 1000000};
            nanosleep(&ts, NULL);
        }
        return true;
    }
    accept_error_logged = false;
    if (verify_peer_uid) {
        uid_t peer_uid;
        gid_t peer_gid;
        if (!get_peer_credentials(peer, &peer_uid, &peer_gid)) {
            log_error("Denying access to peer because failed to get uid and gid for peer: %d with error: %s", peer, strerror(errno));
            shutdown(peer, SHUT_RDWR);
            safe_close(peer, __FILE__, __LINE__);
            return true;
        }
        if (peer_uid != geteuid()) {
            log_error("Denying access to peer because its uid (%d) does not match our uid (%d)", peer_uid, geteuid());
            shutdown(peer, SHUT_RDWR);
            safe_close(peer, __FILE__, __LINE__);
            return true;
        }
    }
    talk_mutex(lock);
    add_peer(peer, is_remote_control_peer);
    talk_mutex(unlock);
    return true;
}

static void
free_peer(Peer *peer) {
    free(peer->read.data);
    peer->read.data = NULL;
    free(peer->write.data);
    peer->write.data = NULL;
    if (peer->fd > -1) {
        nuke_socket(peer->fd);
        peer->fd = -1;
    }
}

#define KITTY_CMD_PREFIX "\x1bP@kitty-cmd{"

static void
queue_peer_message(ChildMonitor *self, Peer *peer) {
    talk_mutex(lock);
    ensure_space_for(self, messages, Message, self->messages_count + 16, messages_capacity, 16, true);
    Message *m = self->messages + self->messages_count++;
    memset(m, 0, sizeof(Message));
    if (peer->read.used) {
        m->data = malloc(peer->read.used);
        if (m->data) {
            memcpy(m->data, peer->read.data, peer->read.used);
            m->sz = peer->read.used;
        }
    }
    m->peer_id = peer->id;
    m->is_remote_control_peer = peer->is_remote_control_peer;
    peer->num_of_unresponded_messages_sent_to_main_thread++;
    talk_mutex(unlock);
    wakeup_main_loop();
}

static void
notify_on_peer_removal(ChildMonitor *self, const Peer *p) {
    ensure_space_for(self, messages, Message, self->messages_count + 16, messages_capacity, 16, true);
    Message *m = self->messages + self->messages_count++;
    memset(m, 0, sizeof(Message));
    m->data = strdup("peer_death");
    if (m->data) m->sz = strlen("peer_death");
    m->peer_id = p->id;
    m->is_remote_control_peer = p->id;
}

static bool
has_complete_peer_command(Peer *peer) {
    peer->read.command_end = 0;
    if (peer->read.used > sizeof(KITTY_CMD_PREFIX) && memcmp(peer->read.data, KITTY_CMD_PREFIX, sizeof(KITTY_CMD_PREFIX) - 1) == 0) {
        for (size_t i = sizeof(KITTY_CMD_PREFIX) - 1; i < peer->read.used - 1; i++) {
            if (peer->read.data[i] == 0x1b && peer->read.data[i + 1] == '\\') {
                peer->read.command_end = i + 2;
                break;
            }
        }
    }
    return peer->read.command_end ? true : false;
}


static void
dispatch_peer_command(ChildMonitor *self, Peer *peer) {
    if (peer->read.command_end) {
        size_t used = peer->read.used;
        peer->read.used = peer->read.command_end;
        queue_peer_message(self, peer);
        peer->read.used = used;
        if (peer->read.used > peer->read.command_end) {
            peer->read.used -= peer->read.command_end;
            memmove(peer->read.data, peer->read.data + peer->read.command_end, peer->read.used);
        } else peer->read.used = 0;
        peer->read.command_end = 0;
    }
}

static void
read_from_peer(ChildMonitor *self, Peer *peer) {
#define failed(msg)                                     \
    {                                                   \
        log_error("Reading from peer failed: %s", msg); \
        shutdown(peer->fd, SHUT_RD);                    \
        peer->read.finished = true;                     \
        return;                                         \
    }
    if (peer->read.used >= peer->read.capacity) {
        if (peer->read.capacity >= 64 * 1024) failed("Ignoring too large message from peer");
        peer->read.capacity = MAX(8192u, peer->read.capacity * 2);
        peer->read.data = realloc(peer->read.data, peer->read.capacity);
        if (!peer->read.data) failed("Out of memory");
    }
    ssize_t n = recv(peer->fd, peer->read.data + peer->read.used, peer->read.capacity - peer->read.used, 0);
    if (n == 0) {
        peer->read.finished = true;
        shutdown(peer->fd, SHUT_RD);
        while (has_complete_peer_command(peer)) dispatch_peer_command(self, peer);
        queue_peer_message(self, peer);
        free(peer->read.data);
        peer->read.data = NULL;
        peer->read.used = 0;
        peer->read.capacity = 0;
    } else if (n < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) failed(strerror(errno));
    } else {
        peer->read.used += n;
        while (has_complete_peer_command(peer)) dispatch_peer_command(self, peer);
    }
#undef failed
}

static void
write_to_peer(Peer *peer) {
    talk_mutex(lock);
    ssize_t n = send(peer->fd, peer->write.data, peer->write.used, MSG_NOSIGNAL);
    if (n == 0) {
        log_error("send() to peer failed to send any data");
        peer->write.used = 0;
        peer->write.failed = true;
    } else if (n < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_error("write() to peer socket failed with error: %s", strerror(errno));
            peer->write.used = 0;
            peer->write.failed = true;
        }
    } else {
        if ((size_t)n < peer->write.used) memmove(peer->write.data, peer->write.data + n, peer->write.used - n);
        peer->write.used -= n;
    }
    talk_mutex(unlock);
}

static void
wakeup_talk_loop(bool in_signal_handler) {
    if (talk_thread_started) wakeup_loop(&talk_data.loop_data, in_signal_handler, "talk_loop");
}


static bool
prune_peers(ChildMonitor *self) {
    bool pruned = false;
    for (size_t idx = talk_data.num_peers; idx-- > 0;) {
        Peer *p = talk_data.peers + idx;
        if (p->read.finished && !p->num_of_unresponded_messages_sent_to_main_thread && !p->write.used && !p->waiting_for_async_response) {
            notify_on_peer_removal(self, p);
            free_peer(p);
            remove_i_from_array(talk_data.peers, idx, talk_data.num_peers);
            pruned = true;
        }
    }
    return pruned;
}

static struct {
    size_t num;
    struct {
        int peer_fd, pipe_fd;
    } fds[16];
} peers_to_inject = {0};

static bool
add_peer_to_injection_queue(int peer_fd, int pipe_fd) {
    bool added = false;
    talk_mutex(lock);
    if (peers_to_inject.num < arraysz(peers_to_inject.fds)) {
        peers_to_inject.fds[peers_to_inject.num].peer_fd = peer_fd;
        peers_to_inject.fds[peers_to_inject.num].pipe_fd = pipe_fd;
        peers_to_inject.num++;
        added = true;
    }
    talk_mutex(unlock);
    return added;
}

static void
simple_write_to_pipe(int fd, void *data, size_t sz) {
    // write a small amount of data to a pipe handling only EINTR
    while (true) {
        ssize_t ret = write(fd, data, sz);
        if (ret == -1 && errno == EINTR) continue;
        break;
    }
}


static void *
talk_loop(void *data) {
    // The talk thread loop
    ChildMonitor *self = (ChildMonitor *)data;
    set_thread_name("KittyPeerMon");
    // talk_data.loop_data is initialized by the thread that spawns this one, before it is spawned (see inject_peer() and start())
    PollFD fds[PEER_LIMIT + 8] = {{0}};
    size_t num_listen_fds = 0, num_peer_fds = 0;
#define add_listener(which)                    \
    if (self->which > -1) {                    \
        fds[num_listen_fds].fd = self->which;  \
        fds[num_listen_fds++].events = POLLIN; \
    }
    add_listener(talk_fd);
    add_listener(listen_fd);
#undef add_listener
    fds[num_listen_fds].fd = talk_data.loop_data.wakeup_read_fd;
    fds[num_listen_fds++].events = POLLIN;

    while (LIKELY(!self->shutting_down)) {
        num_peer_fds = 0;
        bool need_to_wakup_main_loop = false;
        talk_mutex(lock);
        if (peers_to_inject.num) {
            for (size_t i = 0; i < peers_to_inject.num; i++) {
                id_type added_peer_id = add_peer(peers_to_inject.fds[i].peer_fd, true);
                simple_write_to_pipe(peers_to_inject.fds[i].pipe_fd, &added_peer_id, sizeof(id_type));
                safe_close(peers_to_inject.fds[i].pipe_fd, __FILE__, __LINE__);
            }
            peers_to_inject.num = 0;
        }
        if (talk_data.num_peers > 0) {
            if (prune_peers(self)) need_to_wakup_main_loop = true;
            for (size_t i = 0; i < talk_data.num_peers; i++) {
                Peer *p = talk_data.peers + i;
                if (!p->read.finished || p->write.used) {
                    p->fd_array_idx = num_listen_fds + num_peer_fds++;
                    fds[p->fd_array_idx].fd = p->fd;
                    fds[p->fd_array_idx].revents = 0;
                    int flags = 0;
                    if (!p->read.finished) flags |= POLLIN;
                    if (p->write.used) flags |= POLLOUT;
                    fds[p->fd_array_idx].events = flags;
                } else p->fd_array_idx = 0;
            }
        }
        talk_mutex(unlock);
        if (need_to_wakup_main_loop) wakeup_main_loop();
        for (size_t i = 0; i < num_listen_fds; i++) fds[i].revents = 0;
        int ret = poll(fds, num_listen_fds + num_peer_fds, -1);
        if (ret > 0) {
            for (size_t i = 0; i < num_listen_fds - 1; i++) {
                if (fds[i].revents & POLLIN) {
                    const bool is_remote_control_peer = fds[i].fd == self->listen_fd;
                    const bool verify_peer_uid = is_remote_control_peer ? self->verify_listen_peer_uid : self->verify_talk_peer_uid;
                    if (!accept_peer(fds[i].fd, self->shutting_down, is_remote_control_peer, verify_peer_uid)) goto end;
                }
            }
            if (fds[num_listen_fds - 1].revents & POLLIN) {
                drain_fd(fds[num_listen_fds - 1].fd); // wakeup
            }
            for (size_t k = 0; k < talk_data.num_peers; k++) {
                Peer *p = talk_data.peers + k;
                if (p->fd_array_idx) {
                    if (fds[p->fd_array_idx].revents & POLLIN) read_from_peer(self, p);
                    if (fds[p->fd_array_idx].revents & POLLOUT) write_to_peer(p);
                    if (fds[p->fd_array_idx].revents & POLLHUP) {
                        // try to read and write nonetheless these functions will set the failed flags.
                        if (!p->read.finished) read_from_peer(self, p);
                        if (p->write.used) write_to_peer(p);
                    }
                    if (fds[p->fd_array_idx].revents & POLLNVAL) {
                        p->read.finished = true;
                        p->write.failed = true;
                        p->write.used = 0;
                    }
                }
            }
        } else if (ret < 0) {
            if (errno != EAGAIN && errno != EINTR) perror("poll() on talk fds failed");
        }
    }
end:
    free_loop_data(&talk_data.loop_data);
    talk_mutex(lock);
    for (size_t i = 0; i < talk_data.num_peers; i++) free_peer(talk_data.peers + i);
    free(talk_data.peers);
    talk_data.peers = NULL;
    talk_data.num_peers = talk_data.peers_capacity = 0;
    talk_mutex(unlock);
    return 0;
}

static void
send_response_to_peer(id_type peer_id, const char *msg, size_t msg_sz, bool is_async_response) {
    bool wakeup = false;
    talk_mutex(lock);
    for (size_t i = 0; i < talk_data.num_peers; i++) {
        Peer *peer = talk_data.peers + i;
        if (peer->id == peer_id) {
            peer->waiting_for_async_response = is_async_response;
            if (peer->num_of_unresponded_messages_sent_to_main_thread) peer->num_of_unresponded_messages_sent_to_main_thread--;
            if (!peer->write.failed) {
                if (peer->write.capacity - peer->write.used < msg_sz) {
                    void *data = realloc(peer->write.data, peer->write.capacity + msg_sz);
                    if (data) {
                        peer->write.data = data;
                        peer->write.capacity += msg_sz;
                    } else fatal("Out of memory");
                }
                if (msg_sz && msg) {
                    memcpy(peer->write.data + peer->write.used, msg, msg_sz);
                    peer->write.used += msg_sz;
                }
            }
            wakeup = true;
            break;
        }
    }
    talk_mutex(unlock);
    if (wakeup) wakeup_talk_loop(false);
}

// }}}

// Boilerplate {{{
static PyMethodDef methods[] = {
    METHOD(add_child, METH_VARARGS) METHOD(inject_peer, METH_O) METHOD(fail_next_accepts, METH_O) METHOD(needs_write, METH_VARARGS) METHOD(start, METH_NOARGS)
        METHOD(wakeup, METH_NOARGS) METHOD(shutdown_monitor, METH_NOARGS) METHOD(main_loop, METH_NOARGS) METHOD(mark_for_close, METH_VARARGS)
            METHOD(resize_pty, METH_VARARGS) METHODB(handled_signals, METH_NOARGS),
    METHOD(set_wakeup_fd, METH_VARARGS) METHOD(parse_input_once, METH_NOARGS){"set_iutf8_winid", (PyCFunction)pyset_iutf8, METH_VARARGS, ""},
    {NULL} /* Sentinel */
};


PyTypeObject ChildMonitor_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "fast_data_types.ChildMonitor",
    .tp_basicsize = sizeof(ChildMonitor),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "ChildMonitor",
    .tp_methods = methods,
    .tp_new = new_childmonitor_object,
};


static PyObject *
safe_pipe(PyObject *self UNUSED, PyObject *args) {
    int nonblock = 1;
    if (!PyArg_ParseTuple(args, "|p", &nonblock)) return NULL;
    int fds[2] = {0};
    if (!self_pipe(fds, nonblock)) return PyErr_SetFromErrno(PyExc_OSError);
    return Py_BuildValue("ii", fds[0], fds[1]);
}

static PyObject *
cocoa_set_menubar_title(PyObject *self UNUSED, PyObject *args UNUSED) {
#ifdef __APPLE__
    PyObject *title = NULL;
    if (!PyArg_ParseTuple(args, "U", &title)) return NULL;
    change_menubar_title(title);
#endif
    Py_RETURN_NONE;
}

static PyObject *
send_data_to_peer(PyObject *self UNUSED, PyObject *args) {
    char *msg;
    Py_ssize_t sz;
    unsigned long long peer_id;
    int is_async_response = 0;
    if (!PyArg_ParseTuple(args, "Ks#|p", &peer_id, &msg, &sz, &is_async_response)) return NULL;
    send_response_to_peer(peer_id, msg, sz, is_async_response);
    Py_RETURN_NONE;
}

// test only: adds count children, as add_child() does, moves them to those polled, as the I/O thread does, removes
// every remove_every-th one and takes the removed ones as the main thread does, without any child process, file
// descriptor or thread, so it must not be called while a ChildMonitor exists. Returns the ids of the children left,
// those removed, in the order taken, and the capacity of children.
static PyObject *
test_child_arrays(PyObject *self UNUSED, PyObject *args) {
    unsigned long count, remove_every;
    if (!PyArg_ParseTuple(args, "kk", &count, &remove_every)) return NULL;
    if (the_monitor) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot test the child arrays while a ChildMonitor exists");
        return NULL;
    }
    ChildMonitor fake = {0};
    for (unsigned long n = 0; n < count; n++) {
        if (!reserve_child_slot(fake.count)) return PyErr_NoMemory();
        add_queue[add_queue_count++] = (Child){.id = n + 1, .fd = -1};
        // the I/O thread takes them in batches of whatever was added since it last looked
        if (n % 97 == 96) add_children(&fake);
    }
    add_children(&fake);
    if (add_queue_count) return PyErr_NoMemory();
    for (size_t i = 0; remove_every && i < fake.count; i++) {
        if (children[i].id % remove_every == 0) children[i].needs_removal = true;
    }
    remove_marked_children(&fake, false);
    RAII_PyObject(left, PyList_New(fake.count));
    RAII_PyObject(removed, PyList_New(remove_queue_count));
    if (!left || !removed) return NULL;
    for (size_t i = 0; i < fake.count; i++) {
        if (children_fds[EXTRA_FDS + i].fd != children[i].fd) {
            PyErr_SetString(PyExc_AssertionError, "children_fds does not match children");
            return NULL;
        }
        PyList_SET_ITEM(left, i, PyLong_FromUnsignedLong(children[i].id));
    }
    for (size_t i = 0; i < remove_queue_count; i++) PyList_SET_ITEM(removed, i, PyLong_FromUnsignedLong(remove_queue[i].id));
    const size_t capacity = children_capacity;
    for (size_t i = 0; i < fake.count; i++) children[i] = EMPTY_CHILD;
    for (size_t i = 0; i < remove_queue_count; i++) remove_queue[i] = EMPTY_CHILD;
    remove_queue_count = 0;
    return Py_BuildValue("OOk", left, removed, (unsigned long)capacity);
}

// test only: monitors count pids, as monitor_pid() does, reaps every one of them, as the I/O thread does, and returns
// how many were recorded as reaped, which must be all of them, and how many are still monitored
static PyObject *
test_monitored_pids(PyObject *self UNUSED, PyObject *args) {
    unsigned long count;
    if (!PyArg_ParseTuple(args, "k", &count)) return NULL;
    if (the_monitor) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot test the monitored pids while a ChildMonitor exists");
        return NULL;
    }
    for (unsigned long n = 0; n < count; n++) {
        if (!add_monitored_pid((pid_t)(1000000 + n))) return PyErr_NoMemory();
        // some die before others are monitored
        if (n % 3 == 2) record_reaped_pid((pid_t)(1000000 + n - 1), 0);
    }
    for (unsigned long n = 0; n < count; n++) record_reaped_pid((pid_t)(1000000 + n), (int)(n & 0xff));
    const size_t reaped = reaped_pids_count, monitored = monitored_pids_count;
    reaped_pids_count = 0;
    monitored_pids_count = 0;
    return Py_BuildValue("kk", (unsigned long)reaped, (unsigned long)monitored);
}

static PyMethodDef module_methods[] = {
    METHODB(test_child_arrays, METH_VARARGS),
    METHODB(test_monitored_pids, METH_VARARGS),
    METHODB(safe_pipe, METH_VARARGS),
    {"add_timer", (PyCFunction)add_python_timer, METH_VARARGS, ""},
    {"remove_timer", (PyCFunction)remove_python_timer, METH_VARARGS, ""},
    METHODB(monitor_pid, METH_VARARGS),
    METHODB(send_data_to_peer, METH_VARARGS),
    METHODB(cocoa_set_menubar_title, METH_VARARGS),
    METHODB(mask_kitty_signals_process_wide, METH_NOARGS),
    {"sigqueue", (PyCFunction)sig_queue, METH_VARARGS, ""},
    {NULL} /* Sentinel */
};

bool
init_child_monitor(PyObject *module) {
    if (PyType_Ready(&ChildMonitor_Type) < 0) return false;
    if (PyModule_AddObject(module, "ChildMonitor", (PyObject *)&ChildMonitor_Type) != 0) return false;
    Py_INCREF(&ChildMonitor_Type);
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
#ifdef NO_SIGQUEUE
    PyModule_AddIntConstant(module, "has_sigqueue", 0);
#else
    PyModule_AddIntConstant(module, "has_sigqueue", 1);
#endif
    return true;
}

// }}}
