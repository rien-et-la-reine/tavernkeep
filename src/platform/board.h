/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

#ifndef TAVERNKEEP_PLATFORM_BOARD_H
#define TAVERNKEEP_PLATFORM_BOARD_H

#include <stdbool.h>

void board_init(void);

bool board_status_led_available(void);
bool board_status_led_set(bool on);

/* VBUS sensing: whether a USB cable supplies power. The USB controller cannot tell (TinyUSB forces its own
 * VBUS detection on), so a cable's removal is seen only here. The Pico 2 reads VBUS on PICO_VBUS_PIN
 * through its own divider; the final board is to have one of its own (docs/architecture.md, board design
 * notes). The pin is a plain input with its pulls off: the pull-down every pad has at reset, in parallel
 * with the divider's lower resistor, would pull the high level down. */
bool board_vbus_sense_available(void);

/* One sample of the VBUS sense pin, not debounced. True on a board without VBUS sensing, which then
 * behaves as if a cable were always connected. */
bool board_vbus_present(void);

#endif

