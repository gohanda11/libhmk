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

#include "hardware/hardware.h"
#include "hardware/spi_api.h"

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static uint32_t sck_port;
static uint32_t sck_pin;
static uint32_t mosi_port;
static uint32_t mosi_pin;
static uint32_t miso_port;
static uint32_t miso_pin;
static bool is_3wire = false;

static uint8_t cpol = 0;
static uint8_t cpha = 0;
static uint32_t half_period_cycles = 0;

//--------------------------------------------------------------------+
// Delay Helpers
//--------------------------------------------------------------------+

static void spi_soft_delay_cycles(uint32_t cycles) {
  const uint32_t start = board_cycle_count();
  while ((board_cycle_count() - start) < cycles)
    ;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void spi_soft_init(uint32_t sck_p, uint32_t sck_n, uint32_t mosi_p,
                   uint32_t mosi_n, uint32_t miso_p, uint32_t miso_n) {
  sck_port = sck_p;
  sck_pin = sck_n;
  mosi_port = mosi_p;
  mosi_pin = mosi_n;
  miso_port = miso_p;
  miso_pin = miso_n;

  // If MOSI and MISO share the same pin, the bus operates in 3-wire mode
  // with a single bidirectional SDIO line.
  is_3wire = (mosi_port == miso_port) && (mosi_pin == miso_pin);

  gpio_set_output(sck_port, sck_pin);
  gpio_set_output(mosi_port, mosi_pin);
  if (!is_3wire) {
    gpio_set_input(miso_port, miso_pin);
  }

  // Default to SPI mode 0 at 1 MHz
  spi_soft_set_mode(0, 0);
  spi_soft_set_frequency(1000000);
}

void spi_soft_set_sdio_input(bool input) {
  if (!is_3wire)
    return;

  if (input) {
    gpio_set_input(mosi_port, mosi_pin);
  } else {
    gpio_set_output(mosi_port, mosi_pin);
  }
}

void spi_soft_set_mode(uint8_t cpol_val, uint8_t cpha_val) {
  cpol = cpol_val;
  cpha = cpha_val;

  // Ensure clock is at idle level
  gpio_write(sck_port, sck_pin, cpol != 0);
}

void spi_soft_set_frequency(uint32_t hz) {
  const uint32_t system_clock = board_clock_frequency();
  if (hz == 0 || system_clock == 0)
    return;

  // Half-period in CPU cycles, rounded up
  half_period_cycles = (system_clock + hz - 1) / hz / 2;
  if (half_period_cycles == 0)
    half_period_cycles = 1;
}

uint8_t spi_soft_transfer_byte(uint8_t tx) {
  uint8_t rx = 0;

  for (int32_t i = 7; i >= 0; i--) {
    const bool bit = ((tx >> i) & 1) != 0;

    if (cpha != 0) {
      // Mode 1/3: data changes on the first (trailing) edge and is sampled on
      // the second (leading) edge.
      // We use falling edge as the first edge and rising edge as the second.
      gpio_write(sck_port, sck_pin, cpol == 0);
      spi_soft_delay_cycles(half_period_cycles);
      gpio_write(mosi_port, mosi_pin, bit);
      gpio_write(sck_port, sck_pin, cpol != 0);
      spi_soft_delay_cycles(half_period_cycles);
      rx = (uint8_t)((rx << 1) | (gpio_read(miso_port, miso_pin) ? 1 : 0));
    } else {
      // Mode 0/2: data changes on the first (leading) edge and is sampled on
      // the second (trailing) edge.
      gpio_write(mosi_port, mosi_pin, bit);
      gpio_write(sck_port, sck_pin, cpol == 0);
      spi_soft_delay_cycles(half_period_cycles);
      rx = (uint8_t)((rx << 1) | (gpio_read(miso_port, miso_pin) ? 1 : 0));
      gpio_write(sck_port, sck_pin, cpol != 0);
      spi_soft_delay_cycles(half_period_cycles);
    }
  }

  // Leave clock at idle level
  gpio_write(sck_port, sck_pin, cpol != 0);

  return rx;
}
