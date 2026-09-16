# Architecture

This is the living description of Tavernkeep's current firmware architecture.
Update it as the design changes, and preserve concise rationale for choices
that materially affect implementation or future work.

## System Overview

Tavernkeep is firmware for the RP2350-based Tome e-reader and MP3 player.

The firmware is divided into platform-level hardware support and higher-level subsystems responsible for storage, filesystems, book handling, audio playback, display output, physical input, USB functionality, and system operation.

Development is currently proceeding from the lowest-level hardware interfaces upward. Removable storage is the first substantial subsystem under implementation. Higher-level filesystem, EPUB, display, audio, USB, input, and power-management functionality remains planned or only scaffolded unless otherwise documented.

Tome's final hardware uses two e-ink displays and removable SD storage. The exact physical control scheme remains unresolved and must be settled before significant hardware development proceeds.

## Design Principles

**Implement hardware functionality from documented interfaces**

Firmware is developed from hardware datasheets, interface specifications, the Pico SDK, and general embedded-system principles rather than by adapting existing e-reader firmware implementations.

**Prefer simple implementations before optimization**

Subsystems should first be implemented in a straightforward blocking or polling form where practical. More complex mechanisms such as DMA, asynchronous operation, and PIO implementations should be introduced after basic functionality has been validated.

For storage, this means proving SD operation through conventional SPI before introducing DMA or the planned 4-bit SD interface.

**Preserve clear subsystem ownership**

Low-level hardware behavior should remain within the subsystem responsible for that hardware. Higher layers should operate on meaningful subsystem interfaces rather than depending on protocol-specific details where such separation is useful.

For example, SD-specific response values remain within the SD implementation while the generic block-device interface exposes generic storage results.

## System / Subsystem Boundaries

**Platform**

The platform layer owns board-level initialization and low-level facilities shared by other Tavernkeep subsystems.

Current responsibilities include board support, debugging/logging support, and the GPIO interrupt dispatcher that owns the Pico SDK's shared GPIO callback and routes edge events to registered subsystem handlers on the core that initialized it.

The exact long-term boundary of this layer has not yet been finalized.

**Storage**

The storage subsystem owns access to removable SD storage.

Its responsibilities include:

- initializing and deinitializing the storage interface

- issuing SD commands

- transferring fixed-size storage blocks

- handling SD addressing differences

- representing storage failures through the block-device interface

- exposing block-level access to higher layers

The current implementation uses SPI.

A separate 4-bit SD backend using the RP2350's programmable I/O hardware is planned for the final hardware.

Filesystem interpretation does not belong to the block-device backend.

**Filesystem**

The filesystem layer will operate above the block-device interface and provide file-level access to removable storage.

FatFs is planned for this layer but has not yet been integrated into the current Tavernkeep implementation.

The filesystem layer should not depend on whether the underlying SD implementation uses SPI or the planned 4-bit interface.

**EPUB/Book**

The book subsystem will be responsible for opening and navigating supported book content.

EPUB and plain-text files are required book formats.

The internal architecture of EPUB parsing, text layout, caching, and rendering has not yet been defined.

M4B audiobook support is a stretch feature and therefore does not currently form part of the required architecture.

**Audio**

The audio subsystem will be responsible for MP3 playback and audio output.

The final hardware is planned around a PCM5102A DAC and PAM8908 headphone amplifier.

The detailed software architecture for decoding, buffering, scheduling, and audio output has not yet been defined.

**Display**

The display subsystem will control Tome's two e-ink displays.

The dual-display configuration is fixed and defining hardware for Tome.

The detailed display-driver, framebuffer, update scheduling, and rendering architecture has not yet been defined.

**Input**

The input subsystem will translate Tome's physical controls into firmware input events or equivalent higher-level actions.

The final physical control scheme has not yet been selected. Two rotary encoders with push input have been explored, but the control mechanism remains an open design decision.

**USB**

USB mass-storage access to Tome's removable storage is a required feature.

USB mass storage is an exclusive operating mode: while a USB host has the storage, no local reading or playback runs, and vice versa. Coordination with local filesystem access is therefore a mode transition, not concurrent sharing; see "Operating Modes and Storage Ownership". The USB class implementation itself has not yet been defined.

**Power / System Operation**

Tavernkeep will ultimately manage hardware initialization, shutdown, peripheral power state, and other system-level behavior.

Detailed power-management architecture has not yet been implemented.

## Major Interfaces and Abstractions

**Block Device**

block_device_t is the generic block-storage interface used to separate higher storage layers from the physical SD transport.

A block device contains a generic void *context used by a concrete backend to access its device-specific state.

The SPI SD implementation interprets this context as an already-created and configured sd_spi_t.

The abstraction exposes generic block-device results rather than SD protocol responses. SD-specific response interpretation remains within the SD backend.

Current block-device result categories include:

- success

- invalid argument

- not initialized

- out of range

- I/O error

- busy timeout (a card that stayed busy past the driver's declared budget; the device stays initialized so the operation can be retried)

- invalid device

- not implemented

INVALID_DEVICE represents the state in which no usable writable SD card is available.

**SPI SD Device State**

sd_spi_t contains both the fixed configuration required to communicate with an SD card and runtime state learned while communicating with the card.

Static configuration includes the SPI peripheral, the data rate after bring-up, the relevant GPIO assignments, and the sense of the card-detect switch (`card_detect_active_high`).

Runtime state currently includes:

- whether the object has been configured, and initialization state

- whether the card follows the legacy SD initialization path

- whether the card uses SDHC/SDXC block addressing

- the block count decoded from the CSD, which `get_info` reports without touching the bus

- the atomic removal latch set by the card-detect interrupt

Card type is maintained per device rather than globally.

A non-legacy SD card is not assumed to be SDHC/SDXC; version and addressing mode are treated as separate properties.

*Other subsystem interfaces*

Interfaces between the filesystem, EPUB, display, input, audio, USB, and power-management subsystems have not yet been finalized and should be documented here as they become concrete.

## Data Flow

**Storage reads/writes**

The intended storage read path is:

SD card
→ physical SD transport
→ block-device interface
→ filesystem
→ consuming subsystem

Consuming subsystems will include at least the book and audio systems.

The block-device layer operates in logical 512-byte blocks.

For SDHC/SDXC cards, logical block addresses are transmitted directly to applicable SD commands.

For SDSC cards, logical block addresses are converted to byte addresses before being sent to the card.

Writes take the same path in reverse.

**Book content**

The intended book-content path is:

removable storage
→ block device
→ filesystem
→ EPUB/book subsystem
→ rendering
→ dual-display output

The detailed buffering and rendering stages have not yet been designed.

**Audio**

The intended music path is:

removable storage
→ block device
→ filesystem
→ MP3 decoding/playback
→ digital audio output
→ PCM5102A
→ PAM8908
→ headphones

Detailed buffering and concurrency behavior remain to be designed.

**USB storage**

USB mass-storage operation will expose Tome's removable storage to an attached USB host as raw blocks, write-through, with no local filesystem mounted for the duration:

USB host
→ mass-storage class
→ block-device interface
→ physical SD transport
→ SD card

Ownership and coordination rules are defined under "Operating Modes and Storage Ownership".

## Resource Ownership

Detailed system-wide resource ownership has not yet been assigned.

Current storage ownership is more clearly defined:

- the SPI SD backend owns the SPI peripheral and GPIO resources configured for its SD interface while that device is initialized

- the SD device structure retains its static configuration across initialization and deinitialization

- successful SD deinitialization releases the SPI peripheral and associated GPIO configuration

- a failed clean deinitialization may deliberately leave the SD device initialized and its hardware resources configured so that shutdown can be retried or handled by a separate forced-shutdown path

Future ownership rules will be required for:

- DMA channels

- PIO state machines

- shared buffers

- display resources

- audio buffers and peripherals

- switchable peripheral power domains

These have not yet been assigned. Logical removable-media ownership, including during USB mass-storage operation, is assigned in the next section.

## Operating Modes and Storage Ownership

Tavernkeep runs in exactly one of three operating modes at a time:

| Mode | Storage owner | Hard real-time work | Other work |
| --- | --- | --- | --- |
| Reader | filesystem layer (FatFs over the block device) | none; user-interface responsiveness only | input, display, storage |
| Audio | filesystem layer, through the storage coordinator | keeping the I2S output fed | input, occasional display updates, storage, persistent-state writes at defined points |
| USB mass storage | the USB mass-storage class, holding the bare block device | servicing the USB stack promptly | charging/status display only |

Reading and playback never run concurrently. (Audiobook read-along, if ever implemented, would be the one exception and would be designed as a fourth mode rather than by relaxing this rule.) USB mass storage runs with every other function stopped: a plugged-in cable means either a host is using the storage or the device is in use while charging, never both. The consequence for the rest of the system is that **no mode ever has more than one hard-deadline domain**, which is what lets the cooperative execution model stand without an RTOS (see Concurrency).

**Storage has one owner at a time.** The owner is the filesystem layer in reader and audio modes and the mass-storage class in USB mode. Higher layers obtain storage only through the filesystem; the `block_device_t` is held by the filesystem adapter and, in USB mode, by the mass-storage glue, and by nothing else. This is what makes ownership enforceable rather than conventional.

**Every change of ownership is the same transition.** A card removal, entering USB mode, leaving USB mode, and a future mode switch all pass through one storage coordinator path that:

1. cancels cooperative storage consumers and invalidates open handles, cached sectors and bookmarks in memory;
2. flushes nothing to media that may be absent - persistent state is written at its defined save points before the transition, never during it;
3. waits for any in-flight backend operation to unwind;
4. unmounts or hands over the block device;
5. on the way back, treats the medium as new: a fresh mount, no reuse of any cached state, because a USB host may have changed anything and a reinserted card may be a different card.

From the filesystem layer's point of view, entering USB mode is indistinguishable from a removal and leaving it is indistinguishable from a reinsertion. The filesystem layer must therefore be written so that unmount is safe against an absent card and mount assumes a changed medium - not as if it will own the card for the lifetime of the firmware.

**USB mode is write-through.** The mass-storage class maps host reads and writes directly onto block-device operations with no caching layer in between, so a host "synchronize cache" has nothing to flush. Block-device failure results map to the corresponding medium-not-present and write-error conditions. Hot removal during USB mode is handled by the same latch as everywhere else: every block operation fails, the class reports the medium gone, and the host ejects.

**Persistent-state writes happen at named save points behind one function.** Bookmarks and settings are written when the user explicitly bookmarks and automatically at chapter transitions; nowhere else, and never from within the mode transition above. In reader mode the chapter transition coincides with a full display refresh, so the write is issued once the frame has been sent to the panels and completes during the panels' own refresh time, which is much longer than a typical write. In audio mode the same rule bounds when a card write - with its programming-busy budget - can stall the storage coordinator, which sizes the audio buffering. Before entering USB mode the pending state is written and the write is confirmed complete before the handover.

**How USB mode is selected** - automatically on host enumeration, by prompt, or by menu - is an open product decision (`requirements.md`, OPEN-002). It decides whether reader or audio mode can be interrupted by a cable, in which case the interruption is one more caller of the ownership transition above.

## Removable-Media Detection and Hot Removal

**Availability signal and debounce**

The final socket presents card-detect and write-protect information through one availability signal. The sense of that signal is a property of the socket, not of the firmware: a switch that closes to ground when a card is present gives an active-low signal (low means usable media may be present), and one that closes to ground when the socket is empty gives an active-high one. The SPI backend takes the sense as configuration (`card_detect_active_high`), defaulting to active-low; the current development breakout is active-high. In either sense the pad is pulled up, so the switch's open state reads high and only the meaning of each level and the direction of the removal edge change. Whatever the physical cause - card removal or write protection - the "absent" level means the storage device must be treated as unavailable.

The SPI backend debounces its preliminary initialization check before touching the SD bus. It requires ten consecutive samples at the present level, taken one millisecond apart, within a maximum of thirty samples. The pad's internal pull-up is selected before GPIO initialization enables the input buffer. The board-level external pull-up remains required to define the signal before firmware begins executing and while the RP2350 pad is still in its reset state. After the interrupt is armed the level is read once more, so a card that went away between the debounce and the arming is refused rather than left with a latch nothing will set.

The hot-removal interrupt is armed on the removal edge for the configured sense (rising for active-low, falling for active-high) rather than on a level. The first edge conservatively makes storage unavailable immediately and disables further edges until a fresh initialization, so mechanical contact bounce cannot produce an interrupt storm. A transaction interrupted by even an unconfirmed removal edge fails and does not resume; if the signal settles back to the present level, the card must be reinitialized before further access. The insertion edge is ignored.

**Interrupt and foreground responsibilities**

Hot removal uses a two-phase shutdown. The interrupt handler performs only bounded emergency actions, while a storage coordinator performs protocol-independent cancellation and resource teardown in foreground context.

| Immediate interrupt work | Deferred foreground work |
| --- | --- |
| Latch media unavailable and transfer cancelled | Cancel cooperative storage consumers |
| Prevent new block operations | Invalidate filesystem state and open media handles |
| Suppress duplicate card-detect events during debounce | Wait for any interrupted backend operation to unwind |
| Quiesce non-blocking autonomous transfer hardware when required | Abort and reset transfer engines safely |
| Mask related completion interrupts when required | Release DMA, PIO, SPI, GPIO, and buffer ownership |

The card-detect interrupt handler must not mount or unmount a filesystem, perform SD commands, wait for a peripheral, sleep, log, allocate memory, acquire a blocking lock, or call normal device deinitialization. In particular, it must not deinitialize a peripheral while interrupted foreground code may still be using that peripheral.

Tavernkeep does not use an RTOS, so "terminating processes" means cooperative cancellation of subsystems with outstanding storage work. The physical SD backend cannot enumerate filesystem, book, audio, display, or USB consumers; a higher-level storage coordinator must own that cancellation policy.

**Block-device and filesystem behavior**

Once removal is latched, new block operations must return INVALID_DEVICE without touching the bus. Synchronous operations already in progress must check the removal state within bounded wait loops and transfer loops so that they unwind promptly rather than waiting for their normal protocol timeout.

An interrupted read buffer is entirely invalid, regardless of how many bytes or blocks were transferred before removal. This follows the block-device contract that the requested destination contents are unspecified after any non-successful read. A late transfer-completion event must never change a cancelled request into success.

Surprise removal is not a clean filesystem unmount. Higher layers must invalidate cached state and open handles without attempting to flush data to an absent card. Firmware can limit further damage but cannot guarantee that an interrupted write left either the filesystem or the card contents consistent.

Normal deinitialization and removal teardown have different semantics. Normal deinitialization may communicate with a present card and wait for it to become ready. Removal teardown must be idempotent, must not send SD commands or poll the absent card, and must release local hardware resources after the interrupted operation has stopped. If removal occurs during a multi-block transfer, no CMD12 is sent to the absent card.

**Future PIO and DMA backend**

PIO state machines and DMA channels continue independently while the processor services a GPIO interrupt. A future PIO SD backend must therefore have an explicit cancellation path; setting a software flag alone does not immediately stop bus activity or memory transfers.

The removal ISR may latch cancellation, disable the affected PIO state machine, and mask the associated DMA completion interrupt because those are bounded register operations. It should not call `dma_channel_abort()`, which waits until the channel reaches a safe stopped state. Foreground cleanup must disable the active DMA channel and every chained channel before aborting them, as required by RP2350-E5, then acknowledge pending or late DMA interrupts, clear or restart the PIO state machines and FIFOs, place bus pins in safe states, and release owned resources. See the Pico SDK documentation for [`dma_channel_abort()`](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html) and its RP2350 errata note.

Removal, DMA completion, and chained-channel activation can race. The backend should use an explicit transfer state or generation identifier so both GPIO and DMA handlers can distinguish ACTIVE, CANCEL_REQUESTED, QUIESCED, and completed transfers. Completion handlers must check cancellation before publishing results. If storage work later spans both RP2350 cores, the shared removal and transfer state must use appropriate atomic or critical-section protection.

The Pico SDK generic GPIO callback is shared by all ordinary GPIO interrupts on a core. Card detection must therefore participate in a central GPIO dispatcher or use an independent raw shared handler rather than silently replacing another subsystem's callback. GPIO IRQ enablement and callback ownership must remain on the intended core.

**Verification requirements**

Host and hardware tests for hot removal should cover idle removal, removal during initialization, bounded waits, individual block transfer, multi-block transfer, and future PIO/DMA activity. They should verify that the ISR performs no blocking cleanup, new operations are rejected immediately, partial buffers are never consumed, teardown happens exactly once, bounce is idempotent, late completion IRQs cannot report success, chained DMA cannot retrigger, and reinsertion requires a fresh initialization.

## Error Handling Philosophy

Subsystem-specific errors should be interpreted as close as practical to the subsystem that understands them.

The SD backend therefore interprets SD protocol responses internally and maps them onto the generic block-device result type before returning errors to higher storage layers.

Protocol-specific diagnostic information may be logged internally without becoming part of the generic block-device API.

Errors that can be confidently represented generically, such as an out-of-range block request, may be returned using the corresponding block-device result. Other command, response, transfer, or timeout failures normally collapse to an I/O error at the generic storage boundary.

Storage operations use finite timeouts rather than waiting indefinitely for hardware responses.

Once an SD transaction has asserted chip select, the implementation is responsible for restoring the bus to its released state on all completed transaction paths.

A normal device shutdown is distinguished from a forced shutdown. The current SD deinitialization behavior attempts to leave the device operational if the card cannot be cleanly brought to a ready state rather than partially tearing down the peripheral and losing the ability to retry.

Broader error-handling and recovery policy for other Tavernkeep subsystems remains to be defined as those subsystems are implemented.

## Concurrency / Execution Model

Tavernkeep does not use an RTOS.

Current SD communication is synchronous and blocking. Individual SPI transfers and SD commands complete in the caller's execution context. The data CRC16 is computed one byte at a time as each payload byte leaves or enters the SPI peripheral; with the blocking per-byte transfer this costs throughput rather than hiding it, and a pipelined transfer loop is the future work that would recover it.

The initial storage implementation intentionally uses polling rather than DMA.

`src/main.c` is the demo of the highest layer that currently works, replaced as each layer lands: today it exercises the SD driver end to end and reports each step over RTT before falling into the heartbeat loop. The same `main.c` is compiled into a host test that runs it against the card model with the real driver and dispatcher underneath, so every demo is proven on the host before it is flashed and the host and hardware logs are directly comparable.

DMA is planned for bulk storage transfers after the polling implementation has been validated.

Interrupt-driven or asynchronous operation will be introduced where required by later subsystems, but a complete system-wide execution and scheduling model has not yet been defined beyond the following rule: each core runs one cooperative loop of bounded-work steps, and no operating mode places more than one hard-deadline domain on a core (see "Operating Modes and Storage Ownership"). Audio is the only current domain with a hard deadline; its decoder is written as a bounded step reading compressed data from a ring the storage coordinator fills, so whether it runs in the core 0 loop or on the second core is a launch-time placement decision sized by measurement (chiefly the display driver's blocking time), not a structural one. The no-RTOS decision is to be re-evaluated if a mode ever needs two hard-deadline domains on one core, or if long operations start acquiring hand-written yield points.

Audio playback is expected to impose stronger real-time data-flow requirements than the current storage work, but the mechanism by which audio, display updates, storage access, user input, and other work will coexist has not yet been selected.

## Important Design Decisions and Rationale

Keep consequential decisions here unless the project eventually becomes large
enough to justify a separate decision-record system.

### Decision — Develop directly on the Pico SDK without an RTOS

- Context: Tavernkeep is being developed as firmware specifically for Tome's RP2350 platform.

- Decision: Use the Raspberry Pi Pico SDK directly and do not introduce an RTOS.

- Rationale: The project is intended to implement and understand the underlying embedded mechanisms directly rather than placing system behavior behind an operating-system abstraction.

- Alternatives considered: RTOS-based development has not been selected.

- Consequences and tradeoffs: Tavernkeep must explicitly manage execution, asynchronous work, interrupts, peripheral ownership, and any required scheduling behavior.

### Decision — Implement SD access through SPI before the final 4-bit interface

- Context: The current development hardware provides an SPI-accessible microSD breakout, while Tome's final hardware is intended to use a full-size SD card and eventually a wider 4-bit native SD interface.

- Decision: Implement and validate the SD stack over SPI first.

- Rationale: SPI provides a simpler path for proving command handling, addressing, block transfers, filesystem integration, and higher storage layers before introducing the additional complexity of a PIO-based interface.

- Alternatives considered: Implementing the 4-bit interface first.

- Consequences and tradeoffs: Tavernkeep temporarily contains an SD transport that is not intended to be the final high-performance implementation, making a transport-independent block-device boundary useful.

### Decision — Use a block-device abstraction for storage

- Context: Tavernkeep is expected to operate with both the current SPI SD implementation and a planned 4-bit SD implementation.

- Decision: Higher storage layers operate through block_device_t rather than directly through an SD transport implementation.

- Rationale: The filesystem and higher-level content systems should not need to be rewritten when the SD transport changes.

- Alternatives considered: Allow the filesystem layer to call the SD SPI implementation directly.

- Consequences and tradeoffs: A small abstraction boundary is introduced, but portability to arbitrary unrelated hardware is not itself a Tavernkeep design requirement.

### Decision — Begin with polling before introducing DMA

- Context: Reliable SD command and block-transfer behavior must be established before optimizing transfer mechanisms.

- Decision: Implement the initial SD driver synchronously using polling.

- Rationale: This keeps protocol correctness and hardware bring-up easier to observe and debug.

- Alternatives considered: Implement DMA as part of initial SD bring-up.

- Consequences and tradeoffs: Initial transfer performance is not representative of the eventual optimized implementation. DMA will be introduced later without changing the block-level behavior exposed to higher layers.

### Decision — Treat card absence and hardware write protection as the same unavailable-device state

- Context: Tome's final full-size SD socket uses card-detect and write-protect switch information through the same GPIO in order to conserve GPIO resources. Tome requires writable storage for persistent device state.

- Decision: Firmware does not distinguish between an absent card and a write-protected card. Either condition means that a usable storage device is unavailable.

- Rationale: Read-only operation is not a required Tome operating mode, so distinguishing the two states provides no required functionality.

- Alternatives considered: Allocate separate GPIO inputs for card detection and write protection.

- Consequences and tradeoffs: Tavernkeep cannot inform the user which of the two physical conditions caused the device to become unavailable.

### Decision — Dual displays are fixed, the physical control scheme is not

- Context: Tome's two-display format is a defining product characteristic. The originally explored dual-encoder input system may not necessarily provide the best user experience.

- Decision: Design Tavernkeep around two e-ink displays while leaving the physical control interface open until it is deliberately selected.

- Rationale: Display count is fixed by the product concept, while changing the controls remains possible if another scheme materially improves interaction.

- Alternatives considered: Treating both display and control hardware as fixed.

- Consequences and tradeoffs: Input architecture should not become unnecessarily coupled to an unfinalized physical control arrangement during early development.

## Hardware Assumptions Relevant to Firmware

**Processor**

Tome is based on the RP2350.

Firmware is developed using the Raspberry Pi Pico SDK.

Current development uses a Raspberry Pi Pico 2.

**Removable storage**

The final Tome hardware uses a full-size SD card socket.

Current development uses an Adafruit MicroSD card breakout board+ on a breadboard, accessed through SPI1. That breakout carries no pull-ups on any SD line, its 74AHC125 level shifter sits on DI, CLK and CS, and its socket's detect switch is active-high. Until the final board's discrete pull-ups exist, `main.c` enables the RP2350 internal pull-ups on DO and CS: the card leaves DO undriven until CMD0 moves it into SPI mode, and without a pull-up the driver's pre-command ready wait sees a floating line and reports BUSY_TIMEOUT before CMD0 is ever sent.

The current SD implementation begins communication at approximately 400 kHz and increases SPI speed only after successful card initialization. 1 MHz is the only post-initialization rate demonstrated on hardware so far.

Tome operates SD cards from a 3.3 V interface.

The final socket's write-protect switching is used as one firmware-visible availability signal that also incorporates card-detect (this decision was made based on the particular socket's truth table so if a hardware change happens here this may need to be reevaluated).

An external pull-up is provided for this signal.

The SPI backend also selects the RP2350 pad's internal pull-up before enabling the GPIO input and applies bounded software debounce to its initial availability check. The external pull-up remains necessary because software cannot alter the pad state before firmware executes.

**Displays**

Tome uses two e-ink displays.

The currently selected displays are Good Display GDEH0576T81 panels using the SSD2677 controller.

The dual-display configuration is a fixed architectural assumption.

Detailed firmware-side display timing, buffering, update strategy, and memory requirements have not yet been established.

**Audio**

The planned audio path uses:

- PCM5102A digital-to-analog conversion

- PAM8908 headphone amplification

Audio data will be supplied digitally to the DAC.

The detailed firmware timing and buffering requirements have not yet been established.

**Physical controls**

The physical control scheme remains unresolved.

Two rotary encoders with push input have been explored but are not currently a fixed architectural assumption.

**USB**

Tome provides USB connectivity and is required to support USB mass-storage access to removable storage.

The detailed firmware ownership and synchronization model for this access remains unresolved.

## Known Architectural Limitations

The current firmware architecture is incomplete because development is still concentrated on storage bring-up.

At the current stage:

- only the SPI SD transport exists; it is functionally complete against the block-device contract and has one real-card run at 1 MHz behind it

- the planned 4-bit SD backend does not yet exist

- FatFs has not yet been integrated

- DMA-based storage transfer has not yet been implemented

- the filesystem-to-USB ownership model has not yet been defined

- the final physical input hardware has not been selected

- EPUB parsing and rendering architecture has not yet been defined

- display buffering and update scheduling have not yet been defined

- MP3 decoding and audio-buffer scheduling have not yet been defined

- the overall asynchronous/concurrency model beyond the current blocking storage implementation has not yet been defined

- system-wide power-management architecture has not yet been defined

These are development-state limitations rather than necessarily permanent restrictions.

## Planned / Future Architecture

**4-bit SD backend**

A separate SD backend using the RP2350's PIO resources is planned for the final Tome hardware.

It is intended to provide the same block-device behavior to higher layers as the SPI backend so that filesystem and application code do not depend on the physical SD transport.

SPI support is intended to remain useful for hardware bring-up even after the 4-bit backend is introduced.

**DMA-backed storage transfer**

After the polling SD implementation has been validated, bulk block transfers are planned to use DMA.

The exact asynchronous API and DMA ownership model have not yet been designed. They must implement the cancellation, IRQ ordering, buffer invalidation, RP2350-E5 abort sequence, and resource-release rules described in Removable-Media Detection and Hot Removal.

**Filesystem integration**

FatFs is planned above the block-device interface.

The adapter between FatFs and block_device_t has not yet been implemented.

**Input/events**

A general input/event mechanism is expected to be required as Tavernkeep gains display, audio, and physical controls, but its structure should be designed after the physical control scheme and actual subsystem needs become clearer.

**Displays**

Tavernkeep will require a driver and higher-level rendering path for both e-ink displays.

Framebuffer strategy, partial-update policy, rendering ownership, and display scheduling remain to be determined.

**EPUB**

Tavernkeep will require EPUB file access, parsing, layout, navigation, persistent reading position, and bookmark support.

The internal architecture for these functions has not yet been designed.

**Audio**

Tavernkeep will require MP3 decoding and continuous delivery of audio to the hardware audio path.

Buffering, DMA use, timing, decoder selection or implementation, and interaction with simultaneous storage/display activity remain to be determined.

**USB mass storage**

USB mass-storage access to removable storage is required.

A safe ownership model between the USB host and Tavernkeep's own filesystem access must be defined before this feature is implemented.

**Power management**

Power management will eventually coordinate processor activity, peripheral power state, display behavior, audio hardware, removable storage, and system shutdown.

The detailed architecture remains to be designed.
