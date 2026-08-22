#ifndef APP_H
#define APP_H
#include <stdbool.h>

extern volatile bool error_flag;

// Contains setup, for main.c init.
void app_setup(void);

// Contains program loop, for main.c loop. Limit blocking functions here.
void app_loop(void);

#endif /* APP_H */
