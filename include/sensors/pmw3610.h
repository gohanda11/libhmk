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
// PMW3610 Registers
//--------------------------------------------------------------------+

#define PMW3610_REG_PRODUCT_ID 0x00
#define PMW3610_REG_REVISION_ID 0x01
#define PMW3610_REG_MOTION 0x02
#define PMW3610_REG_DELTA_X_L 0x03
#define PMW3610_REG_DELTA_Y_L 0x04
#define PMW3610_REG_DELTA_XY_H 0x05
#define PMW3610_REG_PERFORMANCE 0x11
#define PMW3610_REG_MOTION_BURST 0x12
#define PMW3610_REG_RUN_DOWNSHIFT 0x1B
#define PMW3610_REG_REST1_RATE 0x1C
#define PMW3610_REG_REST1_DOWNSHIFT 0x1D
#define PMW3610_REG_REST2_RATE 0x1E
#define PMW3610_REG_REST2_DOWNSHIFT 0x1F
#define PMW3610_REG_REST3_RATE 0x20
#define PMW3610_REG_OBSERVATION 0x2D
#define PMW3610_REG_POWER_UP_RESET 0x3A
#define PMW3610_REG_SPI_CLK_ON_REQ 0x41
#define PMW3610_REG_RES_STEP 0x85
#define PMW3610_REG_SPI_PAGE0 0x7F
#define PMW3610_REG_SPI_PAGE1 0xFF

#define PMW3610_PRODUCT_ID 0x3E
#define PMW3610_POWERUP_CMD_RESET 0x5A
#define PMW3610_SPI_CLOCK_CMD_ENABLE 0xBA
#define PMW3610_SPI_CLOCK_CMD_DISABLE 0xB5
#define PMW3610_SPI_WRITE_BIT 0x80

#define PMW3610_BURST_SIZE 7
#define PMW3610_BURST_X_L_POS 1
#define PMW3610_BURST_Y_L_POS 2
#define PMW3610_BURST_XY_H_POS 3

#define PMW3610_MIN_CPI 200
#define PMW3610_MAX_CPI 3200

//--------------------------------------------------------------------+
// PMW3610 Diagnostics
//--------------------------------------------------------------------+

typedef struct {
  uint8_t product_id;
  uint8_t observation;
  uint8_t motion;
  bool irq_low;
  bool init_ok;
} pmw3610_info_t;

//--------------------------------------------------------------------+
// PMW3610 API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the PMW3610 sensor
 *
 * @return true if initialization succeeded, false otherwise
 */
bool pmw3610_init(void);

/**
 * @brief Read accumulated motion deltas from the PMW3610 sensor
 *
 * @param dx Pointer to store the X-axis delta
 * @param dy Pointer to store the Y-axis delta
 *
 * @return true if motion was detected, false otherwise
 */
bool pmw3610_read_motion(int16_t *dx, int16_t *dy);

/**
 * @brief Read diagnostic information from the PMW3610 sensor
 *
 * @param info Pointer to store the diagnostic data
 *
 * @return None
 */
void pmw3610_get_info(pmw3610_info_t *info);
