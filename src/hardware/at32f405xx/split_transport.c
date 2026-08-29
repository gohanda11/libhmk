/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "hardware/split_transport_api.h"

#if defined(SPLIT_KEYBOARD)

#include "at32f402_405.h"
#include "hardware/board_api.h"
#include "hardware/timer_api.h"

//--------------------------------------------------------------------+
// Configuration Validation
//--------------------------------------------------------------------+

#if !defined(SPLIT_UART_INSTANCE)
#error "SPLIT_UART_INSTANCE is not defined"
#endif

#if !defined(SPLIT_UART_TX_PORT) || !defined(SPLIT_UART_TX_PIN)
#error "SPLIT_UART_TX_PORT or SPLIT_UART_TX_PIN is not defined"
#endif

#if !defined(SPLIT_UART_TX_MUX)
#error "SPLIT_UART_TX_MUX is not defined"
#endif

#if !defined(SPLIT_UART_BAUD_RATE)
#error "SPLIT_UART_BAUD_RATE is not defined"
#endif

// Maximum time in milliseconds to wait for the transmit buffer to become
// empty or for transmission to complete before aborting the transfer.
#define SPLIT_TRANSPORT_SEND_TIMEOUT_MS 10

// Minimum gap in microseconds between consecutive frames on the shared line.
// The receiver polls a 1-byte USART FIFO and needs time to CRC-check a frame
// and copy it out before the next frame starts; back-to-back frames overrun
// that FIFO and lose the next frame's leading bytes. The sender enforces this
// gap, while the normal 2 ms polling period already exceeds it and adds no
// delay.
#ifndef SPLIT_TRANSPORT_INTER_FRAME_US
#define SPLIT_TRANSPORT_INTER_FRAME_US 150
#endif

//--------------------------------------------------------------------+
// USART Instance Mapping
//--------------------------------------------------------------------+

static usart_type *split_usart_instance(void) {
#if SPLIT_UART_INSTANCE == 1
  return USART1;
#elif SPLIT_UART_INSTANCE == 2
  return USART2;
#elif SPLIT_UART_INSTANCE == 3
  return USART3;
#elif SPLIT_UART_INSTANCE == 4
  return USART4;
#elif SPLIT_UART_INSTANCE == 5
  return USART5;
#elif SPLIT_UART_INSTANCE == 6
  return USART6;
#else
#error "Unsupported SPLIT_UART_INSTANCE"
#endif
}

static void split_usart_clock_enable(void) {
#if SPLIT_UART_INSTANCE == 1
  crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
#elif SPLIT_UART_INSTANCE == 2
  crm_periph_clock_enable(CRM_USART2_PERIPH_CLOCK, TRUE);
#elif SPLIT_UART_INSTANCE == 3
  crm_periph_clock_enable(CRM_USART3_PERIPH_CLOCK, TRUE);
#elif SPLIT_UART_INSTANCE == 4
  crm_periph_clock_enable(CRM_USART4_PERIPH_CLOCK, TRUE);
#elif SPLIT_UART_INSTANCE == 5
  crm_periph_clock_enable(CRM_USART5_PERIPH_CLOCK, TRUE);
#elif SPLIT_UART_INSTANCE == 6
  crm_periph_clock_enable(CRM_USART6_PERIPH_CLOCK, TRUE);
#endif
}

//--------------------------------------------------------------------+
// GPIO Helpers
//--------------------------------------------------------------------+

static gpio_type *const gpio_port_map[] = {
    GPIOA, GPIOB, GPIOC, GPIOD, NULL, GPIOF, NULL, NULL,
};

static uint16_t gpio_pin_mask(uint32_t pin) { return (uint16_t)(1U << pin); }

static void split_gpio_clock_enable(uint32_t port) {
  switch (port) {
  case 0:
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    break;
  case 1:
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    break;
  case 2:
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    break;
  case 3:
    crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    break;
  case 5:
    crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);
    break;
  default:
    break;
  }
}

static void split_gpio_init(uint32_t port, uint32_t pin, uint32_t mux) {
  if (port >= M_ARRAY_SIZE(gpio_port_map) || gpio_port_map[port] == NULL)
    return;

  split_gpio_clock_enable(port);

  gpio_init_type gpio_init_struct;
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = gpio_pin_mask(pin);
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(gpio_port_map[port], &gpio_init_struct);

  gpio_pin_mux_config(gpio_port_map[port], (gpio_pins_source_type)pin,
                      (gpio_mux_sel_type)mux);
}

//--------------------------------------------------------------------+
// Transport API Implementation
//--------------------------------------------------------------------+

static void split_transport_clear_errors(void);

void split_transport_master_init(void) {
  split_usart_clock_enable();

  // The Split60HE hardware connects the halves with a single TRRS data line
  // (PA9) and no external pull-up resistor, so the TX pin must actively drive
  // the line with push-pull output.
  split_gpio_init(SPLIT_UART_TX_PORT, SPLIT_UART_TX_PIN, SPLIT_UART_TX_MUX);
#if !defined(SPLIT_UART_HALF_DUPLEX)
  split_gpio_init(SPLIT_UART_RX_PORT, SPLIT_UART_RX_PIN, SPLIT_UART_RX_MUX);
#endif

  usart_type *usart = split_usart_instance();
  usart_init(usart, SPLIT_UART_BAUD_RATE, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_parity_selection_config(usart, USART_PARITY_NONE);

#if defined(SPLIT_UART_HALF_DUPLEX)
  usart_single_line_halfduplex_select(usart, TRUE);
#endif

  // Keep receiver enabled; transmitter is enabled only when sending in
  // half-duplex mode.
  usart_receiver_enable(usart, TRUE);
  usart_enable(usart, TRUE);
}

void split_transport_slave_init(void) {
  // Same initialization as master for AT32
  split_transport_master_init();
}

static void split_transport_enable_tx(void) {
#if defined(SPLIT_UART_HALF_DUPLEX)
  usart_type *usart = split_usart_instance();
  usart_transmitter_enable(usart, TRUE);
#endif
}

static void split_transport_disable_tx(void) {
#if defined(SPLIT_UART_HALF_DUPLEX)
  usart_type *usart = split_usart_instance();
  usart_transmitter_enable(usart, FALSE);
  // Clear any loopback data that was received while transmitting
  while (usart_flag_get(usart, USART_RDBF_FLAG) != RESET) {
    (void)usart_data_receive(usart);
  }
#endif
}

//--------------------------------------------------------------------+
// Inter-Frame Gap
//--------------------------------------------------------------------+

static bool split_transport_sent_once = false;
static uint32_t split_transport_last_tx_cycle = 0;

static void split_transport_wait_inter_frame(void) {
  // The first send has no preceding frame to leave a gap after.
  if (!split_transport_sent_once)
    return;

  const uint32_t system_clock = board_clock_frequency();
  const uint32_t gap_cycles =
      (system_clock / 1000000) * (uint32_t)SPLIT_TRANSPORT_INTER_FRAME_US;
  const uint32_t now = board_cycle_count();
  const uint32_t elapsed = now - split_transport_last_tx_cycle;

  // Unsigned subtraction wraps safely: the gap is only 150 us, far inside the
  // 32-bit cycle counter's wrap period.
  if (elapsed < gap_cycles) {
    const uint32_t start = now;
    while ((board_cycle_count() - start) < (gap_cycles - elapsed))
      ;
  }
}

bool split_transport_send(const uint8_t *data, uint8_t len) {
  usart_type *usart = split_usart_instance();

  split_transport_wait_inter_frame();

  split_transport_enable_tx();

  for (uint8_t i = 0; i < len; i++) {
    const uint32_t tx_start = timer_read();
    while (usart_flag_get(usart, USART_TDBE_FLAG) == RESET) {
      if (timer_elapsed(tx_start) >= SPLIT_TRANSPORT_SEND_TIMEOUT_MS) {
        split_transport_disable_tx();
        split_transport_last_tx_cycle = board_cycle_count();
        split_transport_sent_once = true;
        return false;
      }
    }
    usart_data_transmit(usart, data[i]);
  }

  // Wait for transmission complete
  const uint32_t tc_start = timer_read();
  while (usart_flag_get(usart, USART_TDC_FLAG) == RESET) {
    if (timer_elapsed(tc_start) >= SPLIT_TRANSPORT_SEND_TIMEOUT_MS) {
      split_transport_disable_tx();
      split_transport_last_tx_cycle = board_cycle_count();
      split_transport_sent_once = true;
      return false;
    }
  }

  // Release the shared line immediately after the final stop bit. Holding the
  // push-pull driver active here collides with the other half's response.
  split_transport_disable_tx();

  split_transport_last_tx_cycle = board_cycle_count();
  split_transport_sent_once = true;
  return true;
}

bool split_transport_receive(uint8_t *data, uint8_t len, uint32_t timeout_ms) {
  usart_type *usart = split_usart_instance();
  const uint32_t start = timer_read();

  for (uint8_t i = 0; i < len; i++) {
    while (usart_flag_get(usart, USART_RDBF_FLAG) == RESET) {
      // Recover from overrun/framing errors so a single glitch cannot stall
      // the half-duplex link permanently.
      split_transport_clear_errors();
      if (timer_elapsed(start) >= timeout_ms)
        return false;
    }
    data[i] = (uint8_t)usart_data_receive(usart);
  }

  return true;
}

static void split_transport_clear_errors(void) {
  usart_type *usart = split_usart_instance();

  // Sticky receiver errors (especially overrun on the 1-byte USART FIFO) stop
  // further reception until they are cleared. The AT32 BSP performs the
  // required STS-then-DT read sequence, so do not read DT a second time here.
  if (usart_flag_get(usart, USART_PERR_FLAG) != RESET ||
      usart_flag_get(usart, USART_FERR_FLAG) != RESET ||
      usart_flag_get(usart, USART_NERR_FLAG) != RESET ||
      usart_flag_get(usart, USART_ROERR_FLAG) != RESET) {
    usart_flag_clear(usart, USART_PERR_FLAG | USART_FERR_FLAG | USART_NERR_FLAG |
                                USART_ROERR_FLAG);
  }
}

void split_transport_clear(void) {
  usart_type *usart = split_usart_instance();
  while (usart_flag_get(usart, USART_RDBF_FLAG) != RESET) {
    (void)usart_data_receive(usart);
  }

  // Clear any receiver error flags so reception can resume after a framing or
  // overrun error. Reading the data register already clears most flags; this
  // ensures the overrun flag is also reset.
  usart_flag_clear(usart, USART_PERR_FLAG | USART_FERR_FLAG | USART_NERR_FLAG |
                              USART_ROERR_FLAG);
}

bool split_transport_available(void) {
  usart_type *usart = split_usart_instance();
  return usart_flag_get(usart, USART_RDBF_FLAG) != RESET;
}

#endif // defined(SPLIT_KEYBOARD)
