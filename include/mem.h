#ifndef GPUTRIGGER_MEM_H
#define GPUTRIGGER_MEM_H
#include <stddef.h>

typedef struct gputrigger_meminfo {
  void *mi_start;
  void *mi_low;
  void *mi_high;
  long mi_size;
} gputrigger_meminfo_t;

void *
gputrigger_malloc(size_t size);


#endif // GPUTRIGGER_MEM_H