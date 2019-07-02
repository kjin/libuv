/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* We lean on the fact that POLL{IN,OUT,ERR,HUP} correspond with their
 * EPOLL* counterparts.  We use the POLL* variants in this file because that
 * is what libuv uses elsewhere.
 */

#include "uv.h"
#include "internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define HAVE_IFADDRS_H 1

#ifdef __UCLIBC__
# if __UCLIBC_MAJOR__ < 0 && __UCLIBC_MINOR__ < 9 && __UCLIBC_SUBLEVEL__ < 32
#  undef HAVE_IFADDRS_H
# endif
#endif

#ifdef HAVE_IFADDRS_H
# if defined(__ANDROID__)
#  include "uv/android-ifaddrs.h"
# else
#  include <ifaddrs.h>
# endif
# include <sys/socket.h>
# include <net/ethernet.h>
# include <netpacket/packet.h>
#endif /* HAVE_IFADDRS_H */

/* Available from 2.6.32 onwards. */
#ifndef CLOCK_MONOTONIC_COARSE
# define CLOCK_MONOTONIC_COARSE 6
#endif

/* This is rather annoying: CLOCK_BOOTTIME lives in <linux/time.h> but we can't
 * include that file because it conflicts with <time.h>. We'll just have to
 * define it ourselves.
 */
#ifndef CLOCK_BOOTTIME
# define CLOCK_BOOTTIME 7
#endif

static int read_models(unsigned int numcpus, uv_cpu_info_t* ci);
static int read_times(FILE* statfile_fp,
                      unsigned int numcpus,
                      uv_cpu_info_t* ci);
static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci);
static uint64_t read_cpufreq(unsigned int cpunum);


int uv__platform_loop_init(uv_loop_t* loop) {
  int fd;

  /* It was reported that EPOLL_CLOEXEC is not defined on Android API < 21,
   * a.k.a. Lollipop. Since EPOLL_CLOEXEC is an alias for O_CLOEXEC on all
   * architectures, we just use that instead.
   */
  fd = epoll_create1(O_CLOEXEC);

  /* epoll_create1() can fail either because it's not implemented (old kernel)
   * or because it doesn't understand the O_CLOEXEC flag.
   */
  if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
    fd = epoll_create(256);

    if (fd != -1)
      uv__cloexec(fd, 1);
  }

  loop->backend_fd = fd;
  loop->inotify_fd = -1;
  loop->inotify_watchers = NULL;

  if (fd == -1)
    return UV__ERR(errno);

  return 0;
}


int uv__io_fork(uv_loop_t* loop) {
  int err;
  void* old_watchers;

  old_watchers = loop->inotify_watchers;

  uv__close(loop->backend_fd);
  loop->backend_fd = -1;
  uv__platform_loop_delete(loop);

  err = uv__platform_loop_init(loop);
  if (err)
    return err;

  return uv__inotify_fork(loop, old_watchers);
}


void uv__platform_loop_delete(uv_loop_t* loop) {
  if (loop->inotify_fd == -1) return;
  uv__io_stop(loop, &loop->inotify_read_watcher, POLLIN);
  uv__close(loop->inotify_fd);
  loop->inotify_fd = -1;
}


void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
  struct epoll_event* events;
  struct epoll_event dummy;
  uintptr_t i;
  uintptr_t nfds;

  assert(loop->watchers != NULL);
  assert(fd >= 0);

  events = (struct epoll_event*) loop->watchers[loop->nwatchers];
  nfds = (uintptr_t) loop->watchers[loop->nwatchers + 1];
  if (events != NULL)
    /* Invalidate events with same file descriptor */
    for (i = 0; i < nfds; i++)
      if (events[i].data.fd == fd)
        events[i].data.fd = -1;

  /* Remove the file descriptor from the epoll.
   * This avoids a problem where the same file description remains open
   * in another process, causing repeated junk epoll events.
   *
   * We pass in a dummy epoll_event, to work around a bug in old kernels.
   */
  if (loop->backend_fd >= 0) {
    /* Work around a bug in kernels 3.10 to 3.19 where passing a struct that
     * has the EPOLLWAKEUP flag set generates spurious audit syslog warnings.
     */
    memset(&dummy, 0, sizeof(dummy));
    epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &dummy);
  }
}


int uv__io_check_fd(uv_loop_t* loop, int fd) {
  struct epoll_event e;
  int rc;

  memset(&e, 0, sizeof(e));
  e.events = POLLIN;
  e.data.fd = -1;

  rc = 0;
  if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &e))
    if (errno != EEXIST)
      rc = UV__ERR(errno);

  if (rc == 0)
    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &e))
      abort();

  return rc;
}


void uv__io_poll(uv_loop_t* loop, int timeout) {
  /* A bug in kernels < 2.6.37 makes timeouts larger than ~30 minutes
   * effectively infinite on 32 bits architectures.  To avoid blocking
   * indefinitely, we cap the timeout and poll again if necessary.
   *
   * Note that "30 minutes" is a simplification because it depends on
   * the value of CONFIG_HZ.  The magic constant assumes CONFIG_HZ=1200,
   * that being the largest value I have seen in the wild (and only once.)
   */
  static const int max_safe_timeout = 1789569;
  struct epoll_event events[1024];
  struct epoll_event* pe;
  struct epoll_event e;
  int real_timeout;
  QUEUE* q;
  uv__io_t* w;
  sigset_t sigset;
  sigset_t* psigset;
  uint64_t base;
  int have_signals;
  int nevents;
  int count;
  int nfds;
  int fd;
  int op;
  int i;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  memset(&e, 0, sizeof(e));

  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);
    assert(w->fd >= 0);
    assert(w->fd < (int) loop->nwatchers);

    e.events = w->pevents;
    e.data.fd = w->fd;

    if (w->events == 0)
      op = EPOLL_CTL_ADD;
    else
      op = EPOLL_CTL_MOD;

    /* XXX Future optimization: do EPOLL_CTL_MOD lazily if we stop watching
     * events, skip the syscall and squelch the events after epoll_wait().
     */
    if (epoll_ctl(loop->backend_fd, op, w->fd, &e)) {
      if (errno != EEXIST)
        abort();

      assert(op == EPOLL_CTL_ADD);

      /* We've reactivated a file descriptor that's been watched before. */
      if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, w->fd, &e))
        abort();
    }

    w->events = w->pevents;
  }

  psigset = NULL;
  if (loop->flags & UV_LOOP_BLOCK_SIGPROF) {
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    psigset = &sigset;
  }

  assert(timeout >= -1);
  base = loop->time;
  count = 48; /* Benchmarks suggest this gives the best throughput. */
  real_timeout = timeout;

  for (;;) {
    /* See the comment for max_safe_timeout for an explanation of why
     * this is necessary.  Executive summary: kernel bug workaround.
     */
    if (sizeof(int32_t) == sizeof(long) && timeout >= max_safe_timeout)
      timeout = max_safe_timeout;

    nfds = epoll_pwait(loop->backend_fd,
                       events,
                       ARRAY_SIZE(events),
                       timeout,
                       psigset);

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     */
    SAVE_ERRNO(uv__update_time(loop));

    if (nfds == 0) {
      assert(timeout != -1);

      if (timeout == 0)
        return;

      /* We may have been inside the system call for longer than |timeout|
       * milliseconds so we need to update the timestamp to avoid drift.
       */
      goto update_timeout;
    }

    if (nfds == -1) {
      if (errno != EINTR)
        abort();

      if (timeout == -1)
        continue;

      if (timeout == 0)
        return;

      /* Interrupted by a signal. Update timeout and poll again. */
      goto update_timeout;
    }

    have_signals = 0;
    nevents = 0;

    assert(loop->watchers != NULL);
    loop->watchers[loop->nwatchers] = (void*) events;
    loop->watchers[loop->nwatchers + 1] = (void*) (uintptr_t) nfds;
    for (i = 0; i < nfds; i++) {
      pe = events + i;
      fd = pe->data.fd;

      /* Skip invalidated events, see uv__platform_invalidate_fd */
      if (fd == -1)
        continue;

      assert(fd >= 0);
      assert((unsigned) fd < loop->nwatchers);

      w = loop->watchers[fd];

      if (w == NULL) {
        /* File descriptor that we've stopped watching, disarm it.
         *
         * Ignore all errors because we may be racing with another thread
         * when the file descriptor is closed.
         */
        epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, pe);
        continue;
      }

      /* Give users only events they're interested in. Prevents spurious
       * callbacks when previous callback invocation in this loop has stopped
       * the current watcher. Also, filters out events that users has not
       * requested us to watch.
       */
      pe->events &= w->pevents | POLLERR | POLLHUP;

      /* Work around an epoll quirk where it sometimes reports just the
       * EPOLLERR or EPOLLHUP event.  In order to force the event loop to
       * move forward, we merge in the read/write events that the watcher
       * is interested in; uv__read() and uv__write() will then deal with
       * the error or hangup in the usual fashion.
       *
       * Note to self: happens when epoll reports EPOLLIN|EPOLLHUP, the user
       * reads the available data, calls uv_read_stop(), then sometime later
       * calls uv_read_start() again.  By then, libuv has forgotten about the
       * hangup and the kernel won't report EPOLLIN again because there's
       * nothing left to read.  If anything, libuv is to blame here.  The
       * current hack is just a quick bandaid; to properly fix it, libuv
       * needs to remember the error/hangup event.  We should get that for
       * free when we switch over to edge-triggered I/O.
       */
      if (pe->events == POLLERR || pe->events == POLLHUP)
        pe->events |=
          w->pevents & (POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI);

      if (pe->events != 0) {
        /* Run signal watchers last.  This also affects child process watchers
         * because those are implemented in terms of signal watchers.
         */
        if (w == &loop->signal_io_watcher)
          have_signals = 1;
        else
          w->cb(loop, w, pe->events);

        nevents++;
      }
    }

    if (have_signals != 0)
      loop->signal_io_watcher.cb(loop, &loop->signal_io_watcher, POLLIN);

    loop->watchers[loop->nwatchers] = NULL;
    loop->watchers[loop->nwatchers + 1] = NULL;

    if (have_signals != 0)
      return;  /* Event loop should cycle now so don't poll again. */

    if (nevents != 0) {
      if (nfds == ARRAY_SIZE(events) && --count != 0) {
        /* Poll for more events but don't block this time. */
        timeout = 0;
        continue;
      }
      return;
    }

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);

    real_timeout -= (loop->time - base);
    if (real_timeout <= 0)
      return;

    timeout = real_timeout;
  }
}


uint64_t uv__hrtime(uv_clocktype_t type) {
  static clock_t fast_clock_id = -1;
  struct timespec t;
  clock_t clock_id;

  /* Prefer CLOCK_MONOTONIC_COARSE if available but only when it has
   * millisecond granularity or better.  CLOCK_MONOTONIC_COARSE is
   * serviced entirely from the vDSO, whereas CLOCK_MONOTONIC may
   * decide to make a costly system call.
   */
  /* TODO(bnoordhuis) Use CLOCK_MONOTONIC_COARSE for UV_CLOCK_PRECISE
   * when it has microsecond granularity or better (unlikely).
   */
  if (type == UV_CLOCK_FAST && fast_clock_id == -1) {
    if (clock_getres(CLOCK_MONOTONIC_COARSE, &t) == 0 &&
        t.tv_nsec <= 1 * 1000 * 1000) {
      fast_clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
      fast_clock_id = CLOCK_MONOTONIC;
    }
  }

  clock_id = CLOCK_MONOTONIC;
  if (type == UV_CLOCK_FAST)
    clock_id = fast_clock_id;

  if (clock_gettime(clock_id, &t))
    return 0;  /* Not really possible. */

  return t.tv_sec * (uint64_t) 1e9 + t.tv_nsec;
}


int uv_resident_set_memory(size_t* rss) {
  char buf[1024];
  const char* s;
  ssize_t n;
  long val;
  int fd;
  int i;

  do
    fd = open("/proc/self/stat", O_RDONLY);
  while (fd == -1 && errno == EINTR);

  if (fd == -1)
    return UV__ERR(errno);

  do
    n = read(fd, buf, sizeof(buf) - 1);
  while (n == -1 && errno == EINTR);

  uv__close(fd);
  if (n == -1)
    return UV__ERR(errno);
  buf[n] = '\0';

  s = strchr(buf, ' ');
  if (s == NULL)
    goto err;

  s += 1;
  if (*s != '(')
    goto err;

  s = strchr(s, ')');
  if (s == NULL)
    goto err;

  for (i = 1; i <= 22; i++) {
    s = strchr(s + 1, ' ');
    if (s == NULL)
      goto err;
  }

  errno = 0;
  val = strtol(s, NULL, 10);
  if (errno != 0)
    goto err;
  if (val < 0)
    goto err;

  *rss = val * getpagesize();
  return 0;

err:
  return UV_EINVAL;
}


int uv_uptime(double* uptime) {
  static volatile int no_clock_boottime;
  struct timespec now;
  int r;

  /* Try CLOCK_BOOTTIME first, fall back to CLOCK_MONOTONIC if not available
   * (pre-2.6.39 kernels). CLOCK_MONOTONIC doesn't increase when the system
   * is suspended.
   */
  if (no_clock_boottime) {
    retry: r = clock_gettime(CLOCK_MONOTONIC, &now);
  }
  else if ((r = clock_gettime(CLOCK_BOOTTIME, &now)) && errno == EINVAL) {
    no_clock_boottime = 1;
    goto retry;
  }

  if (r)
    return UV__ERR(errno);

  *uptime = now.tv_sec;
  return 0;
}


static int uv__cpu_num(FILE* statfile_fp, unsigned int* numcpus) {
  unsigned int num;
  char buf[1024];

  if (!fgets(buf, sizeof(buf), statfile_fp))
    return UV_EIO;

  num = 0;
  while (fgets(buf, sizeof(buf), statfile_fp)) {
    if (strncmp(buf, "cpu", 3))
      break;
    num++;
  }

  if (num == 0)
    return UV_EIO;

  *numcpus = num;
  return 0;
}


int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  unsigned int numcpus;
  uv_cpu_info_t* ci;
  int err;
  FILE* statfile_fp;

  *cpu_infos = NULL;
  *count = 0;

  statfile_fp = uv__open_file("/proc/stat");
  if (statfile_fp == NULL)
    return UV__ERR(errno);

  err = uv__cpu_num(statfile_fp, &numcpus);
  if (err < 0)
    goto out;

  err = UV_ENOMEM;
  ci = uv__calloc(numcpus, sizeof(*ci));
  if (ci == NULL)
    goto out;

  err = read_models(numcpus, ci);
  if (err == 0)
    err = read_times(statfile_fp, numcpus, ci);

  if (err) {
    uv_free_cpu_info(ci, numcpus);
    goto out;
  }

  /* read_models() on x86 also reads the CPU speed from /proc/cpuinfo.
   * We don't check for errors here. Worst case, the field is left zero.
   */
  if (ci[0].speed == 0)
    read_speeds(numcpus, ci);

  *cpu_infos = ci;
  *count = numcpus;
  err = 0;

out:

  if (fclose(statfile_fp))
    if (errno != EINTR && errno != EINPROGRESS)
      abort();

  return err;
}


static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci) {
  unsigned int num;

  for (num = 0; num < numcpus; num++)
    ci[num].speed = read_cpufreq(num) / 1000;
}


/* Also reads the CPU frequency on x86. The other architectures only have
 * a BogoMIPS field, which may not be very accurate.
 *
 * Note: Simply returns on error, uv_cpu_info() takes care of the cleanup.
 */
static int read_models(unsigned int numcpus, uv_cpu_info_t* ci) {
  static const char model_marker[] = "model name\t: ";
  static const char speed_marker[] = "cpu MHz\t\t: ";
  const char* inferred_model;
  unsigned int model_idx;
  unsigned int speed_idx;
  char buf[1024];
  char* model;
  FILE* fp;

  /* Most are unused on non-ARM, non-MIPS and non-x86 architectures. */
  (void) &model_marker;
  (void) &speed_marker;
  (void) &speed_idx;
  (void) &model;
  (void) &buf;
  (void) &fp;

  model_idx = 0;
  speed_idx = 0;

#if defined(__arm__) || \
    defined(__i386__) || \
    defined(__mips__) || \
    defined(__x86_64__)
  fp = uv__open_file("/proc/cpuinfo");
  if (fp == NULL)
    return UV__ERR(errno);

  while (fgets(buf, sizeof(buf), fp)) {
    if (model_idx < numcpus) {
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = uv__strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return UV_ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#if defined(__arm__) || defined(__mips__)
    if (model_idx < numcpus) {
#if defined(__arm__)
      /* Fallback for pre-3.8 kernels. */
      static const char model_marker[] = "Processor\t: ";
#else	/* defined(__mips__) */
      static const char model_marker[] = "cpu model\t\t: ";
#endif
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = uv__strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return UV_ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#else  /* !__arm__ && !__mips__ */
    if (speed_idx < numcpus) {
      if (strncmp(buf, speed_marker, sizeof(speed_marker) - 1) == 0) {
        ci[speed_idx++].speed = atoi(buf + sizeof(speed_marker) - 1);
        continue;
      }
    }
#endif  /* __arm__ || __mips__ */
  }

  fclose(fp);
#endif  /* __arm__ || __i386__ || __mips__ || __x86_64__ */

  /* Now we want to make sure that all the models contain *something* because
   * it's not safe to leave them as null. Copy the last entry unless there
   * isn't one, in that case we simply put "unknown" into everything.
   */
  inferred_model = "unknown";
  if (model_idx > 0)
    inferred_model = ci[model_idx - 1].model;

  while (model_idx < numcpus) {
    model = uv__strndup(inferred_model, strlen(inferred_model));
    if (model == NULL)
      return UV_ENOMEM;
    ci[model_idx++].model = model;
  }

  return 0;
}


static int read_times(FILE* statfile_fp,
                      unsigned int numcpus,
                      uv_cpu_info_t* ci) {
  struct uv_cpu_times_s ts;
  uint64_t clock_ticks;
  uint64_t user;
  uint64_t nice;
  uint64_t sys;
  uint64_t idle;
  uint64_t dummy;
  uint64_t irq;
  uint64_t num;
  uint64_t len;
  char buf[1024];

  clock_ticks = sysconf(_SC_CLK_TCK);
  assert(clock_ticks != (uint64_t) -1);
  assert(clock_ticks != 0);

  rewind(statfile_fp);

  if (!fgets(buf, sizeof(buf), statfile_fp))
    abort();

  num = 0;

  while (fgets(buf, sizeof(buf), statfile_fp)) {
    if (num >= numcpus)
      break;

    if (strncmp(buf, "cpu", 3))
      break;

    /* skip "cpu<num> " marker */
    {
      unsigned int n;
      int r = sscanf(buf, "cpu%u ", &n);
      assert(r == 1);
      (void) r;  /* silence build warning */
      for (len = sizeof("cpu0"); n /= 10; len++);
    }

    /* Line contains user, nice, system, idle, iowait, irq, softirq, steal,
     * guest, guest_nice but we're only interested in the first four + irq.
     *
     * Don't use %*s to skip fields or %ll to read straight into the uint64_t
     * fields, they're not allowed in C89 mode.
     */
    if (6 != sscanf(buf + len,
                    "%" PRIu64 " %" PRIu64 " %" PRIu64
                    "%" PRIu64 " %" PRIu64 " %" PRIu64,
                    &user,
                    &nice,
                    &sys,
                    &idle,
                    &dummy,
                    &irq))
      abort();

    ts.user = clock_ticks * user;
    ts.nice = clock_ticks * nice;
    ts.sys  = clock_ticks * sys;
    ts.idle = clock_ticks * idle;
    ts.irq  = clock_ticks * irq;
    ci[num++].cpu_times = ts;
  }
  assert(num == numcpus);

  return 0;
}


static uint64_t read_cpufreq(unsigned int cpunum) {
  uint64_t val;
  char buf[1024];
  FILE* fp;

  snprintf(buf,
           sizeof(buf),
           "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
           cpunum);

  fp = uv__open_file(buf);
  if (fp == NULL)
    return 0;

  if (fscanf(fp, "%" PRIu64, &val) != 1)
    val = 0;

  fclose(fp);

  return val;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    uv__free(cpu_infos[i].model);
  }

  uv__free(cpu_infos);
}

static int uv__ifaddr_exclude(struct ifaddrs *ent, int exclude_type) {
  if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)))
    return 1;
  if (ent->ifa_addr == NULL)
    return 1;
  /*
   * On Linux getifaddrs returns information related to the raw underlying
   * devices. We're not interested in this information yet.
   */
  if (ent->ifa_addr->sa_family == PF_PACKET)
    return exclude_type;
  return !exclude_type;
}

int uv_interface_addresses(uv_interface_address_t** addresses, int* count) {
#ifndef HAVE_IFADDRS_H
  *count = 0;
  *addresses = NULL;
  return UV_ENOSYS;
#else
  struct ifaddrs *addrs, *ent;
  uv_interface_address_t* address;
  int i;
  struct sockaddr_ll *sll;

  *count = 0;
  *addresses = NULL;

  if (getifaddrs(&addrs))
    return UV__ERR(errno);

  /* Count the number of interfaces */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFADDR))
      continue;

    (*count)++;
  }

  if (*count == 0) {
    freeifaddrs(addrs);
    return 0;
  }

  /* Make sure the memory is initiallized to zero using calloc() */
  *addresses = uv__calloc(*count, sizeof(**addresses));
  if (!(*addresses)) {
    freeifaddrs(addrs);
    return UV_ENOMEM;
  }

  address = *addresses;

  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFADDR))
      continue;

    address->name = uv__strdup(ent->ifa_name);

    if (ent->ifa_addr->sa_family == AF_INET6) {
      address->address.address6 = *((struct sockaddr_in6*) ent->ifa_addr);
    } else {
      address->address.address4 = *((struct sockaddr_in*) ent->ifa_addr);
    }

    if (ent->ifa_netmask->sa_family == AF_INET6) {
      address->netmask.netmask6 = *((struct sockaddr_in6*) ent->ifa_netmask);
    } else {
      address->netmask.netmask4 = *((struct sockaddr_in*) ent->ifa_netmask);
    }

    address->is_internal = !!(ent->ifa_flags & IFF_LOOPBACK);

    address++;
  }

  /* Fill in physical addresses for each interface */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFPHYS))
      continue;

    address = *addresses;

    for (i = 0; i < (*count); i++) {
      size_t namelen = strlen(ent->ifa_name);
      /* Alias interface share the same physical address */
      if (strncmp(address->name, ent->ifa_name, namelen) == 0 &&
          (address->name[namelen] == 0 || address->name[namelen] == ':')) {
        sll = (struct sockaddr_ll*)ent->ifa_addr;
        memcpy(address->phys_addr, sll->sll_addr, sizeof(address->phys_addr));
      }
      address++;
    }
  }

  freeifaddrs(addrs);

  return 0;
#endif
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
  int count) {
  int i;

  for (i = 0; i < count; i++) {
    uv__free(addresses[i].name);
  }

  uv__free(addresses);
}


void uv__set_process_title(const char* title) {
#if defined(PR_SET_NAME)
  prctl(PR_SET_NAME, title);  /* Only copies first 16 characters. */
#endif
}


static uint64_t uv__read_proc_meminfo(const char* what) {
  uint64_t rc;
  ssize_t n;
  char* p;
  int fd;
  char buf[4096];  /* Large enough to hold all of /proc/meminfo. */

  rc = 0;
  fd = uv__open_cloexec("/proc/meminfo", O_RDONLY);

  if (fd == -1)
    return 0;

  n = read(fd, buf, sizeof(buf) - 1);

  if (n <= 0)
    goto out;

  buf[n] = '\0';
  p = strstr(buf, what);

  if (p == NULL)
    goto out;

  p += strlen(what);

  if (1 != sscanf(p, "%" PRIu64 " kB", &rc))
    goto out;

  rc *= 1024;

out:

  if (uv__close_nocheckstdio(fd))
    abort();

  return rc;
}


uint64_t uv_get_free_memory(void) {
  struct sysinfo info;
  uint64_t rc;

  rc = uv__read_proc_meminfo("MemFree:");

  if (rc != 0)
    return rc;

  if (0 == sysinfo(&info))
    return (uint64_t) info.freeram * info.mem_unit;

  return 0;
}


uint64_t uv_get_total_memory(void) {
  struct sysinfo info;
  uint64_t rc;

  rc = uv__read_proc_meminfo("MemTotal:");

  if (rc != 0)
    return rc;

  if (0 == sysinfo(&info))
    return (uint64_t) info.totalram * info.mem_unit;

  return 0;
}


#define PROC_SELF_MOUNTINFO "/proc/self/mountinfo"
#define PROC_SELF_CGROUP "/proc/self/cgroup"
#define CGROUPS_VERSION_UNKNOWN 0x0
#define CGROUPS_VERSION_1 0x1
#define CGROUPS_VERSION_2 0x2
#define CGROUPS_V2_CTRL_PREFIX "0::"
#define CGROUPS_V2_CTRL_PREFIX_LEN 3
#define CGROUPS_V2_NO_LIMIT_VALUE "max"
#define CGROUPS_BUF_SIZE 16384


/*
 * Holds information about a given cgroups subsystem.
 */
typedef struct {
  /* cgroups version, one of CGROUPS_VERSION_*. */
  uint8_t cgroups_version;
  /* Path in which param files are found for a subsystem. */
  char* path;
} uv__cgroups_subsystem_info_t;


/*
 * Given a `separator`-delimited string `haystack`, return 1 if `needle` exactly
 * matches at least one of the elements in that sequence.
 */
static int uv__find_in_delimited_string(const char* haystack, const char* needle, char separator) {
  char* haystack_mutable;
  char* candidate;
  char* haystack_ptr;
  if (needle == NULL || strlen(needle) > strlen(haystack))
    return 0;
  haystack_mutable = malloc(strlen(haystack) + 1);
  strcpy(haystack_mutable, haystack);
  haystack_ptr = haystack_mutable;
  do {
    candidate = strsep(&haystack_ptr, &separator);
    if (!strcmp(candidate, needle)) {
      free(haystack_mutable);
      return 1;
    }
  } while (haystack_ptr != NULL);
  free(haystack_mutable);
  return 0;
}


/**
 * Reads a line from `fp` into `buf`. (A line ends in \n or EOF.) The trailing
 * newline will be replaced with a \0 if it exists.
 *
 * If a line exceeds `buf_size`, `buf` will contain the first `buf_size`
 * characters, and `fp` will be positioned at the beginning of the next line,
 * instead of offset-`buf_size` into the current line (in other words anything
 * after the first `buf_size` characters will be lost).
 *
 * Returns 0 if the entire line is read into `buf`, a positive number if it was
 * truncated, and UV_EIO if reading the file using fgets returns NULL.
 */
static int uv__read_line(char* buf, size_t buf_size, FILE* fp) {
  if (NULL == fgets(buf, CGROUPS_BUF_SIZE, fp)) {
    return UV_EIO;
  }
  if ('\n' != buf[strlen(buf) - 1]) {
    /* We know here that `buf` doesn't end in a newline. */
    /* Scan to the beginning of the next line. */
    fscanf(fp, "%*[^\n]");
    fscanf(fp, "%*[\n]");
    return 1;
  }
  /* Remove the newline at the end. */
  buf[strlen(buf) - 1] = '\0';
  return 0;
}


/*
 * Read /proc/self/mountinfo and /proc/self/cgroup to get info about how the
 * given subsystem is controlled for this process, and the path to the
 * subsystem's parameter files, if possible.
 * 
 * If info->cgroups_version == CGROUPS_VERSION_UNKNOWN, info->path will be NULL.
 * Otherwise, info->path will be a heap pointer and the caller is responsible
 * for freeing it.
 */
static int uv__read_cgroups_proc_files(uv__cgroups_subsystem_info_t* info, const char* subsystem) {
  FILE* fp;
  /*
   * See description of /proc/[pid]/mountinfo in proc(5).
   * Henceforth, numbers in parentheses refer to fields from the docs.
   */
  char* field_ptr;
  char root[PATH_MAX]; /* (4) */
  char mount_point[PATH_MAX]; /* (5) */
  /* From /proc/self/cgroup */
  char hierarchy_path[PATH_MAX];
  char* subsystem_search_string = NULL;

  info->cgroups_version = CGROUPS_VERSION_UNKNOWN;
  info->path = NULL;

  /* Read /proc/self/mountinfo to get controller path. */

  fp = uv__open_file(PROC_SELF_MOUNTINFO);
  if (fp == NULL)
    return UV__ERR(errno);
  /*
   * Loop once per line; try to find the mount location for the given subsystem.
   */
  while (1) {
    /* Should be big enough to fit a single line. */
    char buf[CGROUPS_BUF_SIZE];
    int read_line_rc;
    char* curr_root;
    char* curr_mount_point;
    char* curr_fs_type;
    char* curr_super_options;

    read_line_rc = uv__read_line(buf, CGROUPS_BUF_SIZE, fp);
    if (read_line_rc > 0) {
      /*
       * Treat a truncated line as invalid.
       * cgroup-relevant mountinfo entries should already be well under 12KB
       * in length.
       */
      continue;
    } else if (read_line_rc < 0) {
      /*
       * Note that feof(fp) will evaluate to true immediately after the last
       * line is read, but read_line_rc will be zero.
       */
      if (feof(fp))
        break;
      goto file_malformed;
    }

    field_ptr = buf;
    strsep(&field_ptr, " "); /* mount ID */
    strsep(&field_ptr, " "); /* parent ID */
    strsep(&field_ptr, " "); /* st_dev major:minor */
    curr_root = strsep(&field_ptr, " ");
    curr_mount_point = strsep(&field_ptr, " ");
    strsep(&field_ptr, " "); /* mount options */
    /* A hyphen marks the end of variable-length optional fields. */
    while ('-' != field_ptr[0]) {
      strsep(&field_ptr, " ");
    }
    strsep(&field_ptr, " "); /* separator (hyphen) */
    curr_fs_type = strsep(&field_ptr, " ");
    strsep(&field_ptr, " "); /* mount source */
    curr_super_options = strsep(&field_ptr, " ");
    /*
     * If the fs type (9) is "cgroup" and super options (11) contains the name
     * of the subsystem, then we've found the correct mount location, so save
     * the values of root (4) and mount point (5) and break.
     * Otherwise, if the fs type is "cgroup2", we've potentially found the
     * correct mount location, so save the above values, but don't break,
     * because we don't know yet whether the input subsystem is controlled by
     * cgroups v1 or v2.
     */
    if (!strcmp(curr_fs_type, "cgroup")) {
      /* cgroups v1 */
      if (uv__find_in_delimited_string(curr_super_options, subsystem, ',')) {
        strcpy(root, curr_root);
        strcpy(mount_point, curr_mount_point);
        info->cgroups_version = CGROUPS_VERSION_1;
        break;
      }
    } else if (!strcmp(curr_fs_type, "cgroup2")) {
      /* cgroups v2 */
      strcpy(root, curr_root);
      strcpy(mount_point, curr_mount_point);
      info->cgroups_version = CGROUPS_VERSION_2;
      /*
        * Don't break, as we're not certain that this subsystem is controlled
        * by cgroups v2.
        */
    }
  }

  fclose(fp);

  /*
   * If cgroups version wasn't determined, assume this subsystem isn't enabled
   * in cgroups, so don't bother reading /proc/self/cgroup.
   */
  if (CGROUPS_VERSION_UNKNOWN == info->cgroups_version)
    return 0;
  
  /* Read /proc/self/cgroup to get hierarchy path. */

  hierarchy_path[0] = '\0';

  fp = uv__open_file(PROC_SELF_CGROUP);
  if (fp == NULL)
    return UV__ERR(errno);
  
  /* + 3 for two colons, and terminal character. */
  subsystem_search_string = malloc(strlen(subsystem) + 3);
  snprintf(subsystem_search_string, strlen(subsystem) + 3, ":%s:", subsystem);

  while (1) {
    /* Should be big enough to fit a single line. */
    char buf[CGROUPS_BUF_SIZE];
    int read_line_rc;
    const char* hierarchy_path_search_ptr;

    read_line_rc = uv__read_line(buf, CGROUPS_BUF_SIZE, fp);
    if (read_line_rc > 0) {
      /*
       * Treat a truncated line as invalid.
       * Lines in this file shouldn't be much more than ~4KB.
       */
      continue;
    } else if (read_line_rc < 0) {
      /*
       * Note that feof(fp) will evaluate to true immediately after the last
       * line is read, but read_line_rc will be zero.
       */
      if (feof(fp))
        break;
      goto file_malformed;
    }

    if (CGROUPS_VERSION_1 == info->cgroups_version) {
      hierarchy_path_search_ptr = strstr(buf, subsystem_search_string);
      if (hierarchy_path_search_ptr) {
        hierarchy_path_search_ptr += strlen(subsystem_search_string);
      }
    } else { /* if (CGROUPS_VERSION_2 == info->cgroups_version) */
      if (!strncmp(buf, CGROUPS_V2_CTRL_PREFIX, CGROUPS_V2_CTRL_PREFIX_LEN))
        hierarchy_path_search_ptr = buf + CGROUPS_V2_CTRL_PREFIX_LEN;
    }
    if (hierarchy_path_search_ptr) {
      strcpy(hierarchy_path, hierarchy_path_search_ptr);
      break;
    }
  }

  fclose(fp);
  free(subsystem_search_string);

  /*
   * The hierarchy path should be prefixed with the root path from mountinfo,
   * and should be replaced with the mount point.
   */
  if (!strncmp(hierarchy_path, root, strlen(root))) {
    const char* hierarchy_path_inner;
    size_t path_size;
    hierarchy_path_inner = hierarchy_path + strlen(root);
    /* +2 for "/" and null terminator. */
    path_size = strlen(mount_point) + strlen(hierarchy_path_inner) + 2;
    info->path = malloc(path_size);
    snprintf(info->path, path_size, "%s/%s", mount_point, hierarchy_path_inner);
  } else
    info->cgroups_version = CGROUPS_VERSION_UNKNOWN;
  return 0;

file_malformed:
  fclose(fp);
  info->cgroups_version = CGROUPS_VERSION_UNKNOWN;
  if (subsystem_search_string != NULL)
    free(subsystem_search_string);
  return UV_EIO;
}


static uint64_t uv__read_cgroups_uint64(const char* path, const char* param) {
  char filename[PATH_MAX];
  uint64_t rc;
  int fd;
  ssize_t n;
  char buf[32];  /* Large enough to hold an encoded uint64_t. */

  snprintf(filename, PATH_MAX, "%s/%s", path, param);

  rc = 0;
  fd = uv__open_cloexec(filename, O_RDONLY);

  if (fd < 0)
    return 0;

  n = read(fd, buf, sizeof(buf) - 1);

  if (n > 0) {
    buf[n] = '\0';
    if (!strcmp(buf, CGROUPS_V2_NO_LIMIT_VALUE))
      rc = 0;
    else
      sscanf(buf, "%" PRIu64, &rc);
  }

  if (uv__close_nocheckstdio(fd))
    abort();

  return rc;
}


uint64_t uv_get_constrained_memory(void) {
  uv__cgroups_subsystem_info_t info;
  uint64_t rc;
  /* For v2 only. */
  uint64_t max, high;

  rc = 0;

  if (!uv__read_cgroups_proc_files(&info, "memory")) {
    /*
     * uv__read_cgroups_uint64 might return 0 if there was a problem getting the
     * memory limit from cgroups. This is OK because a return value of 0
     * signifies that the memory limit is unknown.
     */

    if (CGROUPS_VERSION_1 == info.cgroups_version)
      rc = uv__read_cgroups_uint64(info.path, "memory.limit_in_bytes");

    if (CGROUPS_VERSION_2 == info.cgroups_version) {
      max = uv__read_cgroups_uint64(info.path, "memory.max");
      high = uv__read_cgroups_uint64(info.path, "memory.high");
      if (max == 0 && high == 0)
        rc = 0;
      else if (max == 0)
        rc = high;
      else if (high == 0)
        rc = max;
      else
        rc = max < high ? max : high;
    }

    if (NULL != info.path) {
      free(info.path);
    }
  }

  return rc;
}
