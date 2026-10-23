// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef TAVERNKEEP_PLATFORM_GPIO_IRQ_H
#define TAVERNKEEP_PLATFORM_GPIO_IRQ_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*platform_gpio_irq_handler_t)(
    void *context,
    unsigned int gpio,
    uint32_t events
);

//called once on the core that owns GPIO interrupts
bool platform_gpio_irq_init(void);

//each GPIO has one owner, registration must happen on the owning core
bool platform_gpio_irq_register(
    unsigned int gpio,
    uint32_t events,
    platform_gpio_irq_handler_t handler,
    void *context
);

//unregister the interrupt handler
bool platform_gpio_irq_unregister(
    unsigned int gpio
);

//enable or disable the handler without deregistering or reregistering it
bool platform_gpio_irq_set_enabled(
    unsigned int gpio,
    bool enabled
);

#endif