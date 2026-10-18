# Architecture

This is the living description of Tavernkeep's current firmware architecture.
Update it as the design changes, and preserve concise rationale for choices
that materially affect implementation or future work.

## System Overview

Tavernkeep is firmware for the RP2350-based Tome e-reader and MP3 player.

The firmware is divided into platform-level hardware support and higher-level subsystems responsible for storage, filesystems, book handling, audio playback, display output, physical input, USB functionality, and system operation.

Development is currently proceeding from the lowest-level hardware interfaces upward. Removable storage is the first substantial subsystem: the SPI SD driver, the filesystem layer and the storage coordinator are implemented and validated on hardware. EPUB, display, audio, USB, input, and power-management functionality remains planned or only scaffolded unless otherwise documented.

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

The filesystem layer operates above the block-device interface and provides file-level access to removable storage.

FatFs is vendored, configured and built as this layer's implementation. The layer and its FatFs disk I/O adapter are implemented (see "Filesystem" under Major Interfaces) and have passed a host smoke test over a RAM-backed block device and run on a real card at 1 MHz, including hot removal while idle and during writes and save points (VAL-H02 and VAL-H03 in `docs/validation.md`). They have not yet run against the card model, a host simulation of an SD card that is kept outside the repository (see Concurrency / Execution Model).

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

Peripheral power is to be switched as one rail under GPIO control, so that the device can sleep with everything but the microcontroller powered down and wake without a reboot (see "Power management" under Planned / Future Architecture). The power manager that drives it has not yet been implemented; the storage coordinator can already release the card for it.

## Major Interfaces and Abstractions

**Block Device**

block_device_t is the generic block-storage interface used to separate higher storage layers from the physical SD transport.

A block device contains a generic void *context used by a concrete backend to access its device-specific state.

The SPI SD implementation interprets this context as an already-created and configured sd_spi_t.

The abstraction exposes generic block-device results rather than SD protocol responses. SD-specific response interpretation remains within the SD backend.

A successful write means the device has accepted the data, not that it is on the medium, so that a later DMA backend can return while a transfer is still in flight. Every device therefore provides `sync`, which returns OK only once every accepted write is on the medium, and reports any accepted write that failed afterwards. A read after an accepted write returns that write's data whether or not it has been synced. The FatFs adapter forwards `CTRL_SYNC` to `sync`, so a file close only succeeds once its data has landed. The SPI SD backend currently waits out the card's programming busy on every write, so its `sync` only confirms the card is idle.

Besides init, deinit, block reads and writes, `get_info` and `sync`, a device may offer `is_present`: one undebounced sample of the presence signal, with no bus traffic, that works whether or not the device is initialized. It is how an insertion becomes visible, since an uninitialized device has nothing else watching the socket. It is a hint: `init` still does the debounced check. It reports the socket, not the removal latch, so after a removal and reinsertion it reads true while the device still refuses operations until it is re-initialized.

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

- the data rate actually in use after bring-up (`sd_spi_bus_rate_hz`): the nearest rate at or below the configured one that the SPI peripheral's clock divides to

- the atomic removal latch set by the card-detect interrupt

- whether the card-detect pin has been claimed as a pulled-up input; once it has, the device keeps it across deinitialization and failed initializations (see Resource Ownership)

Card type is maintained per device rather than globally.

A non-legacy SD card is not assumed to be SDHC/SDXC; version and addressing mode are treated as separate properties.

**Filesystem**

`src/storage/filesystem.h` is the only way above the storage layer to reach removable storage. It is a single module rather than an object: there is one card, FatFs is configured for one volume, and storage has one owner at a time.

- FatFs is private to the layer. No FatFs type, header or result code appears in the interface, and only `filesystem.c` and `diskio.c` are built with the FatFs include path, so an `#include "ff.h"` anywhere else fails to build. FatFs's time seam, `get_fattime()`, is therefore supplied by `filesystem.c`, which packs the generic time from `platform/clock.h`.
- The FatFs disk I/O adapter (`src/storage/diskio.c`) is part of the layer. Its bind/unbind functions are declared in the private `filesystem_diskio.h`, which nothing above the layer includes.
- Open files and directories live in fixed pools inside the layer. Callers hold handles made of a slot number and that slot's generation; the generation advances on open, close and every unmount, and 0 is never issued. A stale handle is therefore detected rather than used, and unmount invalidates every handle at once without any I/O.
- `filesystem_mount` takes an already-initialized block device from the storage coordinator and mounts immediately, so an absent card or a card without a FAT volume is reported at mount. `filesystem_unmount` performs no I/O, flushes nothing and always succeeds. `filesystem_format` creates a new FAT volume on an unmounted card, for cards that arrive formatted as exFAT.
- The layer is cooperative and single-threaded: it is called only from the core running the storage coordinator, never from an interrupt handler, and has no locking.
- Operations that would corrupt the volume because an entry is open are refused with `FILESYSTEM_RESULT_IN_USE`: removing or renaming an open file, removing a directory being listed, opening a file for replacement while another handle has it open, and opening a file that is open for replacement. Several handles may still read one file, and a directory being listed may be renamed, since the listing follows its clusters rather than its name. FatFs enforces the rules for files with its table of open entries (`FF_FS_LOCK` 5, one entry for each file and directory the pools can hold open, checked at compile time), which costs about 80 bytes and a short scan on each open, remove and rename. That table does not cover a directory being listed - FatFs records a listing under the directory's own cluster but looks a directory up for removal by its entry in the parent - so `filesystem_remove` checks the open listing itself. The layer releases a file's entry even when its close fails, and an unmount empties the table, so a removal cannot leave entries behind. This replaced an earlier rule that left open entries to the callers, which held only while every file had exactly one owner and turned any mistake into silent corruption rather than an error.

**Storage coordinator**

`src/storage/storage.h` is the owner of the removable card. It holds the block device for the life of the firmware and is the only code that initializes or deinitializes it, mounts, unmounts or formats the filesystem, or lends the device to the USB mass-storage class. It runs as `storage_step(now_ms)` in the cooperative loop of the core that also calls the filesystem layer.

Its state is two values. The *owner* is who may use the card: the filesystem layer (local), the USB mass-storage class, or nobody (none: the card released so that the peripheral rail can be powered down; see "Power management"). The *medium* is the card's condition for that owner: absent, preparing (being brought up, including the settle wait after insertion and the waits between attempts), faulty, unformatted (local only: works, but holds no FAT volume), or ready (local: mounted; USB: initialized and lent).

- **Removal** is noticed in the step after the card-detect interrupt latches it: `get_info` then reports `INVALID_DEVICE` without touching the bus. The response is the ownership transition: unmount (no I/O), deinitialize (on a latched device, the driver skips the bus), medium absent. **Insertion** is noticed by polling `is_present` while the device is down. An idle step costs one of these two checks and nothing else.
- **Bring-up** waits first: a new card must read present for `STORAGE_INSERT_SETTLE_MS` before its first attempt, so that it has seated and powered up (see "Availability signal and debounce"). It is then one attempt per step: init, then for the local owner mount. A card the socket reports but the debounced check refuses is still being inserted; it is not counted, and settles again before the next attempt. Any other failure is counted, and after a fixed number of attempts the card is marked faulty, released, and left alone until it leaves the socket. A mount that fails is a removal if the device has stopped answering, whatever the mount reported: a card pulled before the adapter's `disk_initialize` shows as medium unavailable, one pulled during the mount's sector reads as a storage error. If the device still answers, the failure is counted, including a medium-unavailable result, which then means a card the adapter refuses (sector size, capacity, read-only); treated as a removal, that card would be brought up again forever.
- **Requests** - an owner change, a format - are recorded and carried out at the top of a later step, where no consumer is part-way through a call. Callers watch the owner, medium and `storage_request_pending` for the outcome. Formatting needs a confirmed local request and an unformatted or ready card; it is dropped if the card or owner changes first.
- **The epoch** is a counter that changes whenever something a consumer may have cached stops being valid: whenever the medium stops being ready for its owner, and on every owner change. Consumers record it with what they cache and discard the cache when it differs; it is readable from either core. Stale filesystem handles enforce the same thing as a backstop.
- **Release** is the change to the none owner. It unmounts with no I/O and deinitializes the card cleanly: unlike the removal path, the card is present, so the driver waits out any programming busy, and once the deinit succeeds the card can lose power safely. A card being settled or brought up is dropped. While released, bring-up stops and the socket is not watched, so an idle step costs nothing. A card that stays busy is retried at the retry interval, `STORAGE_RELEASE_ATTEMPTS` times, and the request stays pending until it succeeds; after the last attempt the medium is reported faulty with the card still held, the request stops being pending, and whether to cut the rail anyway is the power manager's decision. A removal still releases a held card through the latch. Leaving the none owner treats whatever is in the socket as a card just inserted, settle wait included, because the rail was probably off and the card may have been swapped.

The coordinator is built for the consumers it does not have yet. A consumer that must reach a safe point before a transition - persistent state written before a USB handover, the audio stream stopped - adds that check where requests are carried out, and the request simply stays pending until it passes; the interface does not change. Bounded service work for consumers, such as the audio stream's read-ahead into the decoder's ring, has a marked place at the end of the step, after transitions, so it never runs against a card that is being lost or handed over. The USB mass-storage class will take the device from `storage_usb_device` and compare epochs to report a medium change to the host.

**Save points**

`src/storage/save_point.h` is how persistent state - bookmarks, settings - is written and read back. It sits on top of the filesystem layer and runs where that layer runs. A record is small (at most `SAVE_POINT_DATA_MAX` bytes) and is always saved whole.

- Each record lives in two slot files, the record's name with `.a` and `.b` appended. Each slot holds a magic number, a sequence number, the data length, the data and a CRC-16 over all of it (the same CRC-16 the SD driver uses for its data blocks, from `sd_crc.h`) (the exact format is in `save_point.h`).
- A save checks both slots, then rewrites the one that does not hold the newest valid record, with the next sequence number. Its result is OK once the slot file's close succeeds, which is when the record is confirmed on the card.
- A read checks both slots and returns the valid one with the higher sequence number. A slot that is missing, short, the wrong length or fails its CRC holds no record; it is not an error. Sequence numbers are compared across a wrap.
- An interrupted save can damage only the slot it was rewriting, which then fails its checks, so a read still finds the previous record. That holds for the very first save too: it leaves no valid record, and the read reports that there is none, rather than returning a partial one.
- Nothing is renamed or removed. FatFs writes a renamed entry's new name before removing the old one, so an interrupted rename can leave two names sharing one cluster chain, and removing either then frees the other's data (`filesystem_rename` in `filesystem.h`). An earlier temporary-file-and-rename scheme was replaced for that reason, and because an interrupted first save left only an incomplete temporary that its recovery rule would have promoted.

*Other subsystem interfaces*

Interfaces between the EPUB, display, input, audio, USB, and power-management subsystems have not yet been finalized and should be documented here as they become concrete.

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

- successful SD deinitialization releases the SPI peripheral and associated GPIO configuration, except the card-detect pin: the SD device claims it as a pulled-up input at configure and keeps it for the life of the device, because reading it is the only way to see an insertion while the device is uninitialized

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

**Storage has one owner at a time.** The owner is the filesystem layer in reader and audio modes and the mass-storage class in USB mode. Higher layers obtain storage only through the filesystem. The `block_device_t` is held by the storage coordinator, which initializes and deinitializes it and alone decides who may use it; by the filesystem adapter while a volume is mounted; and, in USB mode, by the mass-storage glue. Nothing else holds it. This is what makes ownership enforceable rather than conventional.

**Every change of ownership is the same transition.** A card removal, entering USB mode, leaving USB mode, releasing the card before the peripheral rail is powered down, and a future mode switch all pass through one storage coordinator path that:

1. cancels cooperative storage consumers and invalidates open handles, cached sectors and bookmarks in memory;
2. flushes nothing to media that may be absent - persistent state is written at its defined save points before the transition, never during it;
3. waits for any in-flight backend operation to unwind;
4. unmounts or hands over the block device;
5. on the way back, treats the medium as new: a fresh mount, no reuse of any cached state, because a USB host may have changed anything and a reinserted card may be a different card.

From the filesystem layer's point of view, entering USB mode is indistinguishable from a removal and leaving it is indistinguishable from a reinsertion. The filesystem layer must therefore be written so that unmount is safe against an absent card and mount assumes a changed medium - not as if it will own the card for the lifetime of the firmware.

**USB mode is write-through.** The mass-storage class maps host reads and writes directly onto block-device operations with no caching layer in between, so a host "synchronize cache" has nothing to flush. Block-device failure results map to the corresponding medium-not-present and write-error conditions. Hot removal during USB mode is handled by the same latch as everywhere else: every block operation fails, the class reports the medium gone, and the host ejects.

**Persistent-state writes happen at named save points behind one function.** Bookmarks and settings are written when the user explicitly bookmarks and automatically at chapter transitions; nowhere else, and never from within the mode transition above. In reader mode the chapter transition coincides with a full display refresh, so the write is issued once the frame has been sent to the panels and completes during the panels' own refresh time, which is much longer than a typical write. In audio mode the same rule bounds when a card write - with its programming-busy budget - can stall the storage coordinator, which sizes the audio buffering. Before entering USB mode the pending state is written and the write is confirmed complete before the handover.

The one function is `save_point_write` (see "Save points" under Major Interfaces). Each save rewrites whichever of a record's two slot files does not hold the newest valid copy, so an interruption at any moment - a removal, a USB handover forced by a cable, a power loss - leaves the previous record readable, and a read never returns a partly written one.

**How USB mode is selected** - automatically on host enumeration, by prompt, or by menu - is an open product decision (`requirements.md`, OPEN-002). It decides whether reader or audio mode can be interrupted by a cable, in which case the interruption is one more caller of the ownership transition above.

## Removable-Media Detection and Hot Removal

**Availability signal and debounce**

The final socket presents card-detect and write-protect information through one availability signal. The sense of that signal is a property of the socket, not of the firmware: a switch that closes to ground when a card is present gives an active-low signal (low means usable media may be present), and one that closes to ground when the socket is empty gives an active-high one. The SPI backend takes the sense as configuration (`card_detect_active_high`), defaulting to active-low; the current development breakout is active-high. In either sense the pad is pulled up, so the switch's open state reads high and only the meaning of each level and the direction of the removal edge change. Whatever the physical cause - card removal or write protection - the "absent" level means the storage device must be treated as unavailable.

The SPI backend debounces its preliminary initialization check before touching the SD bus. It requires ten consecutive samples at the present level, taken one millisecond apart, within a maximum of thirty samples. The pad's internal pull-up is selected before GPIO initialization enables the input buffer. The board-level external pull-up remains required to define the signal before firmware begins executing and while the RP2350 pad is still in its reset state. After the interrupt is armed the level is read once more, so a card that went away between the debounce and the arming is refused rather than left with a latch nothing will set.

That debounce rejects contact bounce; it does not wait for the card itself. The detect switch closes before the card's contacts have seated and before the card has powered up, so an initialization attempted the moment the socket reports a card fails on a good card - on every reinsertion in VAL-H02, and on all three attempts of a deliberately slow one. The storage coordinator therefore waits `STORAGE_INSERT_SETTLE_MS` (500 ms) after a card first appears, sampling the socket on every step and restarting the wait on any empty reading, before its first bring-up attempt. The wait applies to every new card, including one present at boot; so does a card that init refuses as not seated, since it is still arriving. Retries after a counted failure follow a card that has already settled and do not wait again.

The hot-removal interrupt is armed on the removal edge for the configured sense (rising for active-low, falling for active-high) rather than on a level. The first edge conservatively makes storage unavailable immediately and disables further edges until a fresh initialization, so mechanical contact bounce cannot produce an interrupt storm. A transaction interrupted by even an unconfirmed removal edge fails and does not resume; if the signal settles back to the present level, the card must be reinitialized before further access. The insertion edge is ignored by the interrupt; insertion is noticed by the storage coordinator polling the device's presence signal (`is_present`) while the device is uninitialized.

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

Tavernkeep does not use an RTOS, so "terminating processes" means cooperative cancellation of subsystems with outstanding storage work. The physical SD backend cannot enumerate filesystem, book, audio, display, or USB consumers; a higher-level storage coordinator must own that cancellation policy. That is `src/storage/storage.h` (see "Storage coordinator" under Major Interfaces); today it tells consumers through its epoch, and the place for consumers that must reach a safe point first is reserved in its step.

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

`src/main.c` is the demo of the highest layer that currently works, replaced as each layer lands: today it runs the storage coordinator in the loop, logs every change of owner, medium and epoch, and each time the card becomes ready runs a filesystem demo (label, root listing, a counter read from and saved through a save point, and a 16 KiB file written and read back in single calls so that FatFs issues multi-block transfers) whose steps are reported over RTT. It never formats a card unless `TAVERNKEEP_DEMO_FORMAT` is 1, set in `main.c` or with `-DTAVERNKEEP_DEMO_FORMAT=1` on the compiler command line; there is deliberately no CMake option for it, so it cannot be left switched on in a configured build.

Host tests are kept outside the repository. The storage-layer suite (`tests/filesystem/`, local only) runs the save points, the coordinator, the filesystem layer, the adapter and the real FatFs over a RAM-backed fake card. The card model, a host simulation of an SD card that `main.c` was compiled against so that host and hardware logs compare directly, is kept on another machine and has not yet been run against the filesystem layer and the coordinator, so the current demo has not run on it.

DMA is planned for bulk storage transfers after the polling implementation has been validated.

Interrupt-driven or asynchronous operation will be introduced where required by later subsystems, but a complete system-wide execution and scheduling model has not yet been defined beyond the following rule: each core runs one cooperative loop of bounded-work steps, and no operating mode places more than one hard-deadline domain on a core (see "Operating Modes and Storage Ownership"). Audio is the only current domain with a hard deadline; its decoder is written as a bounded step reading compressed data from a ring the storage coordinator fills, so whether it runs in the core 0 loop or on the second core is a launch-time placement decision sized by measurement (chiefly the display driver's blocking time), not a structural one. The no-RTOS decision is to be re-evaluated if a mode ever needs two hard-deadline domains on one core, or if long operations start acquiring hand-written yield points.

**Stacks.** Core 0 runs on a 4 KB stack (`PICO_STACK_SIZE` in `CMakeLists.txt`), the most the SDK's default memory map allows: its linker script places core 0's stack in the 4 KB SCRATCH_Y bank and core 1's in SCRATCH_X. Everything in the storage stack runs on it, including FatFs, which with `FF_USE_LFN` 2 puts a 512-byte long-file-name buffer on the stack in every call that takes a path. GCC's call-graph output (`-fcallgraph-info=su`) for the Release build puts the deepest storage path at about 1.7 KB: the demo's save-point read, through `f_open` (a 624-byte frame) and FatFs's directory code, down to the SD driver. Formatting is shallower, about 0.8 KB, despite `f_mkfs`'s 512-byte work buffer, and a log line costs about 0.8 KB wherever it is made. Interrupt frames come on top, so the margin is roughly half the stack. These are static figures for one build: a less optimised debug build has larger frames, and the first run-time measurement is planned in VAL-H04*. A larger stack, and the stack a core 1 audio decoder is likely to need, means a custom linker script that places the stacks in main SRAM.

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

Current development uses an Adafruit MicroSD card breakout board+ on a breadboard, accessed through SPI0: DO on GPIO 16, CS on GPIO 17, CLK on GPIO 18 and DI on GPIO 19, with the availability signal on GPIO 12 (`main.c` holds the authoritative assignment). The first hardware run, VAL-H01, used an earlier wiring on SPI1; the breadboard was later rewired onto SPI0 to wire up the availability signal. That breakout carries no pull-ups on any SD line, its 74AHC125 level shifter sits on DI, CLK and CS, and its socket's detect switch is active-high. Until the final board's discrete pull-ups exist, `main.c` enables the RP2350 internal pull-ups on DO and CS: the card leaves DO undriven until CMD0 moves it into SPI mode, and without a pull-up the driver's pre-command ready wait sees a floating line and reports BUSY_TIMEOUT before CMD0 is ever sent.

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

Ownership of the storage during this access is defined under "Operating Modes and Storage Ownership"; how USB mode is selected remains open.

## Known Architectural Limitations

The current firmware architecture is incomplete because development is still concentrated on storage bring-up.

At the current stage:

- only the SPI SD transport exists; it is functionally complete against the block-device contract and has three real-card runs at 1 MHz behind it, all on one SDHC card (VAL-H01 to VAL-H03)

- the planned 4-bit SD backend does not yet exist

- the filesystem layer and the storage coordinator have run on the host against a RAM-backed block device and on one real card at 1 MHz (VAL-H02, VAL-H03), but not yet against the card model (kept outside the repository); removal during a multi-block read and during mount are untested, and neither the redesigned save points nor the format path has run on hardware

- DMA-based storage transfer has not yet been implemented

- the USB mass-storage class has not yet been implemented; the storage coordinator's handover to it has

- the final physical input hardware has not been selected

- EPUB parsing and rendering architecture has not yet been defined

- display buffering and update scheduling have not yet been defined

- MP3 decoding and audio-buffer scheduling have not yet been defined

- the overall asynchronous/concurrency model beyond the current blocking storage implementation has not yet been defined

- system-wide power management is designed only in outline (see "Power management" under Planned / Future Architecture): the storage coordinator can release the card for a power-down, but there is no power manager, no rail switch and no safe-point hook for consumers yet

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

FatFs runs above the block-device interface, behind the filesystem layer's interface.

The filesystem layer, the adapter between FatFs and block_device_t, and the storage coordinator above them are implemented and have run on hardware (VAL-H02, VAL-H03); a run on the card model, kept outside the repository, remains.

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

The ownership model between the USB host and Tavernkeep's own filesystem access is defined under "Operating Modes and Storage Ownership"; the mass-storage class itself remains to be designed.

**Power management**

Power management will eventually coordinate processor activity, peripheral power state, display behavior, audio hardware, removable storage, and system shutdown.

*The peripheral rail.* Every peripheral that can be powered down - the SD socket, and the other parts that need it - runs from one rail switched by a MOSFET under GPIO control. Cutting the rail removes power from everything but the microcontroller, which gives a sleep state deeper than leaving the peripherals at their idle draw, yet one the firmware wakes from with its RAM and state intact rather than through a reboot. The rail belongs to the power manager, not to any one driver: a driver cannot cut a rail other peripherals depend on, so the SD driver never touches it, and it copes with the card having lost power between a deinit and the next init because init always starts from CMD0, as for a card just inserted. One consequence is that a single peripheral cannot be power-cycled on its own to recover it; doing that means cycling the whole rail.

*Going to sleep:*

1. ask each owner of a peripheral on the rail to release it, and wait, with a timeout, for all of them. For storage that is `storage_request_owner(STORAGE_OWNER_NONE)`, done once `storage_owner()` is the none owner and `storage_request_pending()` is false; a faulty medium then means the card stayed busy, and cutting the rail could interrupt its programming
2. put every pin facing the rail into a state that cannot feed current into unpowered parts through their protection diodes: nothing driven high, and no microcontroller pull-up to 3.3 V on those lines. That covers the SD lines and the display and audio buses alike
3. turn the rail off, then put the microcontroller to sleep

*Waking:*

1. turn the rail on and wait for it to settle (the load switch's or MOSFET circuit's turn-on time)
2. restore the pins
3. ask for the owners back - for storage, the local owner, or USB if a host is attached. The rail must be on before this request: the coordinator then treats the card as just inserted, and its insertion settle wait (`STORAGE_INSERT_SETTLE_MS`) comfortably covers the card's power-up time before the first command

Waking therefore takes the settle wait plus the card's own initialization before storage is ready, roughly 0.6 to 1 s; turning the rail on early in the wake sequence, while the displays wake, hides some of it.

*Board design notes,* to carry into the first schematic:

- pull-ups on the SD lines (DO, CS, and the data lines) belong on the switched rail, not on the microcontroller's 3.3 V, so that they cannot back-power an unpowered card. The internal pull-ups `main.c` enables on DO and CS are breadboard stand-ins and go away with them. The card-detect switch is the exception: it has to be readable with the rail off, so its pull-up stays on the microcontroller side
- switching the rail on charges the bulk capacitance of everything on it at once. A bare MOSFET can sag the shared 3.3 V enough to brown out the RP2350 on wake; a load switch with a controlled turn-on time, or a slowed gate drive, avoids it
- an SD card needs its supply to fall close to zero, and stay there briefly, before it is powered again, or it may not reset cleanly. Sleep satisfies that naturally, but the power manager needs a minimum off time for any quick off-and-on, and the rail must actually discharge when switched off, which may need a weak pull-down on it
- the development socket's detect switch closes to ground when the socket is empty, so an empty socket draws about 66 µA through the card-detect pull-up for as long as the device sleeps. Whether insertion should wake the device is still open; if it should not, either the driver switches that pull-up off during sleep (a small addition that partly reverses the rule that the detect pin is claimed for the life of the device), or the board uses a weaker external pull-up or a socket whose switch is open when empty

*Built so far:* the storage coordinator's release and reclaim (the none owner, in `storage.h`), tested on the host. Not yet built: the power manager itself, the rail switch, the pin safe-state for each peripheral, a forced release for a card that never stops being busy (the "separate forced-shutdown path" under Resource Ownership), and the consumer safe-point hook in `storage_step`.
