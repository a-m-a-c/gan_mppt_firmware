/**
  ******************************************************************************
  * @file    app.h
  * @author  Angus Macdonald
  * @brief   Application entry points, called from the generated main().
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef APP_H
#define APP_H

/* Everything this project actually does lives behind these two calls, so
 * Src/main.c stays as CubeMX generated it apart from three lines inside the
 * USER CODE markers. Regenerating from the .ioc then has nothing of ours to
 * lose, and there is one obvious place to read the program.
 *
 * Both run after HAL_Init() and every MX_*_Init(), so peripherals are up. */

/* One-time bring-up: sensors, LEDs and the host link. */
void app_setup(void);

/* One pass of the main loop. Must not block - the services inside it are all
 * non-blocking and expect to be called often. */
void app_loop(void);

#endif /* APP_H */
