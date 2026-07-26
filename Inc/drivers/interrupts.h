/**
  ******************************************************************************
  * @file    interrupts.h
  * @author  Angus Macdonald
  * @brief   HAL interrupt callback overrides - routing only.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef INTERRUPTS_H
#define INTERRUPTS_H

/* interrupts.c owns every HAL weak-callback override in this project, so all
 * interrupt entry points are visible in one place and modules never collide
 * over the shared callback symbols. Callbacks contain routing only - the
 * logic lives in a module function called from the callback.
 *
 * The callbacks themselves are declared by the HAL (they override its __weak
 * definitions), so this header declares nothing. It exists to document the
 * convention and hold future declarations if any are needed. */

#endif /* INTERRUPTS_H */
