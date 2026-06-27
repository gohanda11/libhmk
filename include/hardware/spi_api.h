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

#pragma once

#include "common.h"

//--------------------------------------------------------------------+
// Soft SPI API
//--------------------------------------------------------------------+

/**
 * @brief Initialize a bit-banged SPI master
 *
 * @param sck_port Zero-based GPIO port for SCK
 * @param sck_pin  GPIO pin number for SCK
 * @param mosi_port Zero-based GPIO port for MOSI
 * @param mosi_pin  GPIO pin number for MOSI
 * @param miso_port Zero-based GPIO port for MISO
 * @param miso_pin  GPIO pin number for MISO
 *
 * @return None
 */
void spi_soft_init(uint32_t sck_port, uint32_t sck_pin, uint32_t mosi_port,
                   uint32_t mosi_pin, uint32_t miso_port, uint32_t miso_pin);

/**
 * @brief Configure the SDIO pin direction for 3-wire SPI operation
 *
 * When MOSI and MISO share a single bidirectional pin, call this helper
 * before read operations to switch the pin to input and after read
 * operations to return it to output.
 *
 * @param input true for input mode, false for output mode
 *
 * @return None
 */
void spi_soft_set_sdio_input(bool input);

/**
 * @brief Set the SPI mode (CPOL/CPHA)
 *
 * @param cpol Clock polarity (0 = idle low, 1 = idle high)
 * @param cpha Clock phase (0 = sample on first edge, 1 = sample on second edge)
 *
 * @return None
 */
void spi_soft_set_mode(uint8_t cpol, uint8_t cpha);

/**
 * @brief Set the SPI clock frequency
 *
 * This is a best-effort setting based on busy-wait cycle counting.
 *
 * @param hz Target SPI clock frequency in Hz
 *
 * @return None
 */
void spi_soft_set_frequency(uint32_t hz);

/**
 * @brief Transfer a single byte over soft SPI
 *
 * @param tx Byte to transmit
 *
 * @return Byte received on MISO during the transfer
 */
uint8_t spi_soft_transfer_byte(uint8_t tx);
