// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "platform/gpio_irq.h"

#include <limits.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/platform.h"

//define an interrupt handler table slot
typedef struct {
    platform_gpio_irq_handler_t handler;
    void *context;
    uint32_t events;
} platform_gpio_irq_slot_t;

//one slot per GPIO pin, each starts unregistered
static platform_gpio_irq_slot_t slots[NUM_BANK0_GPIOS];

//has dispatcher been initialized, and if so on which core
static bool dispatcher_initialized;
static unsigned int dispatcher_core = UINT_MAX;

static bool platform_gpio_irq_events_valid(uint32_t events) {
    const uint32_t supported_events =
        GPIO_IRQ_LEVEL_LOW  |
        GPIO_IRQ_LEVEL_HIGH |
        GPIO_IRQ_EDGE_FALL  |
        GPIO_IRQ_EDGE_RISE  ;

    return events != 0U && (events & ~supported_events) == 0U;
}

//actual dispatch function, pending interrupt state has already been handled by the SDK generic GPIO handler
static void platform_gpio_irq_dispatch(
    unsigned int gpio,
    uint32_t events)
{
    //if gpio out of bounds do nothing and exit
    if (gpio >= NUM_BANK0_GPIOS) { return; }

    //pull the correct handler from the bank
    const platform_gpio_irq_slot_t slot = slots[gpio];
    //determine which of the triggered events need to be passed on
    const uint32_t relevant_events = events & slot.events;

    //if handler is registered and there are events that require addressing, call the handler
    if (slot.handler != NULL && relevant_events != 0U) {
        slot.handler(slot.context, gpio, relevant_events);
    }
}

//initialize dispatch
bool platform_gpio_irq_init(void) {
    //record the executing core
    const unsigned int current_core = get_core_num();
    //save interrupt state and disable interrupts
    const uint32_t interrupt_state = save_and_disable_interrupts();

    //if dispatcher was already initialized do not reinitialize, instead determine if it's on the core that called the initializer or not
    if (dispatcher_initialized) {
        const bool correct_core = dispatcher_core == current_core;
        restore_interrupts(interrupt_state);
        return correct_core;
    }

    //dispatcher is not initialized, first record the core that has called for initialization, it is now the core where the dispatcher lives
    dispatcher_core = current_core;

    //only place where the generic SDK GPIO callback is set, hooking it into the dispatcher
    gpio_set_irq_callback(platform_gpio_irq_dispatch);
    irq_set_enabled(IO_IRQ_BANK0, true);
    
    //set the initialized state of the dispatcher to prevent conflicting reinitializations
    dispatcher_initialized = true;
    
    //renable interrupts and restore the previous state
    restore_interrupts(interrupt_state);
    return true;
}

//register handlers
bool platform_gpio_irq_register(
    unsigned int gpio,
    uint32_t events,
    platform_gpio_irq_handler_t handler,
    void *context) 
{
    //if gpio out of range, no handler given, no valid events, dispatcher not initialized or not on current core, do nothing
    if (gpio >= NUM_BANK0_GPIOS
            || handler == NULL
            || !platform_gpio_irq_events_valid(events)
            || !dispatcher_initialized
            || dispatcher_core != get_core_num()) {
        return false;
    }

    //save interrupt state and disable interrupts
    const uint32_t interrupt_state = save_and_disable_interrupts();

    //if handler already registered do not overwrite
    if (slots[gpio].handler != NULL) {
        restore_interrupts(interrupt_state);
        return false;
    }

    //register the handler, provided context pointer, and relevant events
    slots[gpio].handler = handler;
    slots[gpio].context = context;
    slots[gpio].events = events;

    //enable gpio's interrupt events
    gpio_set_irq_enabled(gpio, events, true);

    //renable interrupts and restore prior state
    restore_interrupts(interrupt_state);
    return true;
}

//deregister handlers
bool platform_gpio_irq_unregister(unsigned int gpio)
{
    //if dispatcher not initialized or this function called on wrong core, do nothing
    if (gpio >= NUM_BANK0_GPIOS
            || !dispatcher_initialized
            || dispatcher_core != get_core_num()) {
        return false;
    }

    //record interrupt state and disable interrupts
    const uint32_t interrupt_state = save_and_disable_interrupts();

    //if no handler registered, nothing to do, reenable and restore interrupts
    if (slots[gpio].handler == NULL) {
        restore_interrupts(interrupt_state);
        return false;
    }

    //disable gpio's interrupt events
    gpio_set_irq_enabled(gpio, slots[gpio].events, false);
    //unregister the handler
    slots[gpio] = (platform_gpio_irq_slot_t){ 0 };

    //restore interrupts and reenable
    restore_interrupts(interrupt_state);
    return true;
}

//handler enable disable without deregistration
bool platform_gpio_irq_set_enabled(
    unsigned int gpio,
    bool enabled)
{
    //if dispatcher not registered or this function called on wrong core, do nothing
    if (gpio >= NUM_BANK0_GPIOS
            || !dispatcher_initialized
            || dispatcher_core != get_core_num()) {
        return false;
    }

    //record interrupt state and disable interrupts
    const uint32_t interrupt_state = save_and_disable_interrupts();

    //if no registered handler, nothing to do
    if (slots[gpio].handler == NULL) {
        restore_interrupts(interrupt_state);
        return false;
    }

    //disable the handler
    gpio_set_irq_enabled(gpio, slots[gpio].events, enabled);

    //reenable interrupts
    restore_interrupts(interrupt_state);
    return true;
}