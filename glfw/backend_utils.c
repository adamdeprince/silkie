/*
 * backend_utils.c
 * Copyright (C) 2018 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#define _GNU_SOURCE
#include "backend_utils.h"
#include "internal.h"
#include "memfd.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <sys/mman.h>

#ifdef __NetBSD__
#define ppoll pollts
#endif

void
update_fds(EventLoopData *eld) {
    for (nfds_t i = 0; i < eld->watches_count; i++) {
        Watch *w = eld->watches + i;
        eld->fds[i].fd = w->fd;
        eld->fds[i].events = w->enabled ? w->events : 0;
    }
}


id_type
addWatch(EventLoopData *eld, const char *name, int fd, int events, int enabled, watch_callback_func cb, void *cb_data) {
    if (eld->watches_count >= sizeof(eld->watches) / sizeof(eld->watches[0])) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Too many watches added");
        return 0;
    }
    static id_type watch_counter = 0;
    Watch *w = eld->watches + eld->watches_count++;
    w->name = name;
    w->fd = fd;
    w->events = events;
    w->enabled = enabled;
    w->callback = cb;
    w->callback_data = cb_data;
    w->free = NULL;
    w->id = ++watch_counter;
    update_fds(eld);
    return w->id;
}

#define removeX(which, item_id, update_func)                                                                                                   \
    {                                                                                                                                          \
        for (nfds_t i = 0; i < eld->which##_count; i++) {                                                                                      \
            if (eld->which[i].id == item_id) {                                                                                                 \
                eld->which##_count--;                                                                                                          \
                if (eld->which[i].callback_data && eld->which[i].free) {                                                                       \
                    eld->which[i].free(eld->which[i].id, eld->which[i].callback_data);                                                         \
                    eld->which[i].callback_data = NULL;                                                                                        \
                    eld->which[i].free = NULL;                                                                                                 \
                }                                                                                                                              \
                if (i < eld->which##_count) { memmove(eld->which + i, eld->which + i + 1, sizeof(eld->which[0]) * (eld->which##_count - i)); } \
                update_func(eld);                                                                                                              \
                break;                                                                                                                         \
            }                                                                                                                                  \
        }                                                                                                                                      \
    }

void
removeWatch(EventLoopData *eld, id_type watch_id) {
    removeX(watches, watch_id, update_fds);
}

void
toggleWatch(EventLoopData *eld, id_type watch_id, int enabled) {
    for (nfds_t i = 0; i < eld->watches_count; i++) {
        if (eld->watches[i].id == watch_id) {
            if (eld->watches[i].enabled != enabled) {
                eld->watches[i].enabled = enabled;
                update_fds(eld);
            }
            break;
        }
    }
}

static id_type timer_counter = 0;

static int
compare_timers(const void *a_, const void *b_) {
    const Timer *a = (const Timer *)a_, *b = (const Timer *)b_;
    return (a->trigger_at > b->trigger_at) ? 1 : (a->trigger_at < b->trigger_at) ? -1 : 0;
}

static void
update_timers(EventLoopData *eld) {
    if (eld->timers_count > 1) qsort(eld->timers, eld->timers_count, sizeof(eld->timers[0]), compare_timers);
}

static bool
grow_timers(EventLoopData *eld) {
    size_t cap = eld->timers_capacity ? eld->timers_capacity : 64;
    if (eld->timers_capacity) {
        if (cap > SIZE_MAX / 2) goto overflow;
        cap *= 2;
    }
    // timers_count is an nfds_t, so it can count every entry of the arrays
    _Static_assert(sizeof(nfds_t) >= sizeof(size_t), "nfds_t cannot count all timers");
    if (cap > SIZE_MAX / sizeof(Timer) || cap > SIZE_MAX / sizeof(TimerDispatch)) goto overflow;
    // Each array is replaced only once its larger copy exists, so a failure
    // leaves both valid, with the old capacity
    Timer *timers = realloc(eld->timers, cap * sizeof(Timer));
    if (!timers) goto oom;
    eld->timers = timers;
    TimerDispatch *dispatches = realloc(eld->timer_dispatches, cap * sizeof(TimerDispatch));
    if (!dispatches) goto oom;
    eld->timer_dispatches = dispatches;
    eld->timers_capacity = cap;
    return true;
overflow:
    _glfwInputError(GLFW_PLATFORM_ERROR, "Too many timers added, the timer count would overflow");
    return false;
oom:
    _glfwInputError(GLFW_OUT_OF_MEMORY, "Out of memory adding a timer");
    return false;
}

static void
free_timers(EventLoopData *eld) {
    free(eld->timers);
    eld->timers = NULL;
    free(eld->timer_dispatches);
    eld->timer_dispatches = NULL;
    eld->timers_capacity = 0;
    eld->timers_count = 0;
}

// addTimer(), removeTimer(), removeAllTimers() and dispatchTimers() are exported,
// like utf_8_strndup(), so that kitty_tests/glfw.py can test the timer table
GLFWAPI id_type
addTimer(
    EventLoopData *eld, const char *name, monotonic_t interval, int enabled, bool repeats, timer_callback_func cb, void *cb_data, GLFWuserdatafreefun free) {
    if (eld->timers_count >= eld->timers_capacity && !grow_timers(eld)) return 0;
    Timer *t = eld->timers + eld->timers_count++;
    t->interval = interval;
    t->name = name;
    t->trigger_at = enabled ? monotonic() + interval : MONOTONIC_T_MAX;
    t->repeats = repeats;
    t->callback = cb;
    t->callback_data = cb_data;
    t->free = free;
    t->id = ++timer_counter;
    update_timers(eld);
    return timer_counter;
}

GLFWAPI void
removeTimer(EventLoopData *eld, id_type timer_id) {
    removeX(timers, timer_id, update_timers);
}

GLFWAPI void
removeAllTimers(EventLoopData *eld) {
    for (nfds_t i = 0; i < eld->timers_count; i++) {
        if (eld->timers[i].free && eld->timers[i].callback_data) eld->timers[i].free(eld->timers[i].id, eld->timers[i].callback_data);
    }
    eld->timers_count = 0;
}

void
toggleTimer(EventLoopData *eld, id_type timer_id, int enabled) {
    for (nfds_t i = 0; i < eld->timers_count; i++) {
        if (eld->timers[i].id == timer_id) {
            monotonic_t trigger_at = enabled ? (monotonic() + eld->timers[i].interval) : MONOTONIC_T_MAX;
            if (trigger_at != eld->timers[i].trigger_at) {
                eld->timers[i].trigger_at = trigger_at;
                update_timers(eld);
            }
            break;
        }
    }
}

void
changeTimerInterval(EventLoopData *eld, id_type timer_id, monotonic_t interval) {
    for (nfds_t i = 0; i < eld->timers_count; i++) {
        if (eld->timers[i].id == timer_id) {
            eld->timers[i].interval = interval;
            break;
        }
    }
}


monotonic_t
prepareForPoll(EventLoopData *eld, monotonic_t timeout) {
    for (nfds_t i = 0; i < eld->watches_count; i++) eld->fds[i].revents = 0;
    if (!eld->timers_count || eld->timers[0].trigger_at == MONOTONIC_T_MAX) return timeout;
    monotonic_t now = monotonic(), next_repeat_at = eld->timers[0].trigger_at;
    if (timeout < 0 || now + timeout > next_repeat_at) { timeout = next_repeat_at <= now ? 0 : next_repeat_at - now; }
    return timeout;
}

static struct timespec
calc_time(monotonic_t nsec) {
    struct timespec result;
    result.tv_sec = nsec / (1000LL * 1000LL * 1000LL);
    result.tv_nsec = nsec % (1000LL * 1000LL * 1000LL);
    return result;
}

int
pollWithTimeout(struct pollfd *fds, nfds_t nfds, monotonic_t timeout) {
    struct timespec tv = calc_time(timeout);
    return ppoll(fds, nfds, &tv, NULL);
}

static void
dispatchEvents(EventLoopData *eld) {
    for (nfds_t i = 0; i < eld->watches_count; i++) {
        Watch *ww = eld->watches + i;
        struct pollfd *pfd = eld->fds + i;
        if (pfd->revents & ww->events) {
            ww->ready = 1;
            if (ww->callback) ww->callback(ww->fd, pfd->revents, ww->callback_data);
        } else ww->ready = 0;
    }
}

GLFWAPI size_t
dispatchTimers(EventLoopData *eld) {
    if (!eld->timers_count || eld->timers[0].trigger_at == MONOTONIC_T_MAX) return 0;
    // timer_dispatches has room for every timer, see grow_timers()
    size_t num_dispatches = 0;
    monotonic_t now = monotonic();
    for (nfds_t i = 0; i < eld->timers_count && eld->timers[i].trigger_at <= now; i++) {
        eld->timers[i].trigger_at = now + eld->timers[i].interval;
        TimerDispatch *d = eld->timer_dispatches + num_dispatches++;
        d->func = eld->timers[i].callback;
        d->id = eld->timers[i].id;
        d->data = eld->timers[i].callback_data;
        d->repeats = eld->timers[i].repeats;
    }
    // We dispatch separately so that the callbacks can modify timers. A
    // callback that adds timers can move timer_dispatches, so copy each entry
    // out before calling it and do not keep pointers into the array.
    for (size_t i = 0; i < num_dispatches; i++) {
        const TimerDispatch d = eld->timer_dispatches[i];
        d.func(d.id, d.data);
        if (!d.repeats) { removeTimer(eld, d.id); }
    }
    if (num_dispatches) update_timers(eld);
    return num_dispatches;
}

static void
drain_wakeup_fd(int fd, EventLoopData *eld) {
    static char drain_buf[64];
    eld->wakeup_data_read = false;
    while (true) {
        ssize_t ret = read(fd, drain_buf, sizeof(drain_buf));
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret > 0) {
            eld->wakeup_data_read = true;
            continue;
        }
        break;
    }
}

static void
mark_wakep_fd_ready(int fd UNUSED, int events UNUSED, void *data) {
    ((EventLoopData *)(data))->wakeup_fd_ready = true;
}

static void
mark_key_repeat_fd_ready(int fd UNUSED, int events UNUSED, void *data) {
    ((EventLoopData *)(data))->key_repeat_fd_ready = true;
}


bool
initPollData(EventLoopData *eld, int display_fd) {
    if (!addWatch(eld, "display", display_fd, POLLIN, 1, NULL, NULL)) return false;
#ifdef HAS_EVENT_FD
    eld->wakeupFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (eld->wakeupFd == -1) return false;
    const int wakeup_fd = eld->wakeupFd;
#else
    if (pipe2(eld->wakeupFds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    const int wakeup_fd = eld->wakeupFds[0];
#endif
    if (!addWatch(eld, "wakeup", wakeup_fd, POLLIN, 1, mark_wakep_fd_ready, eld)) return false;

#ifdef HAS_TIMER_FD
    eld->key_repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (eld->key_repeat_fd < 0) return false;
    const int key_repeat_fd = eld->key_repeat_fd;
#else
    if (pipe2(eld->key_repeat_fds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    const int key_repeat_fd = eld->key_repeat_fds[0];
#endif
#ifdef _GLFW_WAYLAND
    if (!addWatch(eld, "key_repeat", key_repeat_fd, POLLIN, 1, mark_key_repeat_fd_ready, eld)) return false;
#else
    (void)key_repeat_fd;
    (void)mark_key_repeat_fd_ready;
#endif
    return true;
}

void
check_for_wakeup_events(EventLoopData *eld) {
#ifdef HAS_EVENT_FD
    int fd = eld->wakeupFd;
#else
    int fd = eld->wakeupFds[0];
#endif
    drain_wakeup_fd(fd, eld);
}

void
wakeupEventLoop(EventLoopData *eld) {
#ifdef HAS_EVENT_FD
    static const uint64_t value = 1;
    while (write(eld->wakeupFd, &value, sizeof value) < 0 && (errno == EINTR || errno == EAGAIN));
#else
    while (write(eld->wakeupFds[1], "w", 1) < 0 && (errno == EINTR || errno == EAGAIN));
#endif
}

static void
closeFds(int *fds, size_t count) {
    while (count--) {
        if (*fds > 0) {
            close(*fds);
            *fds = -1;
        }
        fds++;
    }
}

void
finalizePollData(EventLoopData *eld) {
    (void)closeFds;
    // Only the storage is freed here, the free callbacks of remaining timers
    // are called by removeAllTimers(), as before
    free_timers(eld);
#ifdef HAS_EVENT_FD
    close(eld->wakeupFd);
    eld->wakeupFd = -1;
#else
    closeFds(eld->wakeupFds, arraysz(eld->wakeupFds));
#endif
#ifdef HAS_TIMER_FD
    close(eld->key_repeat_fd);
    eld->key_repeat_fd = -1;
#else
    closeFds(eld->key_repeat_fds, arraysz(eld->key_repeat_fds));
#endif
}

static int
display_fd_ready(EventLoopData *eld) {
    // watches[0] is always the display fd. Errors count as ready: the display
    // callback must complete the read rather than cancel it, so that a
    // compositor disconnect is turned into a display error instead of a poll()
    // that keeps returning immediately forever.
    return eld->fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL);
}

int
pollForEvents(EventLoopData *eld, monotonic_t timeout, watch_callback_func display_callback) {
    int read_ok = 0;
    timeout = prepareForPoll(eld, timeout);
    EVDBG("pollForEvents final timeout: %.3f", monotonic_t_to_s_double(timeout));
    int result;
    monotonic_t end_time = monotonic() + timeout;
    eld->wakeup_fd_ready = false;
    eld->key_repeat_fd_ready = false;

    while (1) {
        if (timeout >= 0) {
            errno = 0;
            result = pollWithTimeout(eld->fds, eld->watches_count, timeout);
            int saved_errno = errno;
            if (display_callback) display_callback(result, display_fd_ready(eld), NULL);
            dispatchTimers(eld);
            if (result > 0) {
                dispatchEvents(eld);
                read_ok = eld->watches[0].ready;
                break;
            }
            timeout = end_time - monotonic();
            if (timeout <= 0) break;
            if (result < 0 && (saved_errno == EINTR || saved_errno == EAGAIN)) continue;
            break;
        } else {
            errno = 0;
            result = poll(eld->fds, eld->watches_count, -1);
            int saved_errno = errno;
            if (display_callback) display_callback(result, display_fd_ready(eld), NULL);
            dispatchTimers(eld);
            if (result > 0) {
                dispatchEvents(eld);
                read_ok = eld->watches[0].ready;
            }
            if (result < 0 && (saved_errno == EINTR || saved_errno == EAGAIN)) continue;
            break;
        }
    }
    return read_ok;
}

// Duplicate a UTF-8 encoded string
// but cut it so that it has at most max_length bytes plus the null byte.
// This does not take combining characters into account.
GLFWAPI char *
utf_8_strndup(const char *source, size_t max_length) {
    if (!source) return NULL;
    size_t length = strnlen(source, max_length);
    if (length >= max_length) {
        for (length = max_length; length > 0; length--) {
            if ((source[length] & 0xC0) != 0x80) break;
        }
    }

    char *result = malloc(length + 1);
    memcpy(result, source, length);
    result[length] = 0;
    return result;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 *
 * posix_fallocate() is used to guarantee that disk space is available
 * for the file at the given size. If disk space is insufficient, errno
 * is set to ENOSPC. If posix_fallocate() is not supported, program may
 * receive SIGBUS on accessing mmap()'ed file contents instead.
 */
int
createAnonymousFile(off_t size) {
    int ret, fd = -1, shm_anon = 0;
#ifdef HAS_MEMFD_CREATE
    fd = glfw_memfd_create("glfw-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    // We can add this seal before calling posix_fallocate(), as the file
    // is currently zero-sized anyway.
    //
    // There is also no need to check for the return value, we couldn’t do
    // anything with it anyway.
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
#elif defined(SHM_ANON)
    fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    shm_anon = 1;
#else
    static const char template[] = "/glfw-shared-XXXXXX";
    const char *path;
    char *name;

    path = getenv("XDG_RUNTIME_DIR");
    if (!path) {
        errno = ENOENT;
        return -1;
    }

    name = calloc(strlen(path) + sizeof(template), 1);
    strcpy(name, path);
    strcat(name, template);

    fd = createTmpfileCloexec(name);

    free(name);

    if (fd < 0) return -1;
#endif
    // posix_fallocate does not work on SHM descriptors
    ret = shm_anon ? ftruncate(fd, size) : posix_fallocate(fd, 0, size);
    if (ret != 0) {
        close(fd);
        errno = ret;
        return -1;
    }
    return fd;
}
