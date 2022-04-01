#include "mem.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "debug-info.h"

#define DEFAULT_MEMSIZE (4 * 1024 * 1024)
#define DEFAULT_PAGESIZE 4096

static size_t pagesize = DEFAULT_PAGESIZE;
static int out_of_mem_mesg = 0;
static long num_failures = 0;
static long total_non_freeable = 0;
static long num_segments = 0;
static long total_allocation = 0;

static inline size_t
round_up(size_t size) {
  return (size + 7) & ~7L;
}
static inline size_t
gputrigger_align_pagesize(size_t size) {
  return ((size + pagesize - 1) / pagesize) * pagesize;
}

//
// Returns: address of mmap-ed region, else NULL on failure.
//
static void *
gputrigger_mmap_anon(size_t size) {
  int prot, flags, fd;
  char *str;
  void *addr;

  size = gputrigger_align_pagesize(size);
  prot = PROT_READ | PROT_WRITE;
  fd = -1;

#if defined(MAP_ANON)
  flags = MAP_PRIVATE | MAP_ANON;
#elif defined(MAP_ANONYMOUS)
  flags = MAP_PRIVATE | MAP_ANONYMOUS;
#else
  flags = MAP_PRIVATE;
  fd = open("/dev/zero", O_RDWR);
  if (fd < 0) {
    str = strerror(errno);
    PRINT_ERR("MALLOC %s: open /dev/null failed: %s\n", __func__, str);
    return NULL;
  }
#endif

  addr = mmap(NULL, size, prot, flags, fd, 0);
  if (addr == MAP_FAILED) {
    str = strerror(errno);
    PRINT_ERR("MALLOC %s: mmap failed: %s\n", __func__, str);
    addr = NULL;
  } else {
    num_segments++;
    total_allocation += size;
  }
  PRINT_INFO("MALLOC %s: size = %ld, fd = %d, addr = %p\n",
             __func__, size, fd, addr);
  return addr;
}

//
// Returns: address of non-freeable region at the high end,
// else NULL on failure.
//
void *
gputrigger_malloc(size_t size) {
  gputrigger_meminfo_t *mi;
  void *addr;
  if (size == 0) {
    return NULL;
  }
  size = round_up(size);
  addr = gputrigger_mmap_anon(size);
  if (addr == NULL) {
    PRINT_ERR("Malloc Error: %s shutting down\n", __func__);
    exit(-2);
  }
  PRINT_INFO("MALLOC %s: size = %ld, addr = %p\n", __func__, size, addr);
  total_non_freeable += size;
  return addr;
}
