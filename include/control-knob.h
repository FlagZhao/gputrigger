#ifndef GPUPUNK_CONTROL_KNOB_H
#define GPUPUNK_CONTROL_KNOB_H

#define FORALL_KNOBS(macro)                                              \
  macro(GPUPUNK_SANITIZER_GPU_PATCH_RECORD_NUM)                          \
      macro(GPUPUNK_SANITIZER_BUFFER_POOL_SIZE)                          \
          macro(GPUPUNK_SANITIZER_APPROX_LEVEL)                          \
              macro(GPUPUNK_SANITIZER_DEFAULT_TYPE)                      \
                  macro(GPUPUNK_SANITIZER_KERNEL_SAMPLING_FREQUENCY)     \
                      macro(GPUPUNK_SANITIZER_BLOCK_SAMPLEING_FREQUENCY) \
                          macro(GPUPUNK_SANITIZER_WHITELIST)             \
                              macro(GPUPUNK_SANITIZER_BLACKLIST)         \
                                  macro(GPUPUNK_PREPROCESSOR_ENABLE)

typedef enum {
#define DEFINE_ENUM_KNOBS(knob_name)  \
  knob_name,

  FORALL_KNOBS(DEFINE_ENUM_KNOBS) 

#undef DEFINE_ENUM_KNOBS

#define COUNT_FORALL_CLAUSE(a) + 1
#define NUM_CLAUSES(forall_macro) 0 forall_macro(COUNT_FORALL_CLAUSE)

  GPUPUNK_NUM_CONTROL_KNOBS = NUM_CLAUSES(FORALL_KNOBS)

#undef NUM_CLAUSES
#undef COUNT_FORALL_CLAUSE
} control_category;


void control_knob_init();

char *control_knob_value_get(control_category c);

int control_knob_value_get_int(control_category c);

#define PRINT_KNOBS(...) fprintf(stderr, __VA_ARGS__)

#endif
