// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "platform/board.h"

#include "pico/stdlib.h"

void board_init(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    (void)board_status_led_set(false);
#endif
#if defined(PICO_VBUS_PIN)
    gpio_init(PICO_VBUS_PIN);
    gpio_set_dir(PICO_VBUS_PIN, GPIO_IN);
    gpio_disable_pulls(PICO_VBUS_PIN);
#endif
}

bool board_status_led_available(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    return true;
#else
    return false;
#endif
}

bool board_status_led_set(bool on)
{
#if defined(PICO_DEFAULT_LED_PIN)
#if defined(PICO_DEFAULT_LED_PIN_INVERTED) && PICO_DEFAULT_LED_PIN_INVERTED
    gpio_put(PICO_DEFAULT_LED_PIN, !on);
#else
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#endif
    return true;
#else
    (void)on;
    return false;
#endif
}


bool board_vbus_sense_available(void)
{
#if defined(PICO_VBUS_PIN)
    return true;
#else
    return false;
#endif
}

bool board_vbus_present(void)
{
#if defined(PICO_VBUS_PIN)
    return gpio_get(PICO_VBUS_PIN);
#else
    return true;
#endif
}
