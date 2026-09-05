/* samplib.c — in-process CPU stack sampler for FreeCAD's renderer.
 *
 * LD_PRELOAD'd into the FreeCAD process.  It arms an ITIMER_PROF (CPU-time)
 * timer and, on each SIGPROF, snapshots the interrupted thread's call stack
 * with backtrace() and appends the raw instruction addresses to
 * FC_PROF_STACK_FILE.  It also dumps /proc/self/maps to FC_PROF_MAPS_FILE so
 * the host can symbolize the addresses to file:line via addr2line.
 *
 * Build:
 *   gcc -shared -fPIC -O2 -o /tmp/opencode/samplib.so samplib.c -ldl
 *
 * Run FreeCAD with:
 *   LD_PRELOAD=/tmp/opencode/samplib.so FC_PROF_STACK_FILE=... \
 *   FC_PROF_MAPS_FILE=... FC_PROF_HZ=<n>  (default 100)
 *
 * Because it uses the CPU-time timer the signal is delivered to the thread
 * that is actually burning CPU (the render/GUI thread), so the profile is a
 * faithful CPU attribution of the raster path.  Frames are written as:
 *   S <addr>\n<addr>\n... E\n   (one sample)
 */
#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

static int g_fd = -1;
static int g_maps = -1;

static void dump_maps(void)
{
  const char *p = getenv("FC_PROF_MAPS_FILE");
  if (!p) return;
  int in = open("/proc/self/maps", O_RDONLY);
  if (in < 0) return;
  int out = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { close(in); return; }
  char buf[8192];
  ssize_t n;
  while ((n = read(in, buf, sizeof buf)) > 0)
    write(out, buf, n);
  close(in);
  close(out);
}

static void handler(int sig, siginfo_t *si, void *uctx)
{
  if (g_fd < 0) return;
  void *bt[96];
  int n = backtrace(bt, 96);
  if (n <= 0) return;
  char buf[48];
  if (write(g_fd, "S\n", 2) < 0) return;
  for (int i = 0; i < n; i++) {
    int w = snprintf(buf, sizeof buf, "%p\n", bt[i]);
    if (w > 0) write(g_fd, buf, w);
  }
  write(g_fd, "E\n", 2);
}

__attribute__((constructor)) static void init(void)
{
  const char *path = getenv("FC_PROF_STACK_FILE");
  if (!path) return;
  g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (g_fd < 0) return;
  dump_maps();

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, 0);

  int hz = 100;
  const char *h = getenv("FC_PROF_HZ");
  if (h) { int v = atoi(h); if (v > 0) hz = v; }
  struct itimerval it;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = (1000000 + hz - 1) / hz;
  it.it_value = it.it_interval;
  setitimer(ITIMER_PROF, &it, 0);
}

__attribute__((destructor)) static void fini(void)
{
  if (g_fd >= 0) close(g_fd);
  if (g_maps >= 0) close(g_maps);
}
