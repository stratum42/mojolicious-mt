#ifndef MOJO_MT_REACTOR_H
#define MOJO_MT_REACTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#if !defined(MT_FORCE_POLL) && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__))
#  define MT_USE_KQUEUE 1
#  include <sys/types.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  define MT_WAKE_IDENT 0x4d54574bUL
#elif !defined(MT_FORCE_POLL) && defined(__linux__)
#  define MT_USE_EPOLL 1
#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#elif !defined(MT_FORCE_POLL) && defined(_AIX) && !defined(__PASE__)
#  define MT_USE_POLLSET 1
#  include <sys/pollset.h>
#  include <fcntl.h>
#else
#  define MT_USE_POLL 1
#  include <poll.h>
#  include <fcntl.h>
#endif

static inline const char *mt_reactor_backend_name(void) {
#if defined(MT_USE_KQUEUE)
  return "kqueue";
#elif defined(MT_USE_EPOLL)
  return "epoll";
#elif defined(MT_USE_POLLSET)
  return "pollset";
#else
  return "poll";
#endif
}

#if defined(MT_USE_POLLSET) || defined(MT_USE_POLL)
static inline void mt_set_nonblock_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags >= 0) (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
}
#endif

#define MT_REACTOR_MAX_EVENTS 128

static inline double mt_monotonic_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
  SV     *cb;
  uint8_t mode;    /* bit 0 = read, bit 1 = write */
  uint8_t active;
  uint8_t reg_r;   /* registered in kqueue/epoll for read */
  uint8_t reg_w;   /* registered in kqueue/epoll for write */
  uint8_t dirty;   /* 1 if queued in dirty_fds[] (kqueue batched changes) */
} mt_io_watcher_t;

typedef struct {
  uint32_t gen;
  int32_t  heap_idx;  /* index in heap[], or -1 if popped/inactive */
  uint32_t next_free; /* freelist link when inactive */
  double   fire_at;
  double   after;
  SV      *cb;
  uint8_t  recurring;
  uint8_t  active;
} mt_timer_slot_t;

typedef struct {
#if defined(MT_USE_POLLSET)
  pollset_t        poll_fd;      /* AIX pollset handle */
  int              wake_pipe[2]; /* self-pipe wakeup */
#elif defined(MT_USE_POLL)
  int              poll_fd;      /* unused (-1) */
  int              wake_pipe[2]; /* self-pipe wakeup */
  struct pollfd   *pfds;
  int              pfds_cap;
#elif defined(MT_USE_EPOLL)
  int              poll_fd;      /* epoll fd */
  int              wake_fd;      /* eventfd */
#else /* MT_USE_KQUEUE */
  int              poll_fd;      /* kqueue fd */
  int             *dirty_fds;
  int              dirty_len;
  int              dirty_cap;
#endif
  uint64_t         generation; /* bumped on reset() */

  /* O(1) fd-indexed I/O watchers */
  mt_io_watcher_t *io;
  int              io_cap;
  int              io_count;

  /* O(1) slot table + binary min-heap for timers */
  mt_timer_slot_t *timers;
  uint32_t         timer_cap;
  uint32_t         timer_count;
  uint32_t         free_head;  /* UINT32_MAX if empty */
  uint32_t         next_gen;

  uint32_t        *heap;
  uint32_t         heap_len;
  uint32_t         heap_cap;
} mt_reactor_t;

/* ---- Binary min-heap helpers ---- */

static inline void mt_heap_swap(mt_reactor_t *r, uint32_t a, uint32_t b) {
  uint32_t sa = r->heap[a];
  uint32_t sb = r->heap[b];
  r->heap[a] = sb;
  r->heap[b] = sa;
  r->timers[sb].heap_idx = (int32_t)a;
  r->timers[sa].heap_idx = (int32_t)b;
}

static void mt_heap_sift_up(mt_reactor_t *r, uint32_t idx) {
  while (idx > 0) {
    uint32_t parent = (idx - 1) >> 1;
    if (r->timers[r->heap[idx]].fire_at < r->timers[r->heap[parent]].fire_at) {
      mt_heap_swap(r, idx, parent);
      idx = parent;
    } else {
      break;
    }
  }
}

static void mt_heap_sift_down(mt_reactor_t *r, uint32_t idx) {
  uint32_t n = r->heap_len;
  while (1) {
    uint32_t left  = (idx << 1) + 1;
    uint32_t right = left + 1;
    uint32_t best  = idx;
    if (left < n && r->timers[r->heap[left]].fire_at < r->timers[r->heap[best]].fire_at)
      best = left;
    if (right < n && r->timers[r->heap[right]].fire_at < r->timers[r->heap[best]].fire_at)
      best = right;
    if (best == idx) break;
    mt_heap_swap(r, idx, best);
    idx = best;
  }
}

static int mt_heap_push(mt_reactor_t *r, uint32_t slot_idx) {
  if (r->heap_len >= r->heap_cap) {
    uint32_t ncap = r->heap_cap ? r->heap_cap * 2 : 32;
    uint32_t *nh  = (uint32_t *)realloc(r->heap, ncap * sizeof(uint32_t));
    if (!nh) return 0;
    r->heap = nh;
    r->heap_cap = ncap;
  }
  uint32_t pos = r->heap_len++;
  r->heap[pos] = slot_idx;
  r->timers[slot_idx].heap_idx = (int32_t)pos;
  mt_heap_sift_up(r, pos);
  return 1;
}

static void mt_heap_remove_at(mt_reactor_t *r, uint32_t pos) {
  uint32_t slot_idx = r->heap[pos];
  r->timers[slot_idx].heap_idx = -1;
  uint32_t last = --r->heap_len;
  if (pos < last) {
    uint32_t moved = r->heap[last];
    r->heap[pos] = moved;
    r->timers[moved].heap_idx = (int32_t)pos;
    mt_heap_sift_up(r, pos);
    mt_heap_sift_down(r, (uint32_t)r->timers[moved].heap_idx);
  }
}

#ifdef MT_USE_KQUEUE
static inline void mt_mark_dirty(mt_reactor_t *r, int fd) {
  if (r->io[fd].dirty) return;
  if (r->dirty_len >= r->dirty_cap) {
    int ncap = r->dirty_cap ? r->dirty_cap * 2 : 64;
    int *nd  = (int *)realloc(r->dirty_fds, (size_t)ncap * sizeof(int));
    if (!nd) return;
    r->dirty_fds = nd;
    r->dirty_cap = ncap;
  }
  r->io[fd].dirty = 1;
  r->dirty_fds[r->dirty_len++] = fd;
}
#endif

/* ---- Reactor lifecycle ---- */

static mt_reactor_t *mt_reactor_new(void) {
  mt_reactor_t *r = (mt_reactor_t *)calloc(1, sizeof(mt_reactor_t));
  if (!r) return NULL;
  r->free_head  = UINT32_MAX;
  r->next_gen   = 1;
  r->generation = 1;

#if defined(MT_USE_KQUEUE)
  r->poll_fd = kqueue();
  if (r->poll_fd < 0) {
    free(r);
    return NULL;
  }
  struct kevent ev;
  EV_SET(&ev, MT_WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  kevent(r->poll_fd, &ev, 1, NULL, 0, NULL);
#elif defined(MT_USE_EPOLL)
  r->poll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (r->poll_fd < 0) {
    free(r);
    return NULL;
  }
  r->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (r->wake_fd < 0) {
    close(r->poll_fd);
    free(r);
    return NULL;
  }
  struct epoll_event ev;
  ev.events  = EPOLLIN;
  ev.data.fd = r->wake_fd;
  epoll_ctl(r->poll_fd, EPOLL_CTL_ADD, r->wake_fd, &ev);
#elif defined(MT_USE_POLLSET)
  r->poll_fd = pollset_create(-1);
  if (r->poll_fd < 0) {
    free(r);
    return NULL;
  }
  if (pipe(r->wake_pipe) < 0) {
    pollset_destroy(r->poll_fd);
    free(r);
    return NULL;
  }
  mt_set_nonblock_cloexec(r->wake_pipe[0]);
  mt_set_nonblock_cloexec(r->wake_pipe[1]);
  struct poll_ctl pc;
  pc.cmd    = PS_ADD;
  pc.events = POLLIN;
  pc.fd     = r->wake_pipe[0];
  pollset_ctl(r->poll_fd, &pc, 1);
#else /* MT_USE_POLL */
  r->poll_fd = -1;
  if (pipe(r->wake_pipe) < 0) {
    free(r);
    return NULL;
  }
  mt_set_nonblock_cloexec(r->wake_pipe[0]);
  mt_set_nonblock_cloexec(r->wake_pipe[1]);
#endif
  return r;
}

static void mt_reactor_clear_io(pTHX_ mt_reactor_t *r) {
  for (int fd = 0; fd < r->io_cap; fd++) {
    mt_io_watcher_t *w = &r->io[fd];
    if (!w->active && !w->reg_r && !w->reg_w) continue;
#if defined(MT_USE_KQUEUE)
    struct kevent ev[2];
    int n = 0;
    if (w->reg_r) EV_SET(&ev[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    if (w->reg_w) EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (n > 0) kevent(r->poll_fd, ev, n, NULL, 0, NULL);
#elif defined(MT_USE_EPOLL)
    if (w->reg_r || w->reg_w) epoll_ctl(r->poll_fd, EPOLL_CTL_DEL, fd, NULL);
#elif defined(MT_USE_POLLSET)
    if (w->reg_r || w->reg_w) {
      struct poll_ctl pc;
      pc.cmd    = PS_DELETE;
      pc.events = 0;
      pc.fd     = fd;
      pollset_ctl(r->poll_fd, &pc, 1);
    }
#endif
    if (w->cb) {
      SvREFCNT_dec(w->cb);
      w->cb = NULL;
    }
    w->active = 0;
    w->mode   = 0;
    w->reg_r  = 0;
    w->reg_w  = 0;
    w->dirty  = 0;
  }
#ifdef MT_USE_KQUEUE
  r->dirty_len = 0;
#endif
  r->io_count = 0;
}

static void mt_reactor_clear_timers(pTHX_ mt_reactor_t *r) {
  for (uint32_t i = 0; i < r->timer_cap; i++) {
    if (r->timers[i].active) {
      if (r->timers[i].cb) {
        SvREFCNT_dec(r->timers[i].cb);
        r->timers[i].cb = NULL;
      }
      r->timers[i].active    = 0;
      r->timers[i].heap_idx  = -1;
      r->timers[i].gen++;
      r->timers[i].next_free = r->free_head;
      r->free_head = i;
    }
  }
  r->timer_count = 0;
  r->heap_len    = 0;
}

static void mt_reactor_reset(pTHX_ mt_reactor_t *r) {
  if (!r) return;
  r->generation++;
  mt_reactor_clear_io(aTHX_ r);
  mt_reactor_clear_timers(aTHX_ r);
}

static void mt_reactor_destroy(pTHX_ mt_reactor_t *r) {
  if (!r) return;
  mt_reactor_clear_io(aTHX_ r);
  mt_reactor_clear_timers(aTHX_ r);
#if defined(MT_USE_POLLSET)
  if (r->poll_fd >= 0) pollset_destroy(r->poll_fd);
  if (r->wake_pipe[0] >= 0) close(r->wake_pipe[0]);
  if (r->wake_pipe[1] >= 0) close(r->wake_pipe[1]);
#elif defined(MT_USE_POLL)
  if (r->wake_pipe[0] >= 0) close(r->wake_pipe[0]);
  if (r->wake_pipe[1] >= 0) close(r->wake_pipe[1]);
  free(r->pfds);
#elif defined(MT_USE_EPOLL)
  if (r->poll_fd >= 0) close(r->poll_fd);
  if (r->wake_fd >= 0) close(r->wake_fd);
#else /* MT_USE_KQUEUE */
  if (r->poll_fd >= 0) close(r->poll_fd);
  free(r->dirty_fds);
#endif
  free(r->io);
  free(r->timers);
  free(r->heap);
  free(r);
}

/* ---- I/O Watchers ---- */

static int mt_reactor_ensure_fd(mt_reactor_t *r, int fd) {
  if (fd < 0) return 0;
  if (fd < r->io_cap) return 1;
  int ncap = r->io_cap ? r->io_cap : 64;
  while (ncap <= fd) ncap *= 2;
  mt_io_watcher_t *nio = (mt_io_watcher_t *)realloc(r->io, (size_t)ncap * sizeof(mt_io_watcher_t));
  if (!nio) return 0;
  memset(nio + r->io_cap, 0, (size_t)(ncap - r->io_cap) * sizeof(mt_io_watcher_t));
  r->io     = nio;
  r->io_cap = ncap;
  return 1;
}

static int mt_reactor_watch(mt_reactor_t *r, int fd, int want_r, int want_w) {
  if (fd < 0 || fd >= r->io_cap || !r->io[fd].active) return 0;
  mt_io_watcher_t *w = &r->io[fd];
  uint8_t nr = want_r ? 1 : 0;
  uint8_t nw = want_w ? 1 : 0;
  uint8_t nm = (uint8_t)((nr ? 1 : 0) | (nw ? 2 : 0));
  if (w->mode == nm && w->reg_r == nr && w->reg_w == nw) return 1;
  w->mode = nm;

#if defined(MT_USE_KQUEUE)
  /* Defer kqueue changes to the single kevent() call in mt_reactor_wait() */
  mt_mark_dirty(r, fd);
#elif defined(MT_USE_EPOLL)
  uint32_t events = 0;
  if (nr) events |= EPOLLIN | EPOLLPRI;
  if (nw) events |= EPOLLOUT;
  int was_reg = (w->reg_r || w->reg_w);
  if (events) {
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    int op = was_reg ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(r->poll_fd, op, fd, &ev) < 0 && op == EPOLL_CTL_MOD && errno == ENOENT) {
      epoll_ctl(r->poll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
  } else if (was_reg) {
    epoll_ctl(r->poll_fd, EPOLL_CTL_DEL, fd, NULL);
  }
  w->reg_r = nr;
  w->reg_w = nw;
#elif defined(MT_USE_POLLSET)
  short events = 0;
  if (nr) events |= POLLIN | POLLPRI;
  if (nw) events |= POLLOUT;
  int was_reg = (w->reg_r || w->reg_w);
  if (was_reg) {
    struct poll_ctl pc;
    pc.cmd    = PS_DELETE;
    pc.events = 0;
    pc.fd     = fd;
    pollset_ctl(r->poll_fd, &pc, 1);
  }
  if (events) {
    struct poll_ctl pc;
    pc.cmd    = PS_ADD;
    pc.events = events;
    pc.fd     = fd;
    pollset_ctl(r->poll_fd, &pc, 1);
  }
  w->reg_r = nr;
  w->reg_w = nw;
#else /* MT_USE_POLL */
  w->reg_r = nr;
  w->reg_w = nw;
#endif
  return 1;
}

static int mt_reactor_io(pTHX_ mt_reactor_t *r, int fd, SV *cb) {
  if (!mt_reactor_ensure_fd(r, fd)) return 0;
  mt_io_watcher_t *w = &r->io[fd];
  if (w->cb) SvREFCNT_dec(w->cb);
  w->cb = SvREFCNT_inc(cb);
  if (!w->active) {
    w->active = 1;
    w->reg_r  = 0; /* Handle reused fd cleanly; EV_ADD is idempotent if still open */
    w->reg_w  = 0;
    r->io_count++;
  }
  return mt_reactor_watch(r, fd, 1, 1);
}

static int mt_reactor_remove_io(pTHX_ mt_reactor_t *r, int fd) {
  if (fd < 0 || fd >= r->io_cap || !r->io[fd].active) return 0;
  mt_io_watcher_t *w = &r->io[fd];
#if defined(MT_USE_KQUEUE)
  w->mode = 0;
  if (w->reg_r || w->reg_w) {
    mt_mark_dirty(r, fd);
  }
#elif defined(MT_USE_EPOLL)
  if (w->reg_r || w->reg_w) epoll_ctl(r->poll_fd, EPOLL_CTL_DEL, fd, NULL);
  w->reg_r = 0;
  w->reg_w = 0;
#elif defined(MT_USE_POLLSET)
  if (w->reg_r || w->reg_w) {
    struct poll_ctl pc;
    pc.cmd    = PS_DELETE;
    pc.events = 0;
    pc.fd     = fd;
    pollset_ctl(r->poll_fd, &pc, 1);
  }
  w->reg_r = 0;
  w->reg_w = 0;
#else /* MT_USE_POLL */
  w->mode  = 0;
  w->reg_r = 0;
  w->reg_w = 0;
#endif
  if (w->cb) {
    SvREFCNT_dec(w->cb);
    w->cb = NULL;
  }
  w->active = 0;
  r->io_count--;
  return 1;
}

static SV *mt_reactor_get_io_cb(pTHX_ mt_reactor_t *r, uint64_t gen, int fd, int is_write) {
  if (r->generation != gen) return NULL;
  if (fd < 0 || fd >= r->io_cap) return NULL;
  mt_io_watcher_t *w = &r->io[fd];
  if (!w->active || !w->cb) return NULL;
  if (is_write && !(w->mode & 2)) return NULL;
  if (!is_write && !(w->mode & 1)) return NULL;
  return SvREFCNT_inc(w->cb);
}

/* ---- Timers ---- */

static inline uint64_t mt_make_timer_id(uint32_t gen, uint32_t idx) {
  return ((uint64_t)gen << 32) | (uint64_t)idx;
}

static uint64_t mt_reactor_add_timer(pTHX_ mt_reactor_t *r, int recurring, double after, SV *cb) {
  uint32_t idx;
  if (r->free_head != UINT32_MAX) {
    idx = r->free_head;
    r->free_head = r->timers[idx].next_free;
  } else {
    uint32_t old_cap = r->timer_cap;
    uint32_t ncap    = old_cap ? old_cap * 2 : 32;
    mt_timer_slot_t *nt = (mt_timer_slot_t *)realloc(r->timers, (size_t)ncap * sizeof(mt_timer_slot_t));
    if (!nt) return 0;
    for (uint32_t i = old_cap; i < ncap; i++) {
      nt[i].gen       = 1;
      nt[i].heap_idx  = -1;
      nt[i].cb        = NULL;
      nt[i].active    = 0;
      nt[i].recurring = 0;
      nt[i].next_free = (i + 1 < ncap) ? (i + 1) : UINT32_MAX;
    }
    r->timers    = nt;
    r->timer_cap = ncap;
    idx          = old_cap;
    r->free_head = (old_cap + 1 < ncap) ? (old_cap + 1) : UINT32_MAX;
  }

  mt_timer_slot_t *t = &r->timers[idx];
  t->gen++;
  if (t->gen == 0) t->gen = 1;
  t->after     = after;
  t->fire_at   = mt_monotonic_time() + after;
  t->cb        = cb ? SvREFCNT_inc(cb) : NULL;
  t->recurring = recurring ? 1 : 0;
  t->active    = 1;
  t->heap_idx  = -1;

  if (!mt_heap_push(r, idx)) {
    if (t->cb) { SvREFCNT_dec(t->cb); t->cb = NULL; }
    t->active = 0;
    t->next_free = r->free_head;
    r->free_head = idx;
    return 0;
  }
  r->timer_count++;
  return mt_make_timer_id(t->gen, idx);
}

static int mt_reactor_again(mt_reactor_t *r, uint64_t id, int has_after, double after) {
  uint32_t idx = (uint32_t)(id & 0xFFFFFFFFULL);
  uint32_t gen = (uint32_t)(id >> 32);
  if (idx >= r->timer_cap) return 0;
  mt_timer_slot_t *t = &r->timers[idx];
  if (!t->active || t->gen != gen) return 0;

  if (has_after) t->after = after;
  t->fire_at = mt_monotonic_time() + t->after;

  if (t->heap_idx >= 0) {
    mt_heap_sift_up(r, (uint32_t)t->heap_idx);
    mt_heap_sift_down(r, (uint32_t)t->heap_idx);
  } else {
    mt_heap_push(r, idx);
  }
  return 1;
}

static int mt_reactor_remove_timer(pTHX_ mt_reactor_t *r, uint64_t id) {
  uint32_t idx = (uint32_t)(id & 0xFFFFFFFFULL);
  uint32_t gen = (uint32_t)(id >> 32);
  if (idx >= r->timer_cap) return 0;
  mt_timer_slot_t *t = &r->timers[idx];
  if (!t->active || t->gen != gen) return 0;

  if (t->heap_idx >= 0) {
    mt_heap_remove_at(r, (uint32_t)t->heap_idx);
  }
  if (t->cb) {
    SvREFCNT_dec(t->cb);
    t->cb = NULL;
  }
  t->active = 0;
  t->gen++;
  t->next_free = r->free_head;
  r->free_head = idx;
  r->timer_count--;
  return 1;
}

/* Pop next due timer (fire_at <= now).
 * If recurring, records its id in reinsert_av so caller reinserts after loop.
 * Returns refcounted cb SV (caller must mortalize/dec) or NULL if none due.
 */
static SV *mt_reactor_pop_timer(pTHX_ mt_reactor_t *r, uint64_t gen, double now, AV *reinsert_av) {
  if (r->generation != gen) return NULL;
  while (r->heap_len > 0) {
    uint32_t idx = r->heap[0];
    mt_timer_slot_t *t = &r->timers[idx];
    if (t->fire_at > now) return NULL;

    mt_heap_remove_at(r, 0);
    SV *cb = t->cb ? SvREFCNT_inc(t->cb) : NULL;

    if (t->recurring) {
      t->fire_at = now + t->after;
      av_push(reinsert_av, newSVuv((UV)mt_make_timer_id(t->gen, idx)));
    } else {
      if (t->cb) {
        SvREFCNT_dec(t->cb);
        t->cb = NULL;
      }
      t->active = 0;
      t->gen++;
      t->next_free = r->free_head;
      r->free_head = idx;
      r->timer_count--;
    }

    if (cb) return cb;
  }
  return NULL;
}

static void mt_reactor_reinsert_timers(pTHX_ mt_reactor_t *r, uint64_t gen, AV *reinsert_av) {
  if (r->generation != gen) return;
  SSize_t len = av_len(reinsert_av) + 1;
  for (SSize_t i = 0; i < len; i++) {
    SV **svp = av_fetch(reinsert_av, i, 0);
    if (!svp || !*svp) continue;
    uint64_t id  = (uint64_t)SvUV(*svp);
    uint32_t idx = (uint32_t)(id & 0xFFFFFFFFULL);
    uint32_t tg  = (uint32_t)(id >> 32);
    if (idx < r->timer_cap && r->timers[idx].active &&
        r->timers[idx].gen == tg && r->timers[idx].heap_idx < 0) {
      mt_heap_push(r, idx);
    }
  }
}

/* ---- Wakeup & Event Wait ---- */

static void mt_reactor_wake(mt_reactor_t *r) {
  if (!r) return;
#if defined(MT_USE_KQUEUE)
  if (r->poll_fd < 0) return;
  struct kevent ev;
  EV_SET(&ev, MT_WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(r->poll_fd, &ev, 1, NULL, 0, NULL);
#elif defined(MT_USE_EPOLL)
  if (r->wake_fd < 0) return;
  uint64_t one = 1;
  ssize_t ignored = write(r->wake_fd, &one, sizeof(one));
  (void)ignored;
#else /* MT_USE_POLLSET || MT_USE_POLL */
  if (r->wake_pipe[1] < 0) return;
  char b = 1;
  ssize_t ignored = write(r->wake_pipe[1], &b, 1);
  (void)ignored;
#endif
}

/* Wait for I/O or timer timeout or wake().
 * Flushes batched kqueue changelist in the same kevent() syscall.
 * Appends (fd, is_write) pairs to ready_av.
 * Sets *woke_out = 1 if EVFILT_USER/eventfd triggered.
 * Sets *due_out = 1 if any timer is due now.
 */
static int mt_reactor_wait(pTHX_ mt_reactor_t *r, AV *ready_av, int *woke_out, int *due_out) {
  *woke_out = 0;
  *due_out  = 0;
  av_clear(ready_av);

  double now = 0.0;
  double timeout = 0.5;
  if (r->heap_len > 0) {
    now = mt_monotonic_time();
    double diff = r->timers[r->heap[0]].fire_at - now;
    timeout = (diff <= 0.0) ? 0.0 : diff;
  }

  int count = 0;

#ifdef MT_USE_KQUEUE
  struct kevent change_stack[256];
  struct kevent *changes = change_stack;
  int nchanges = 0;
  if (r->dirty_len > 0) {
    int max_c = r->dirty_len * 2;
    if (max_c > 256) {
      changes = (struct kevent *)malloc((size_t)max_c * sizeof(struct kevent));
    }
    if (changes) {
      for (int i = 0; i < r->dirty_len; i++) {
        int fd = r->dirty_fds[i];
        if (fd < 0 || fd >= r->io_cap) continue;
        mt_io_watcher_t *w = &r->io[fd];
        w->dirty = 0;
        uint8_t want_r = (w->active && (w->mode & 1)) ? 1 : 0;
        uint8_t want_w = (w->active && (w->mode & 2)) ? 1 : 0;
        if (want_r != w->reg_r) {
          EV_SET(&changes[nchanges++], fd, EVFILT_READ,
                 want_r ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
          w->reg_r = want_r;
        }
        if (want_w != w->reg_w) {
          EV_SET(&changes[nchanges++], fd, EVFILT_WRITE,
                 want_w ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
          w->reg_w = want_w;
        }
      }
    }
    r->dirty_len = 0;
  }

  struct kevent events[MT_REACTOR_MAX_EVENTS];
  struct timespec ts;
  ts.tv_sec  = (time_t)timeout;
  ts.tv_nsec = (long)((timeout - (double)ts.tv_sec) * 1e9);
  if (ts.tv_nsec < 0) ts.tv_nsec = 0;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

  int n = kevent(r->poll_fd, nchanges ? changes : NULL, nchanges,
                 events, MT_REACTOR_MAX_EVENTS, &ts);
  if (changes != change_stack) free(changes);

  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (events[i].flags & EV_ERROR) continue;
      if (events[i].filter == EVFILT_USER) {
        *woke_out = 1;
        continue;
      }
      int fd = (int)events[i].ident;
      if (events[i].filter == EVFILT_READ) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(0));
        count++;
      } else if (events[i].filter == EVFILT_WRITE) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(1));
        count++;
      }
    }
  }
#elif defined(MT_USE_EPOLL)
  struct epoll_event events[MT_REACTOR_MAX_EVENTS];
  int ms = (timeout <= 0.0) ? 0 : (int)(timeout * 1000.0) + 1;
  int n = epoll_wait(r->poll_fd, events, MT_REACTOR_MAX_EVENTS, ms);
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == r->wake_fd) {
        uint64_t val;
        while (read(r->wake_fd, &val, sizeof(val)) > 0) {}
        *woke_out = 1;
        continue;
      }
      uint32_t ev = events[i].events;
      if (ev & (EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP)) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(0));
        count++;
      }
      if (ev & EPOLLOUT) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(1));
        count++;
      }
    }
  }
#elif defined(MT_USE_POLLSET)
  struct pollfd events[MT_REACTOR_MAX_EVENTS];
  int ms = (timeout <= 0.0) ? 0 : (int)(timeout * 1000.0) + 1;
  int n = pollset_poll(r->poll_fd, events, MT_REACTOR_MAX_EVENTS, ms);
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      int fd = events[i].fd;
      if (fd == r->wake_pipe[0]) {
        char drain[64];
        while (read(r->wake_pipe[0], drain, sizeof(drain)) > 0) {}
        *woke_out = 1;
        continue;
      }
      short rev = events[i].revents;
      if (rev & (POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL)) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(0));
        count++;
      }
      if (rev & POLLOUT) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(1));
        count++;
      }
    }
  }
#else /* MT_USE_POLL */
  int need = r->io_count + 1;
  if (need > r->pfds_cap) {
    int ncap = r->pfds_cap ? r->pfds_cap : 64;
    while (ncap < need) ncap *= 2;
    struct pollfd *np = (struct pollfd *)realloc(r->pfds, (size_t)ncap * sizeof(struct pollfd));
    if (!np) return 0;
    r->pfds     = np;
    r->pfds_cap = ncap;
  }
  int nfds = 0;
  r->pfds[nfds].fd      = r->wake_pipe[0];
  r->pfds[nfds].events  = POLLIN;
  r->pfds[nfds].revents = 0;
  nfds++;
  for (int fd = 0; fd < r->io_cap; fd++) {
    mt_io_watcher_t *w = &r->io[fd];
    if (!w->active || !w->mode) continue;
    short ev = 0;
    if (w->mode & 1) ev |= POLLIN | POLLPRI;
    if (w->mode & 2) ev |= POLLOUT;
    r->pfds[nfds].fd      = fd;
    r->pfds[nfds].events  = ev;
    r->pfds[nfds].revents = 0;
    nfds++;
  }
  int ms = (timeout <= 0.0) ? 0 : (int)(timeout * 1000.0) + 1;
  int n = poll(r->pfds, (nfds_t)nfds, ms);
  if (n > 0) {
    if (r->pfds[0].revents & POLLIN) {
      char drain[64];
      while (read(r->wake_pipe[0], drain, sizeof(drain)) > 0) {}
      *woke_out = 1;
    }
    for (int i = 1; i < nfds; i++) {
      short rev = r->pfds[i].revents;
      if (!rev) continue;
      int fd = r->pfds[i].fd;
      if (rev & (POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL)) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(0));
        count++;
      }
      if (rev & POLLOUT) {
        av_push(ready_av, newSViv(fd));
        av_push(ready_av, newSViv(1));
        count++;
      }
    }
  }
#endif

  if (r->heap_len > 0) {
    if (r->timers[r->heap[0]].fire_at <= mt_monotonic_time()) {
      *due_out = 1;
    }
  }

  return count;
}

#endif /* MOJO_MT_REACTOR_H */
