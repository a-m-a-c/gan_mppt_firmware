/**
  ******************************************************************************
  * @file    serial.c
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
#include "serial.h"

#include "main.h"
#include "usart.h"

/* Longest line this module will assemble. Anything longer is reported as
   SERIAL_LINE_OVERFLOW rather than silently cut in half and passed on. */
#define SERIAL_LINE_MAX 96

/* UART5 preempts nothing that matters and must never delay the OVP/OCP fault
   vectors, which run at priority 0. */
#define SERIAL_IRQ_PRIORITY 5U

/* Both rings are single-producer / single-consumer, which is what lets them go
   without a lock: for RX the ISR only advances head and the loop only advances
   tail, and for TX it is the other way round. A 32-bit aligned load or store is
   atomic on Cortex-M7, so each side always sees a whole index - a stale one
   only ever understates how much is available, which is the safe direction.
   Always read an index once into a local; re-reading mid-expression can give
   two different answers. */
static volatile uint8_t serial_rx_ring[SERIAL_RX_RING_LEN];
static volatile uint32_t serial_rx_head; /* ISR writes, loop reads */
static volatile uint32_t serial_rx_tail; /* loop writes, ISR reads */

static volatile uint8_t serial_tx_ring[SERIAL_TX_RING_LEN];
static volatile uint32_t serial_tx_head; /* loop writes, ISR reads */
static volatile uint32_t serial_tx_tail; /* ISR writes, loop reads */

/* Lines refused because the ring was too full to take them whole. */
static uint32_t serial_tx_dropped_lines;

/* Line under assembly, carried across calls until a terminator arrives. */
static char serial_line[SERIAL_LINE_MAX];
static size_t serial_line_length;
static bool serial_line_too_long;

/* Set by the ISR when a byte could not be kept, cleared when the line it
   belonged to is handed over. That line has a hole in it and is rejected.
   Setting it just after the consumer clears it costs one spuriously rejected
   line, which is why the host retries rather than assumes. */
static volatile bool serial_rx_lost;

size_t serial_tx_free(void)
{
  uint32_t head = serial_tx_head;
  uint32_t tail = serial_tx_tail;

  /* One slot is always left empty so head == tail means empty, never full. */
  return (size_t)((tail - head - 1U) & (SERIAL_TX_RING_LEN - 1U));
}

uint32_t serial_tx_dropped(void)
{
  return serial_tx_dropped_lines;
}

bool serial_write(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t head;

  if (length == 0U)
  {
    return true;
  }
  if (length > serial_tx_free())
  {
    serial_tx_dropped_lines++;
    return false;
  }

  head = serial_tx_head;
  for (size_t i = 0U; i < length; i++)
  {
    serial_tx_ring[head] = bytes[i];
    head = (head + 1U) & (SERIAL_TX_RING_LEN - 1U);
  }

  /* Publish the data before the index, and the index before the interrupt is
     enabled. The other order lets the ISR see an empty ring, switch TXEIE off,
     and have this enable undone - the line would then sit in the ring until
     something else happened to queue more. Re-enabling an already-enabled
     TXEIE is harmless, so erring this way costs nothing. */
  __DMB();
  serial_tx_head = head;
  __DMB();
  SET_BIT(huart5.Instance->CR1, USART_CR1_TXEIE_TXFNFIE);

  return true;
}

static bool serial_rx_pop(uint8_t *byte)
{
  uint32_t tail = serial_rx_tail;

  if (tail == serial_rx_head)
  {
    return false;
  }

  *byte = serial_rx_ring[tail];
  serial_rx_tail = (tail + 1U) & (SERIAL_RX_RING_LEN - 1U);

  return true;
}

serial_line_t serial_read_line(char *out, size_t size)
{
  uint8_t byte;

  if ((out == NULL) || (size == 0U))
  {
    return SERIAL_LINE_NONE;
  }

  while (serial_rx_pop(&byte))
  {
    if ((byte != '\r') && (byte != '\n'))
    {
      if (serial_line_length < (sizeof(serial_line) - 1U))
      {
        serial_line[serial_line_length] = (char)byte;
        serial_line_length++;
      }
      else
      {
        serial_line_too_long = true;
      }
      continue;
    }

    /* A terminator closes the line. CRLF produces one line and then an empty
       one, which reports NONE and costs nothing. */
    serial_line_t result = SERIAL_LINE_NONE;
    if (serial_rx_lost)
    {
      result = SERIAL_LINE_LOST;
    }
    else if (serial_line_too_long)
    {
      result = SERIAL_LINE_OVERFLOW;
    }
    else if (serial_line_length > 0U)
    {
      size_t copy = serial_line_length;
      if (copy > (size - 1U))
      {
        copy = size - 1U;
        result = SERIAL_LINE_OVERFLOW;
      }
      else
      {
        result = SERIAL_LINE_OK;
      }
      for (size_t i = 0U; i < copy; i++)
      {
        out[i] = serial_line[i];
      }
      out[copy] = '\0';
    }

    serial_line_length = 0U;
    serial_line_too_long = false;
    serial_rx_lost = false;

    if (result != SERIAL_LINE_NONE)
    {
      return result;
    }
  }

  return SERIAL_LINE_NONE;
}

/* ISR context, both directions. Registers rather than HAL throughout: the HAL
   IT paths drive the handle's own state machine and take __HAL_LOCK, so RX and
   TX could not share one handle. Here each half just moves a byte.
   serial.c owns UART5 at the register level - never call HAL_UART_* on huart5.

   UART5 FIFO mode is disabled (usart.c), so TXE/RXNE each mean exactly one
   slot. Re-enabling the FIFO would require both halves to loop while their
   flag stays set. */
void serial_irq(void)
{
  USART_TypeDef *uart = huart5.Instance;
  uint32_t status = uart->ISR;

  /* Gate on TXEIE, not on the TXE flag alone: TXE is set whenever the data
     register is empty, which is nearly always, so testing the flag by itself
     would send every *receive* interrupt down this path too - and with an
     empty ring it would keep re-entering forever. */
  if (((uart->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U) &&
      ((status & USART_ISR_TXE_TXFNF) != 0U))
  {
    uint32_t tail = serial_tx_tail;

    if (tail == serial_tx_head)
    {
      CLEAR_BIT(uart->CR1, USART_CR1_TXEIE_TXFNFIE);
    }
    else
    {
      uart->TDR = serial_tx_ring[tail];
      serial_tx_tail = (tail + 1U) & (SERIAL_TX_RING_LEN - 1U);
    }
  }

  /* Overrun latches RXNE off for good, so clear it first: losing one byte
     must not take the whole command path down with it. The half-received
     line is then discarded, since a byte is missing from the middle of it. */
  if ((status & USART_ISR_ORE) != 0U)
  {
    uart->ICR = USART_ICR_ORECF;
    serial_rx_lost = true;
  }

  if ((status & USART_ISR_RXNE_RXFNE) != 0U)
  {
    uint8_t byte = (uint8_t)uart->RDR;
    uint32_t next = (serial_rx_head + 1U) & (SERIAL_RX_RING_LEN - 1U);

    /* Full ring: drop the byte. Same outcome as an overrun - the line it
       belonged to is corrupt and is rejected when its terminator arrives. */
    if (next != serial_rx_tail)
    {
      serial_rx_ring[serial_rx_head] = byte;
      serial_rx_head = next;
    }
    else
    {
      serial_rx_lost = true;
    }
  }
}

void serial_init(void)
{
  /* Enable receive interrupts last, so the rings and the line buffer are
     already in a valid state before the first byte can arrive. */
  __HAL_UART_ENABLE_IT(&huart5, UART_IT_RXNE);
  HAL_NVIC_SetPriority(UART5_IRQn, SERIAL_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(UART5_IRQn);
}
