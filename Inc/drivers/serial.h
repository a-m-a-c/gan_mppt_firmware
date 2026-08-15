/**
  ******************************************************************************
  * @file    serial.h
  * @author  Angus Macdonald
  * @brief   UART5 transport (onboard UART-to-USB). Bytes only, no protocol.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* This module moves bytes over UART5 and frames them into lines. It knows
 * nothing about what the lines mean - see command.h for the protocol.
 *
 * Nothing here blocks. Both directions are interrupt-driven ring buffers, and
 * serial.c owns UART5 at the register level: never call HAL_UART_* on huart5,
 * because the HAL's own paths take __HAL_LOCK on the handle and would fight
 * the ISR for it. */

typedef enum
{
  SERIAL_LINE_NONE = 0, /* no complete line ready                          */
  SERIAL_LINE_OK,       /* out holds a NUL-terminated line, no terminator  */
  SERIAL_LINE_OVERFLOW, /* the line was longer than the buffer; discarded  */
  SERIAL_LINE_LOST      /* a byte was dropped inside this line; discarded  */
} serial_line_t;

/* Enables the UART5 interrupt. Call once from app_setup(), after
 * MX_UART5_Init(). */
void serial_init(void);

/* Returns the next complete line, if one has arrived. CR, LF or CRLF all end a
 * line; the terminator is stripped and `out` is NUL-terminated. Returns
 * SERIAL_LINE_NONE when nothing is ready, which is the usual answer - call it
 * every pass and act only on OK.
 *
 * OVERFLOW and LOST mean a line arrived but cannot be trusted. They are worth
 * reporting to the host, since they are the difference between "you sent
 * nothing" and "we could not hear you". */
serial_line_t serial_read_line(char *out, size_t size);

/* Queues bytes for transmission and returns immediately - the ISR drains the
 * ring. All-or-nothing: returns false and queues nothing if the whole length
 * does not fit, so a caller never emits half a line. */
bool serial_write(const void *data, size_t length);

/* Bytes the transmit ring can still accept. Check this before committing to a
 * burst that must not be split, and before consuming input that obliges a
 * reply. */
size_t serial_tx_free(void);

/* Lines refused for want of room since boot. Nonzero means the host is not
 * keeping up, or something queued without checking serial_tx_free() first. */
uint32_t serial_tx_dropped(void);

/* UART5 ISR body, both directions. Called from the UART5 vector in
 * interrupts.c; not for application use. */
void serial_irq(void);

#endif /* SERIAL_H */
