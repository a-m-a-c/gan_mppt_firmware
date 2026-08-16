#include "interrupts.h"
#include "channel_telem.h"
#include "i2c.h"
#include "iind.h"
#include "main.h"
#include "pwm.h"
#include "serial.h"

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

/* UART5 host link, receive and transmit. This one overrides the startup file's
   weak vector rather than a HAL callback - UART5 is not enabled in CubeMX's
   NVIC list, so stm32h7xx_it.c has no handler for it - but the entry point
   still belongs here with the rest. */
void UART5_IRQHandler(void)
{
  serial_irq();
}

/* Telemetry I2C. Like UART5 these vectors are not in CubeMX's NVIC list, so
   they override the startup file's weak symbols. The HAL drives the transfer;
   telem_* only records that it finished. */
void I2C1_EV_IRQHandler(void)
{
  HAL_I2C_EV_IRQHandler(&hi2c1);
}

void I2C1_ER_IRQHandler(void)
{
  HAL_I2C_ER_IRQHandler(&hi2c1);
}

/* Both HAL callbacks are shared by every I2C handle, so each checks which
   instance it is being told about - hi2c2 exists, even if nothing uses it. */
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance == I2C1)
  {
    telem_i2c_complete();
  }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance == I2C1)
  {
    telem_i2c_error();
  }
}

/* Shared by every ADC handle, so it checks which instance completed. Only
   ADC3 is of interest: it carries four of the five inductor current channels,
   so its sequence completing is what marks a full set. ADC2's single channel
   lands in the same window and needs no separate notification.

   analog.c's ADC1 conversions are polled, not interrupt-driven, so they never
   reach here. */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC3)
  {
    iind_conversion_complete();
  }
}
