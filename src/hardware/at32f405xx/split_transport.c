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

#if !defined(SPLIT_UART_TURNAROUND_DELAY_US)
// Short guard time between the end of a half-duplex transmission and the
// release of the driver. This gives the receiver time to switch from TX back
// to RX before the other half starts its response.
#define SPLIT_UART_TURNAROUND_DELAY_US 20
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

static void split_transport_turnaround_delay(void) {
#if SPLIT_UART_TURNAROUND_DELAY_US > 0
  // Rough microsecond busy loop. The divisor is an estimate of the cycles
  // consumed by one loop iteration; the exact value is not critical.
  const uint32_t count =
      (F_CPU / 1000000UL) * SPLIT_UART_TURNAROUND_DELAY_US / 8;
  for (volatile uint32_t i = 0; i < count; i++)
    ;
#endif
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

bool split_transport_send(const uint8_t *data, uint8_t len) {
  usart_type *usart = split_usart_instance();

  split_transport_enable_tx();

  for (uint8_t i = 0; i < len; i++) {
    uint32_t timeout = 10000;
    while (usart_flag_get(usart, USART_TDBE_FLAG) == RESET) {
      if (--timeout == 0) {
        split_transport_disable_tx();
        return false;
      }
    }
    usart_data_transmit(usart, data[i]);
  }

  // Wait for transmission complete
  uint32_t timeout = 10000;
  while (usart_flag_get(usart, USART_TDC_FLAG) == RESET) {
    if (--timeout == 0) {
      split_transport_disable_tx();
      return false;
    }
  }

  // Small guard time before releasing the line in half-duplex mode so the
  // other half is ready to receive the response.
  split_transport_turnaround_delay();

  split_transport_disable_tx();
  return true;
}

bool split_transport_receive(uint8_t *data, uint8_t len, uint32_t timeout_ms) {
  usart_type *usart = split_usart_instance();
  const uint32_t start = timer_read();

  for (uint8_t i = 0; i < len; i++) {
    while (usart_flag_get(usart, USART_RDBF_FLAG) == RESET) {
      if (timer_elapsed(start) >= timeout_ms)
        return false;
    }
    data[i] = (uint8_t)usart_data_receive(usart);
  }

  return true;
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
