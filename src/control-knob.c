#include "control-knob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *control_knob_names[] = {
#define DEFINE_KNOB_NAMES(knob_name) \
#knob_name,

    FORALL_KNOBS(DEFINE_KNOB_NAMES)

#undef DEFINE_KNOB_NAMES
};

typedef struct control_knob {
  char *name;
  char *value;
} control_knob_t;

control_knob_t control_knobs[GPUPUNK_NUM_CONTROL_KNOBS];

void control_knob_init() {
#define INIT_KNOBS(knob_name)                                    \
  control_knobs[knob_name].name = control_knob_names[knob_name]; \
  control_knobs[knob_name].value = NULL;

  FORALL_KNOBS(INIT_KNOBS)

#undef INIT_KNOBS

  control_category c;
  for (c = 0; c < GPUPUNK_NUM_CONTROL_KNOBS; c++) {
    char *s = getenv(control_knob_names[c]);
    if (s) {
      control_knobs[c].value = s;
      PRINT_KNOBS("gputrigger-> Control knob %s: %s\n", control_knob_names[c], s);
    }
  }
}

char *
control_knob_value_get(control_category c) {
  if (c < GPUPUNK_NUM_CONTROL_KNOBS) {
    return control_knobs[c].value;
  }
  return NULL;
}

int control_knob_value_get_int(control_category c) {
  char *ret = NULL;
  if ((ret = control_knob_value_get(c)) != NULL) {
    return atoi(ret);
  }
  return 0;
}