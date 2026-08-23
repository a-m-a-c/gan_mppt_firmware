#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H
#include <stdbool.h>

typedef enum {
  MODE_NONE,
  MODE_SINGLE_CH_MPPT,
  MODE_SINGLE_CH_CV,
  MODE_MPPT
} mode_t;

typedef enum {
  MODE_STATE_INIT,
  MODE_STATE_RUNNING,
  MODE_STATE_FAULTED,
  MODE_STATE_EXIT
} mode_state_t;

typedef enum {
    MODE_INIT_REFUSED,
    MODE_INIT_FAULT,
    MODE_INIT_OK
} mode_init_result_t;

mode_init_result_t mode_begin(mode_t mode);
mode_state_t mode_service(mode_t mode);

#endif /* MODE_MANAGER_H */