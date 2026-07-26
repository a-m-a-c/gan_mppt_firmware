#include "interrupts.h"
#include "main.h"
#include "pwm.h"

/* All callbacks here run in ISR context. Routing only - no logic. */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == OVP_Pin)
  {
    pwm_OVP_fault();
  }
}

/* Per-channel OCP faults: FLT1->A ... FLT5->E. */

void HAL_HRTIM_Fault1Callback(HRTIM_HandleTypeDef *hhrtim)
{
  UNUSED(hhrtim);
  pwm_OCP_fault(PWM_CHANNEL_A);
}

void HAL_HRTIM_Fault2Callback(HRTIM_HandleTypeDef *hhrtim)
{
  UNUSED(hhrtim);
  pwm_OCP_fault(PWM_CHANNEL_B);
}

void HAL_HRTIM_Fault3Callback(HRTIM_HandleTypeDef *hhrtim)
{
  UNUSED(hhrtim);
  pwm_OCP_fault(PWM_CHANNEL_C);
}

void HAL_HRTIM_Fault4Callback(HRTIM_HandleTypeDef *hhrtim)
{
  UNUSED(hhrtim);
  pwm_OCP_fault(PWM_CHANNEL_D);
}

void HAL_HRTIM_Fault5Callback(HRTIM_HandleTypeDef *hhrtim)
{
  UNUSED(hhrtim);
  pwm_OCP_fault(PWM_CHANNEL_E);
}
