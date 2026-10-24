# Architecture

This is the living description of Tavernkeep's current firmware architecture.
Update it as the design changes, and preserve concise rationale for choices
that materially affect implementation or future work.

## System Overview

Tavernkeep is firmware for the RP2350-based Tome e-reader and MP3 player.

The firmware is divided into platform-level hardware support and higher-level subsystems responsible for storage, filesystems, book handling, audio playback, display output, physical input, USB functionality, and system operation.

Development is currently proceeding from the lowest-level hardware interfaces upward. Removable storage is the first substantial subsystem: the SPI SD driver, the filesystem layer and the storage coordinator are implemented and validated on hardware. USB mass storage over it is implemented and validated too, entered on request until automatic entry is added. EPUB, display, audio, input, and power-management functionality remains planned or only scaffolded unless otherwise documented.

Tome's final hardware uses three displays - two e-ink main panels inside and a memory LCD outside - and removable SD storage. The exact physical control scheme remains unresolved and must be settled before significant hardware development proceeds.

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

FatFs is vendored, configured and built as this layer's implementation. The layer and its FatFs disk I/O adapter are implemented (see "Filesystem" under Major Interfaces) and validated on a real card (VAL-H02 to VAL-H07 in `docs/validation.md`).

The filesystem layer should not depend on whether the underlying SD implementation uses SPI or the planned 4-bit interface.

**EPUB/Book**

The book subsystem will be responsible for opening and navigating supported book content.

EPUB and plain-text files are required book formats.

The container and package layer - zip, DEFLATE, the OCF container, the OPF package and spine, navigation, and EPUB CFI parsing and resolution - is Bartap, a first-party library in `lib/bartap/` with draft interfaces and no implementation yet (see "Develop the EPUB container layer as a portable library, Bartap" under Important Design Decisions and Rationale). Whether it also turns XHTML into a styled text stream is open (Q17 in `lib/bartap/DESIGN-QUESTIONS.md`). The firmware supplies what Bartap needs from the platform, such as a byte source over a file on the card (`src/epub/bartap_adapter.c`).

Text layout and rendering stay in the firmware and are not yet defined. The only derived data written to the card is the library catalogue, a disposable index; pagination is kept in RAM, and books are cached in PSRAM on Rev F (see "Book state on the card, device state on the device" under Important Design Decisions and Rationale).

M4B audiobook support is a stretch feature and therefore does not currently form part of the required architecture.

**Audio**

The audio subsystem will be responsible for MP3 playback and audio output.

The final hardware is planned around a PCM5102A DAC and PAM8908 headphone amplifier.

The detailed software architecture for decoding, buffering, scheduling, and audio output has not yet been defined.

**Display**

The display subsystem will control Tome's three displays: the two inner e-ink panels, which are the main displays, and the external memory LCD, a secondary display for status such as the current song or book and progress bars.

The display configuration is fixed and defining hardware for Tome.

The detailed display-driver, framebuffer, update scheduling, and rendering architecture has not yet been defined.

**Input**

The input subsystem will translate Tome's physical controls into firmware input events or equivalent higher-level actions.

The final physical control scheme has not yet been selected. Two rotary encoders with push input have been explored, but the control mechanism remains an open design decision.

**USB**

USB mass-storage access to Tome's removable storage is a required feature.

USB mass storage is an exclusive operating mode: while a USB host has the storage, no local reading or playback runs, and vice versa. Coordination with local filesystem access is therefore a mode transition, not concurrent sharing; see "Operating Modes and Storage Ownership". The class is TinyUSB's mass-storage device, from the Pico SDK, over the bare block device; when the device enters and leaves USB mode is set out under "USB Mode Entry and Exit". Both the class and the policy that decides when to lend it the card are implemented (see "USB mass-storage disk" and "USB-mode policy" under Major Interfaces); entry is by request until automatic entry is added.

**Power / System Operation**

Tavernkeep will ultimately manage hardware initialization, shutdown, peripheral power state, and other system-level behavior.

Peripheral power is to be switched as one rail under GPIO control, for the two sleep states, soft and deep (see "Power management" under Planned / Future Architecture). The power manager that drives it has not yet been implemented; the storage coordinator can already release the card for it.

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
- `filesystem_mount` takes an already-initialized block device from the storage coordinator and mounts immediately, so an absent card or a card without a FAT volume is reported at mount. `filesystem_unmount` performs no I/O, flushes nothing and always succeeds. The firmware never formats a card: one with no FAT volume (an SDXC card arrives as exFAT) is reported, and the user formats it as FAT32 on a computer.
- The layer is cooperative and single-threaded: it is called only from the core running the storage coordinator, never from an interrupt handler, and has no locking.
- Operations that would corrupt the volume because an entry is open are refused with `FILESYSTEM_RESULT_IN_USE`: removing or renaming an open file, removing a directory being listed, opening a file for replacement while another handle has it open, and opening a file that is open for replacement. Several handles may still read one file, and a directory being listed may be renamed, since the listing follows its clusters rather than its name. FatFs enforces the rules for files with its table of open entries (`FF_FS_LOCK` 5, one entry per file and directory the pools can hold, checked at compile time; about 80 bytes and a short scan on each open, remove and rename). That table does not cover a directory being listed - FatFs records a listing under the directory's own cluster but looks a directory up for removal by its entry in the parent - so `filesystem_remove` checks the open listing itself. These checks replaced an earlier rule that left open entries to the callers, which turned any mistake into silent corruption rather than an error.
- A close whose flush fails is retried once, so a transient error does not leave the file's new clusters allocated with no entry pointing at them. The layer releases a file's entry even when its close fails, and an unmount empties the table, so a removal cannot leave entries behind.

**Storage coordinator**

`src/storage/storage.h` is the owner of the removable card. It holds the block device for the life of the firmware and is the only code that initializes or deinitializes it, mounts or unmounts the filesystem, or lends the device to the USB mass-storage class. It runs as `storage_step(now_ms)` in the cooperative loop of the core that also calls the filesystem layer.

Its state is two values. The *owner* is who may use the card: the filesystem layer (local), the USB mass-storage class, or nobody (none: the card released so that the peripheral rail can be powered down; see "Power management"). The *medium* is the card's condition for that owner: absent, preparing (being brought up, including the settle wait after insertion and the waits between attempts), faulty, unformatted (local only: works, but holds no FAT volume; left alone until removed, for the user to format on a computer), or ready (local: mounted; USB: initialized and lent).

- **Removal** is noticed in the step after the card-detect interrupt latches it: `get_info` then reports `INVALID_DEVICE` without touching the bus. The response is the ownership transition: unmount (no I/O), deinitialize (on a latched device, the driver skips the bus), medium absent. **Insertion** is noticed by polling `is_present` while the device is down. An idle step costs one of these two checks and nothing else.
- **Bring-up** waits first: a new card must read present for `STORAGE_INSERT_SETTLE_MS` before its first attempt, so that it has seated and powered up (see "Availability signal and debounce"). It is then one attempt per step: init, then for the local owner mount. A card the socket reports but the debounced check refuses is still being inserted; it is not counted, and settles again before the next attempt. Any other failure is counted, and after a fixed number of attempts the card is marked faulty, released, and left alone until it leaves the socket. A mount that fails is a removal if the device has stopped answering, whatever the mount reported: a card pulled before the adapter's `disk_initialize` shows as medium unavailable, one pulled during the mount's sector reads as a storage error. If the device still answers, the failure is counted, including a medium-unavailable result, which then means a card the adapter refuses (sector size, capacity, read-only); treated as a removal, that card would be brought up again forever.
- **Requests** - an owner change - are recorded and carried out at the top of a later step, where no consumer is part-way through a call. Callers watch the owner, medium and `storage_request_pending` for the outcome.
- **The epoch** is a counter that changes whenever something a consumer may have cached stops being valid: whenever the medium stops being ready for its owner, and on every owner change. Consumers record it with what they cache and discard the cache when it differs; it is readable from either core. Stale filesystem handles enforce the same thing as a backstop.
- **Release** is the change to the none owner. It unmounts with no I/O and deinitializes the card cleanly: unlike the removal path, the card is present, so the driver waits out any programming busy, and once the deinit succeeds the card can lose power safely. A card being settled or brought up is dropped. While released, bring-up stops and the socket is not watched, so an idle step costs nothing. A card that stays busy is retried at the retry interval, `STORAGE_RELEASE_ATTEMPTS` times, and the request stays pending until it succeeds; after the last attempt the medium is reported faulty with the card still held, the request stops being pending, and whether to cut the rail anyway is the power manager's decision. A removal still releases a held card through the latch. Leaving the none owner treats whatever is in the socket as a card just inserted, settle wait included, because the rail was probably off and the card may have been swapped.

The coordinator is built for the consumers it does not have yet. A consumer that must reach a safe point before a transition - persistent state written before a USB handover, the audio stream stopped - adds that check where requests are carried out, and the request simply stays pending until it passes; the interface does not change. Bounded service work for consumers, such as the audio stream's read-ahead into the decoder's ring, has a marked place at the end of the step, after transitions, so it never runs against a card that is being lost or handed over. The USB mass-storage disk takes the device from `storage_usb_device` and compares epochs to report a medium change to the host (see below).

**Save points**

`src/storage/save_point.h` is how persistent state - bookmarks and, for now, settings - is written and read back. Settings are to move off the card to the device's own non-volatile storage, and the save points split accordingly (see "Book state on the card, device state on the device"). It sits on top of the filesystem layer and runs where that layer runs. A record is small (at most `SAVE_POINT_DATA_MAX` bytes) and is always saved whole.

- Each record lives in two slot files, the record's name with `.a` and `.b` appended. Each slot holds a magic number, a sequence number, the data length, the data and a CRC-16 over all of it, the same CRC-16 the SD driver uses for its data blocks (`sd_crc.h`). The exact format is in `save_point.h`. Its fields are little-endian, and the code writes and reads them by copying them as they are in memory: the firmware targets only the RP2350, whose Cortex-M33 cores are little-endian in every variant, so the byte order is assumed rather than checked or converted.
- A save checks both slots, then rewrites the one that does not hold the newest valid record, with the next sequence number. Its result is OK once the slot file's close succeeds, which is when the record is confirmed on the card.
- A read checks both slots and returns the valid one with the higher sequence number. A slot that is missing, short, the wrong length or fails its CRC holds no record; it is not an error. A slot the card fails to read is read once more, so a passing error changes nothing; one that fails again, while the other can be read, is not an error either: the read returns the other's record, and the next save rewrites the unreadable slot, which repairs it. Sequence numbers are compared across a wrap.
- An interrupted save can damage only the slot it was rewriting, which then fails its checks, so a read still finds the previous record - or, if the slot holding that record could not be read when the save began, the newest record the card could still read. That holds for the very first save too: it leaves no valid record, and the read reports that there is none, rather than returning a partial one.
- Nothing is renamed or removed. FatFs writes a renamed entry's new name before removing the old one, so an interrupted rename can leave two names sharing one cluster chain, and removing either then frees the other's data (`filesystem_rename` in `filesystem.h`). An earlier temporary-file-and-rename scheme was replaced for that reason, and because an interrupted first save left only an incomplete temporary that its recovery rule would have promoted.

**USB mass-storage disk**

`src/usb/msc_disk.h` is the disk a USB host sees: SCSI semantics over the block device the coordinator lends to the USB owner. It holds no device and changes no owner; each command asks `storage_usb_device()` for the device afresh. `src/usb/usb_device.c` is the TinyUSB side - the descriptors and the mass-storage callbacks, each handing straight on to `msc_disk` - so that `msc_disk` builds without TinyUSB and runs in the host tests over the real coordinator.

- **No medium** (NOT READY, MEDIUM NOT PRESENT) whenever `storage_usb_device()` is NULL: before a handover, with no card or a faulty one, while a card is brought up, after the card has gone back to the local owner. A card that a transfer finds removed counts as no medium at once, for every command, without waiting for the coordinator's next step to notice the removal.
- **UNIT ATTENTION** (NOT READY TO READY CHANGE) on the first command that finds a medium the host has not been told about, which the host then retries. The disk records the epoch of the medium it presented, so the first lend, a reinsertion during USB mode and a new handover are each announced, even if the host sent nothing while the card was away.
- **Transfers** arrive one TinyUSB buffer at a time (`CFG_TUD_MSC_EP_BUFSIZE`, 4 KiB, eight blocks), each one multi-block read or write. Writes are synced (`block_device_sync`) before they complete, so the host is told data is written only when it is on the card, and SYNCHRONIZE CACHE succeeds with nothing to do. Failures map to SCSI sense: a removal to no medium, a medium failure or busy timeout to UNRECOVERED READ ERROR or WRITE ERROR, an address outside the medium to LBA OUT OF RANGE (refused before the device is asked), a fault of the firmware's own to INTERNAL TARGET FAILURE. TinyUSB 0.18 overwrites the sense of every failed READ (10) or WRITE (10) with MEDIUM NOT PRESENT after the callback returns, so `usb_device.c` keeps the real sense and writes it into the answer to the host's next REQUEST SENSE.
- **Capacity** is offered through READ CAPACITY (10), which carries the last block in 32 bits: a 2 TB card, 2^32 blocks, is offered one block short.
- **Eject** (START STOP UNIT with LoEj set and Start clear) withholds the medium until the epoch moves on, and is recorded for the USB-mode policy (`msc_disk_take_eject`), whose job it is to return the card to the local owner. A load is accepted and changes nothing; PREVENT ALLOW MEDIUM REMOVAL likewise, since the socket has no lock.

The device descriptor uses TinyUSB's example vendor and product IDs, which are for development only; a device given to anyone else needs IDs of its own. The serial number is the flash chip's unique ID.

**USB-mode policy**

`src/usb/usb_mode.h` decides when the card is lent to USB and when it comes back, as set out under "USB Mode Entry and Exit", and asks the coordinator through `storage_request_owner`; it holds no device and does no I/O. Its step takes one sample of the VBUS sense pin (`board_vbus_present`) and whether a host has the device configured (`tud_mounted`), and returns whether the device should be on the bus, which `usb_device_attach` applies.

- **Entry** is by request: `usb_mode_request`, written by the GDB harness, asks for it, and the card is lent once a host has the device configured. Automatic entry on configuration waits for the consumer safe-point hook (see the work list under "USB mass storage" in Planned / Future Architecture).
- **The card comes back** on an eject (`msc_disk_take_eject`), on the cable's removal, when the host stops having the device configured (a bus reset, or unconfiguring it), or on request. A host that suspends the bus keeps the card.
- **VBUS** is believed once it has read the same for `USB_MODE_VBUS_DEBOUNCE_MS` (50 ms). The device is on the bus while the cable is there. TinyUSB forces the controller's own VBUS detection on, so without the pin the device could not tell a removed cable from an idle host; on a board without VBUS sensing the cable is taken to be always there.
- It logs each decision (`usb: ...`): a host configuring the device or leaving, the cable, an entry asked for, the card lent, and the card returned with the reason. The hardware tests key on these lines.

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
→ display output

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

Audio's read-ahead has priority on the card (see "Reading and audio are separate modes, with concurrency kept possible" under Important Design Decisions and Rationale); detailed buffering and concurrency behavior remain to be designed.

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

Reading and playback are separate modes, switched by a toggle in the user interface, and do not run concurrently. Reading while audio plays is not required, but the design keeps it possible (see "Reading and audio are separate modes, with concurrency kept possible" under Important Design Decisions and Rationale). (Audiobook read-along, if ever implemented, would be designed as a fourth mode.) USB mass storage runs with every other function stopped: a plugged-in cable means either a host is using the storage or the device is in use while charging, never both. The consequence for the rest of the system is that **no mode ever has more than one hard-deadline domain**, which is what lets the cooperative execution model stand without an RTOS (see Concurrency).

**Storage has one owner at a time.** The owner is the filesystem layer in reader and audio modes and the mass-storage class in USB mode. Higher layers obtain storage only through the filesystem. The `block_device_t` is held by the storage coordinator, which initializes and deinitializes it and alone decides who may use it; by the filesystem adapter while a volume is mounted; and, in USB mode, by the mass-storage glue. Nothing else holds it. This is what makes ownership enforceable rather than conventional.

**Every change of ownership is the same transition.** A card removal, entering USB mode, leaving USB mode, releasing the card before the peripheral rail is powered down, and a future mode switch all pass through one storage coordinator path that:

1. cancels cooperative storage consumers and invalidates open handles, cached sectors and bookmarks in memory;
2. flushes nothing to media that may be absent - persistent state is written at its defined save points before the transition, never during it;
3. waits for any in-flight backend operation to unwind;
4. unmounts or hands over the block device;
5. on the way back, treats the medium as new: a fresh mount, no reuse of any cached state, because a USB host may have changed anything and a reinserted card may be a different card.

From the filesystem layer's point of view, entering USB mode is indistinguishable from a removal and leaving it is indistinguishable from a reinsertion. The filesystem layer must therefore be written so that unmount is safe against an absent card and mount assumes a changed medium - not as if it will own the card for the lifetime of the firmware.

**USB mode is write-through.** The mass-storage class maps host reads and writes directly onto block-device operations with no caching layer in between, so a host "synchronize cache" has nothing to flush. Block-device failure results map to the corresponding medium-not-present and write-error conditions. Hot removal during USB mode is handled by the same latch as everywhere else: every block operation fails, the class reports the medium gone, and the host ejects.

**Persistent-state writes happen at named save points behind one function.** Bookmarks and reading positions are written when the user explicitly bookmarks and automatically at chapter transitions; the recent-books list whenever opening a book changes its order; the library catalogue only when it is reconciled, after a mount or a USB return; nowhere else, and never from within the mode transition above. Settings belong on the device, not the card. In reader mode the chapter transition coincides with a full display refresh, so the write is issued once the frame has been sent to the panels and completes during the panels' own refresh time, which is much longer than a typical write. In audio mode the same rule bounds when a card write - with its programming-busy budget - can stall the storage coordinator, which sizes the audio buffering. Before entering USB mode the pending state is written and the write is confirmed complete before the handover.

The one function is `save_point_write` (see "Save points" under Major Interfaces). Each save rewrites whichever of a record's two slot files does not hold the newest valid copy, so an interruption at any moment - a removal, a USB handover forced by a cable, a power loss - leaves the previous record readable, and a read never returns a partly written one.

**USB mode is entered automatically** when a host configures the device (`requirements.md`, FR-010), so a cable can interrupt reader or audio mode. That interruption is one more caller of the ownership transition above; how the device tells a host from a charger, and when it hands the card over and takes it back, is the next section.

## USB Mode Entry and Exit

Plugging in a cable does not by itself mean a host wants the storage: a wall charger, a cable without data lines, a powered-off computer and a locked phone all supply VBUS and never talk. The device therefore keeps the card where it is when VBUS appears, and hands it to the mass-storage class only once a host has configured the device. A charger never reaches that point, so it never interrupts reading or playback.

**The policy is separate from the handover.** A small USB-mode policy, run as a bounded step in the cooperative loop, watches VBUS and the USB stack and decides when to ask for an owner. The asking is always `storage_request_owner`, the same call every other ownership change uses; the policy holds no block device and does no I/O. That separation is what lets the test build swap the trigger (below) without touching the handover.

**Entering:**

1. VBUS rises (seen on the VBUS sense pin, debounced). The policy connects the device to the bus (`tud_connect`); the card stays with its current owner.
2. A host shows itself by resetting the bus and sending its first request (`tud_connected`). If none does within a bounded time, the connection is taken to be a charger: the policy disconnects from the bus and the device carries on charging, with the USB controller idle until VBUS next rises. The time is a named constant to be set by measurement; a host normally starts within a few hundred milliseconds of attach.
3. The host configures the device (`tud_mount_cb`). The policy asks the active mode to reach a save point - the consumer safe-point hook, which the power manager needs too (see "Power management") - and then requests `STORAGE_OWNER_USB`. Configuration rather than the first request is the trigger because a host that resets the bus but never configures (a charging port with data lines, some hubs) does not want the storage either.
4. Until `storage_usb_device()` returns the block device (the USB owner, with the medium READY), the class answers the host as an empty card reader does: TEST UNIT READY fails with NOT READY, MEDIUM NOT PRESENT (sense 02h/3Ah/00h). Once it has the device, the next command gets UNIT ATTENTION, NOT READY TO READY CHANGE (06h/28h/00h), and the host mounts the volume. The class records `storage_epoch()` with the device and treats any change in it as the medium having changed, so a removal and reinsertion in USB mode reaches the host as a new medium rather than as the old one with its caches still valid.

The host can therefore enumerate the device before the card is released; it sees a reader with no card, not an error.

**Leaving:**

- **The host ejects** (START STOP UNIT with LoEj set and Start clear): the policy requests `STORAGE_OWNER_LOCAL`, and the class reports MEDIUM NOT PRESENT again. The device stays on the bus, so it keeps charging and can be used in the meantime (FR-010). It does not re-enter USB mode until the cable is next connected.
- **The cable is removed** (VBUS falls): the policy disconnects from the bus and requests `STORAGE_OWNER_LOCAL`. The coordinator mounts the card as a new medium, as step 5 of the ownership transition requires, since the host may have changed anything and may not have finished writing.
- **The host stops having the device configured** (it reset the bus or unconfigured the device, with the cable still in): the policy requests `STORAGE_OWNER_LOCAL` and the device stays on the bus.
- **The host suspends the bus** (a computer going to sleep with VBUS kept up): the device stays in USB mode. Taking the card back while the host may still hold writes in its cache is how volumes are corrupted; the host resumes or the cable is removed.
- **The card is removed in USB mode:** the removal latch fails every block operation, and the class reports MEDIUM NOT PRESENT, as under "Operating Modes and Storage Ownership". A card inserted while USB mode lasts is brought up for the USB owner.

PREVENT ALLOW MEDIUM REMOVAL is accepted and ignored: the socket has no lock, and a removal is handled the same way whether or not the host asked to prevent it.

**USB mode is entered by hand for now.** Until the safe-point hook and the reader and audio modes exist, there is no automatic entry: the GDB harness writes `usb_mode_request` (`usb_mode.h`), and the policy's next step carries it out as step 3 will, through `storage_request_owner`, once a host has the device configured. Leaving is requested the same way, or by ejecting on the host. No RTT input is read by the firmware. The Pico 2 is powered from a computer's USB port for these runs, so a host is always present and the device is always configured; without the request it remains a reader with no card. That also means a cable removal cannot be tried on that bench: pulling the cable is a power cut. Trying it needs the Pico 2 powered through VSYS as well.

**USB stdio is unavailable in a build with the mass-storage class.** TinyUSB's device stack has one owner, and its descriptors are the mass-storage class's; stdio stays on RTT (`TAVERNKEEP_STDIO_RTT`) or UART, and `TAVERNKEEP_STDIO_USB` is refused alongside it.

**Throughput** in USB mode is set by the SD transport, not by USB: full-speed USB carries roughly 1 MB/s of mass-storage traffic, and the SPI backend writes about 0.5 MB/s at 25 MHz (see `validation.md`, Measurements). The 4-bit backend is what would make USB the limit.

## Removable-Media Detection and Hot Removal

**Availability signal and debounce**

The final socket presents card-detect and write-protect information through one availability signal. The sense of that signal is a property of the socket, not of the firmware: a switch that closes to ground when a card is present gives an active-low signal (low means usable media may be present), and one that closes to ground when the socket is empty gives an active-high one. The SPI backend takes the sense as configuration (`card_detect_active_high`), defaulting to active-low; the current development breakout is active-high. In either sense the pad is pulled up, so the switch's open state reads high and only the meaning of each level and the direction of the removal edge change. Whatever the physical cause - card removal or write protection - the "absent" level means the storage device must be treated as unavailable.

The SPI backend debounces its preliminary initialization check before touching the SD bus. It requires ten consecutive samples at the present level, taken one millisecond apart, within a maximum of thirty samples. The pad's internal pull-up is selected before GPIO initialization enables the input buffer. The board-level external pull-up remains required to define the signal before firmware begins executing and while the RP2350 pad is still in its reset state. After the interrupt is armed the level is read once more, so a card that went away between the debounce and the arming is refused rather than left with a latch nothing will set.

That debounce rejects contact bounce; it does not wait for the card itself. The detect switch closes before the card's contacts have seated and before the card has powered up, so an initialization attempted the moment the socket reports a card fails on a good card (VAL-H02). The storage coordinator therefore waits `STORAGE_INSERT_SETTLE_MS` (500 ms) after a card appears, sampling the socket on every step and restarting the wait on any empty reading, before its first bring-up attempt. The wait applies to every new card, including one present at boot, and again to a card that init refuses as not seated; counted retries follow a settled card and do not wait again. The settle wait cannot tell a seated card from a switch that closed early: a card pushed partly in, then home after a pause, failed the three attempts 250 ms apart that the coordinator then made (VAL-H07). Bring-up now makes nine attempts over about 3.5 s (`STORAGE_BRING_UP_DELAYS_MS`), so a card that really is bad takes that long to be reported faulty. The SD driver logs which initialization step failed, which the block-device result cannot carry.

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

`src/main.c` is the demo of the highest layer that currently works, replaced as each layer lands: today it runs the USB device and the storage coordinator in the loop, logs every change of owner, medium and epoch, and each time the card becomes ready runs a filesystem demo (label, root listing, a counter read from and saved through a save point, and a 16 KiB file written and read back in single calls so that FatFs issues multi-block transfers) whose steps are reported over RTT. While a USB host is on the bus the loop does not idle between passes: each of the host's commands waits for the next call into TinyUSB's task, and a mass-storage READ (10) or WRITE (10) is carried out inside that task, one 4 KiB buffer per call.

Two test suites live in `tests/`. The host tests (`tests/filesystem/`) are unit and contract tests of the save points, the coordinator, the filesystem layer and the adapter, and of the USB mass-storage disk over the coordinator, with the real FatFs over a RAM-backed fake card that follows the SD driver's contract. They cover what hardware cannot reach precisely - a failure at every write, retry timing to the millisecond, every result path - and mutation testing checks that they catch planted bugs. The hardware tests (`tests/hardware/`) run the demo on a Pico 2 through the Debug Probe, injecting removals and errors at breakpoints, and ask a person at the bench for physical removals; they produce the runs in `docs/validation.md`. The card model, a host simulation of an SD card that `main.c` was compiled against so that host and hardware logs compare directly, is kept on another machine and has not yet been run against the filesystem layer and the coordinator, so the current demo has not run on it.

Bartap's host tests live with the library, in `lib/bartap/tests/`, so that they move with it, and build only when Bartap is built standalone. The firmware build runs Bartap's symbol check over its own Arm build of the library.

DMA is planned for bulk storage transfers after the polling implementation has been validated.

Interrupt-driven or asynchronous operation will be introduced where required by later subsystems, but a complete system-wide execution and scheduling model has not yet been defined beyond the following rule: each core runs one cooperative loop of bounded-work steps, and no operating mode places more than one hard-deadline domain on a core (see "Operating Modes and Storage Ownership"). Audio is the only current domain with a hard deadline; its decoder is written as a bounded step reading compressed data from a ring the storage coordinator fills, so whether it runs in the core 0 loop or on the second core is a launch-time placement decision sized by measurement (chiefly the display driver's blocking time), not a structural one. The no-RTOS decision is to be re-evaluated if a mode ever needs two hard-deadline domains on one core, or if long operations start acquiring hand-written yield points.

**Stacks.** Core 0 runs on a 4 KB stack (`PICO_STACK_SIZE` in `CMakeLists.txt`), the most the SDK's default memory map allows: its linker script places core 0's stack in the 4 KB SCRATCH_Y bank and core 1's in SCRATCH_X. Everything in the storage stack runs on it, including FatFs, which with `FF_USE_LFN` 2 puts a 512-byte long-file-name buffer on the stack in every call that takes a path. GCC's call-graph output (`-fcallgraph-info=su`) for the Release build puts the deepest storage path at about 1.7 KB: the demo's save-point read, through `f_open` (a 624-byte frame) and FatFs's directory code, down to the SD driver. A log line costs about 0.8 KB wherever it is made. Interrupt frames come on top, so the margin is roughly half the stack. These are static figures for one build: a less optimised debug build has larger frames, and the run-time measurement in VAL-H06 found 1628 bytes used in the Release build's demo, interrupt frames included. A larger stack, and the stack a core 1 audio decoder is likely to need, means a custom linker script that places the stacks in main SRAM.

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

- Context: Tome's final full-size SD socket uses card-detect and write-protect switch information through the same GPIO in order to conserve GPIO resources. Tome requires writable storage for the reading state it keeps with the books.

- Decision: Firmware does not distinguish between an absent card and a write-protected card. Either condition means that a usable storage device is unavailable.

- Rationale: Read-only operation is not a required Tome operating mode, so distinguishing the two states provides no required functionality.

- Alternatives considered: Allocate separate GPIO inputs for card detection and write protection.

- Consequences and tradeoffs: Tavernkeep cannot inform the user which of the two physical conditions caused the device to become unavailable.

### Decision — The displays are fixed, the physical control scheme is not

- Context: Tome's two inner e-ink panels, with an external status display, are a defining product characteristic. The originally explored dual-encoder input system may not necessarily provide the best user experience.

- Decision: Design Tavernkeep around two inner e-ink main displays and an external memory LCD while leaving the physical control interface open until it is deliberately selected.

- Rationale: Display count is fixed by the product concept, while changing the controls remains possible if another scheme materially improves interaction.

- Alternatives considered: Treating both display and control hardware as fixed.

- Consequences and tradeoffs: Input architecture should not become unnecessarily coupled to an unfinalized physical control arrangement during early development.

### Decision — Enter USB mode automatically, once a host configures the device

- Context: FR-010 requires USB mass-storage access as an exclusive mode, and the same connector charges the device. A cable may lead to a host that wants the storage, or only to a supply.

- Decision: Connect to the bus whenever VBUS is present, and hand the card to the mass-storage class when a host configures the device, with no prompt. A connection on which no host appears within a bounded time is a charger and changes nothing. Leave USB mode on eject or cable removal.

- Rationale: Copying files is the reason to connect to a computer, so asking first adds a step to every transfer. Waiting for configuration, rather than acting on VBUS, keeps chargers and data-less cables from ever interrupting reading or playback, and needs no extra hardware beyond VBUS sensing.

- Alternatives considered: A prompt on the displays when a host appears; an explicit menu action; entering on VBUS alone.

- Consequences and tradeoffs: Connecting to a computer only to charge interrupts reading or playback until the host ejects the storage. Reader and audio modes must reach a save point on an outside request at any time (the consumer safe-point hook). The device must sense VBUS.

### Decision — Develop the EPUB container layer as a portable library, Bartap

*Drafted for review.*

- Context: Opening an EPUB means reading its zip container, inflating DEFLATE-compressed entries, parsing the OCF container and the OPF package for the spine, and reading its navigation (the EPUB 3 navigation document or the EPUB 2 NCX). None of this depends on Tome's hardware.

- Decision: This layer is Bartap, a first-party library in `lib/bartap/`, built as its own CMake project and intended to move to its own repository. Its scope is zip, inflate, OCF, OPF and spine, navigation, and EPUB CFI parsing and resolution. Layout and rendering are out of scope, and whether it also produces a styled text stream from XHTML is open (Q17). Bartap's own decisions are recorded in `lib/bartap/DECISIONS.md` and its open questions in `lib/bartap/DESIGN-QUESTIONS.md`, not here. Tavernkeep supplies its platform side (`src/epub/bartap_adapter.c`) and keeps layout, rendering and caching policy. `lib/` holds first-party libraries intended to become separate repositories, distinct from the vendored code in `third_party/`, which keeps its upstream licences.

- Rationale: With no Pico SDK, FatFs or TinyUSB includes, the boundary is enforced by the build rather than by convention; every malformed-input path can be tested on a host under sanitizers and several compilers and word sizes; and the layer stays usable outside Tome.

- Alternatives considered: Keeping the layer in `src/epub/` as part of the firmware, where it began. Bartap's own alternatives are in `lib/bartap/DECISIONS.md` (D1).

- Consequences and tradeoffs: Bartap keeps rules the firmware itself does not need - no allocation, no mutable global state, caller-supplied memory and I/O, bounded work per call - and checks that enforce them, in its own tests and, for the Arm build, in the firmware build. EPUB code lives in two places, and Bartap's interfaces stay drafts until Bartap settles them.

### Decision — Open EPUB files directly on the device

*Drafted for review.*

- Context: FR-001 requires opening EPUB files stored on the device. Books reach the card by being copied, over USB mass storage (FR-010) or from a card reader.

- Decision: Tavernkeep reads EPUB files as they are on the card. No tool on a computer converts or prepares books for the device.

- Rationale: Copying a file is the whole workflow and works from any computer, with no companion software to maintain or require.

- Alternatives considered: Converting books on a computer into a form prepared for the device, such as pre-decompressed or pre-paginated.

- Consequences and tradeoffs: Everything an EPUB needs - zip, DEFLATE with its 32 KiB window, XHTML and CSS parsing, layout - runs on the device, within its memory and time, on books as they are found, malformed ones included. Apart from the library catalogue, nothing derived from a book is kept on the card, so that work is redone whenever a cache in RAM or PSRAM does not already hold its result.

### Decision — Book state on the card, device state on the device

*Drafted for review.*

- Context: The card is removable, and in USB mode a host can change anything on it: books may be added, removed or renamed on a computer. Tavernkeep already treats a medium it gets back as new and reuses no cached state ("Operating Modes and Storage Ownership").

- Decision: What belongs to a book lives on the card with the books; what belongs to the device lives on the device.
  - On the card: each book's bookmarks and reading position, as CFIs, in an independent file of its own right next to the book's `.epub`; and in one hidden folder, `/.tome/`, the library catalogue and, in a file of its own, the card's recent-books list (each book's title and author; its position is in the book's own bookmark file), kept apart from the catalogue because it is written much more often. The list holds the five most recently opened books, newest first: opening a book not in it puts it at the top and pushes the oldest out; opening one already in it moves it to the top.
  - The catalogue is an index, rebuildable from the books and disposable: if it is missing or corrupt it is rebuilt, and a catalogue problem never affects a bookmark. It is reconciled against the files on every mount and after every USB return (each new storage-coordinator epoch).
  - On the device: settings (font size, dark mode, numerals and so on), in a small non-volatile part on the board - the RTC's user EEPROM or a separate I2C EEPROM or FRAM - never in the MCU's flash. The MCU's internal flash is not written at run time.
  - Pagination is derived from both the book and the device's font settings, so it is kept in RAM only.
  - On Rev F, books are cached in PSRAM, though the reader must work without it: the current book is held inflated; other recent books are held as their compressed `.epub` files and inflated from PSRAM on switching. How many are held is set by the space available, up to a maximum, not a fixed count. The inflate window and decode tables stay in SRAM. Cached books are validated against the card before use and dropped when the card changes.

- Rationale: Reading state belongs to the book, so it travels with the book's card; settings belong to the reader, so they stay with the device whichever card is in. The catalogue can live on the card because nothing depends on it surviving: whatever a host or a swapped card does to it, a rebuild restores it. The MCU's flash is in-package, so wearing it out kills the chip, and a write stalls code execution and PSRAM access.

- Alternatives considered: Writing only bookmarks and settings to the card, with every cache in RAM (this record's earlier decision); settings on the card; settings or state in the MCU's flash.

- Consequences and tradeoffs: A user can keep several cards and swap them; each card brings its own library, positions and recent books. The card holds files the user did not put there: the bookmark file next to each book and `/.tome/`. A book renamed on a computer without its bookmark file loses its bookmarks and position, unless the catalogue's reconciliation pairs them again, which is not yet designed. Inflated chapters and pagination are rebuilt after a restart, a card change or a USB session unless PSRAM still holds them; PSRAM is not assumed (NFR-005), so caching must work, if smaller, in SRAM alone, and Bartap streams content rather than needing a whole chapter in memory. Positions and bookmarks are a place in the book's content, not a page number, so a change of font settings does not move them. The save points today keep bookmarks and settings together on the card; splitting them is open work.

### Decision — Reading and audio are separate modes, with concurrency kept possible

*Drafted for review.*

- Context: Reader and audio are separate operating modes, and audio is the only domain with a hard deadline (NFR-002). Reading while audio plays is not required.

- Decision: Reading and audio playback are separate modes, switched by a toggle in the user interface. Reading while audio plays must stay possible, and three rules keep it so: the reader works within an explicit memory budget that leaves room for audio's buffers; audio's read-ahead has priority on the card; and the audio decoder may run on core 1.

- Rationale: Separate modes keep each one simple to build and validate now. The rules are cheap to keep from the start and expensive to retrofit: a reader that has grown to fill RAM, or storage service with no priority order, would both need redesigning. A decoder on core 1 keeps audio's deadline away from the reader's and the displays' work on core 0.

- Alternatives considered: Strictly exclusive modes, letting each mode use all memory and the card freely; designing concurrent reading and playback now.

- Consequences and tradeoffs: The reader's memory budget must be set before layout and caching are designed. Audio's read-ahead runs first in the storage coordinator's service work, at the place reserved for it at the end of `storage_step`. A decoder on core 1 takes its compressed data from the ring the coordinator fills, never from the filesystem, and needs more stack than the default memory map gives core 1 (see Stacks under Concurrency / Execution Model). Running both together would still place only one hard-deadline domain on a core, so the execution model would not change.

### Decision — Built-in glyph strikes with SD-card expansion

*Drafted for review.*

- Context: Text is drawn from bitmap fonts; a strike is one typeface at one pixel size. Every glyph built into the firmware costs flash.

- Decision: The firmware carries built-in strikes covering Latin, Greek and the characters likely in English and French books, plus a replacement glyph. More strikes can be loaded from a `fonts` folder at the card's root, in the same versioned format the built-in strikes use. Glyphs are found through a chain of providers: the built-in strikes, then the card's, then the replacement glyph. Strikes on the card extend which books Tavernkeep can render; they are not for changing how text looks.

- Rationale: Built-in strikes let the device show text - menus, errors, a missing-card message - with no card at all, and cover what most books need. One versioned format for both means one loader and one set of tests, and a format change cannot make old strikes on a card misread. The chain means a missing glyph always draws something.

- Alternatives considered: Built-in strikes only; strikes only from the card; card strikes taking precedence over built-in ones, so that a user could change how text looks.

- Consequences and tradeoffs: Built-in strikes take flash. A book in a script with no strike on the card shows replacement glyphs. Because the built-in strikes come first, a card strike adds glyphs the built-in strikes lack and never replaces one of theirs. Fonts embedded in EPUBs are likely to be ignored. The strike format and how a strike is chosen are not yet defined. Strikes on the card are content the user put there, like books, not state the device writes.

## Hardware Assumptions Relevant to Firmware

**Processor**

Tome is based on the RP2350.

Firmware is developed using the Raspberry Pi Pico SDK.

Current development uses a Raspberry Pi Pico 2.

Rev F, the next board, assumes:

- the RP2354B (QFN-80), for the extra GPIOs needed by native 4-bit SD, three displays, I2C (the RTC and settings storage), the lid sensor, audio and input
- an RTC with user EEPROM, or a separate I2C EEPROM or FRAM, for settings
- 8 MB of PSRAM on QSPI chip select 1, on an always-on rail
- ADC inputs on the 80-pin package's ADC pins

**Removable storage**

The final Tome hardware uses a full-size SD card socket.

Current development uses an Adafruit MicroSD card breakout board+ on a breadboard, accessed through SPI0: DO on GPIO 16, CS on GPIO 17, CLK on GPIO 18 and DI on GPIO 19, with the availability signal on GPIO 12 (`main.c` holds the authoritative assignment). The first hardware run, VAL-H01, used an earlier wiring on SPI1; the breadboard was later rewired onto SPI0 to wire up the availability signal. That breakout carries no pull-ups on any SD line, its 74AHC125 level shifter sits on DI, CLK and CS, and its socket's detect switch is active-high. Until the final board's discrete pull-ups exist, `main.c` enables the RP2350 internal pull-ups on DO and CS: the card leaves DO undriven until CMD0 moves it into SPI mode, and without a pull-up the driver's pre-command ready wait sees a floating line and reports BUSY_TIMEOUT before CMD0 is ever sent.

The current SD implementation begins communication at approximately 400 kHz and increases SPI speed only after successful card initialization. Post-initialization rates of 1, 5, 12.5 and 25 MHz have been demonstrated on hardware (VAL-H05); the demo runs at 25 MHz unless the build overrides `SD_BAUD_HZ`. USB mode has run at 1 MHz (VAL-H08, VAL-H09) and 25 MHz (VAL-H10).

Tome operates SD cards from a 3.3 V interface.

The final socket's write-protect switching is used as one firmware-visible availability signal that also incorporates card-detect (this decision was made based on the particular socket's truth table so if a hardware change happens here this may need to be reevaluated).

An external pull-up is provided for this signal.

The SPI backend also selects the RP2350 pad's internal pull-up before enabling the GPIO input and applies bounded software debounce to its initial availability check. The external pull-up remains necessary because software cannot alter the pad state before firmware executes.

**Displays**

Tome uses three displays: two e-ink main panels inside, and a memory LCD outside for status (the current song or book, progress bars and such).

The currently selected e-ink panels are Good Display GDEH0576T81 using the SSD2677 controller.

The display configuration is a fixed architectural assumption.

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

Ownership of the storage during this access is defined under "Operating Modes and Storage Ownership", and when the device enters and leaves USB mode under "USB Mode Entry and Exit".

The firmware needs to see VBUS to know a cable is connected: TinyUSB forces the controller's own VBUS detection on, so the controller never sees a cable go. The Pico 2 brings VBUS to GPIO 24 through a divider (`PICO_VBUS_PIN` in the SDK's board header), which `board_vbus_present` reads; the final board needs the same, a divider from VBUS to a GPIO that stays readable with the peripheral rail off. Its values, 5.6 kΩ over 8.2 kΩ, and the reasons for them are in the board design notes under "Power management".

**Battery and charging**

Tome runs from a single-cell lithium-ion battery charged from USB. The charger IC has not been selected. A power-path charger is preferred, one that supplies the device from USB separately from the battery's charge current: with a charger whose load sits directly on the battery, a device in use while charging can draw more than the charger's termination current, so the charge never ends and the battery is held at full voltage. How the battery level is measured, and how charge status reaches the firmware, depend on that choice and have not been decided.

## Known Architectural Limitations

At the current stage:

- only the SPI SD transport exists; it is functionally complete against the block-device contract and has run on one SDHC card at up to 25 MHz (VAL-H01 to VAL-H07)

- the planned 4-bit SD backend does not yet exist

- the filesystem layer and the storage coordinator have run on the host against a RAM-backed block device and on one real card at up to 25 MHz (VAL-H02 to VAL-H07), but not yet against the card model (kept outside the repository); removal during a multi-block read is untested, and physical removal has run only at 1 MHz

- DMA-based storage transfer has not yet been implemented

- USB mode is entered only on request (`usb_mode_request`, from GDB): automatic entry waits for the consumer safe-point hook, and the charger timeout with it. A cable's removal has run only in the host tests

- the USB device descriptor still uses TinyUSB's example vendor and product IDs; they must be replaced with IDs of our own before any device is distributed

- the final physical input hardware has not been selected

- the EPUB container and package layer (Bartap, `lib/bartap/`) is scaffolded with draft interfaces and no implementation; text layout and rendering have not yet been defined

- the reader's memory budget, and the room it leaves for audio's buffers, have not yet been set

- display buffering and update scheduling have not yet been defined

- MP3 decoding and audio-buffer scheduling have not yet been defined

- the overall asynchronous/concurrency model beyond the current blocking storage implementation has not yet been defined

- system-wide power management is designed only in outline (see "Power management" under Planned / Future Architecture): the storage coordinator can release the card for a power-down, but there is no power manager, no rail switch and no safe-point hook for consumers yet

## Planned / Future Architecture

**4-bit SD backend**

A separate SD backend using the RP2350's PIO resources is planned for the final Tome hardware.

It is intended to provide the same block-device behavior to higher layers as the SPI backend so that filesystem and application code do not depend on the physical SD transport.

SPI support is intended to remain useful for hardware bring-up even after the 4-bit backend is introduced.

**DMA-backed storage transfer**

After the polling SD implementation has been validated, bulk block transfers are planned to use DMA.

The exact asynchronous API and DMA ownership model have not yet been designed. They must implement the cancellation, IRQ ordering, buffer invalidation, RP2350-E5 abort sequence, and resource-release rules described in Removable-Media Detection and Hot Removal.

**Input/events**

A general input/event mechanism is expected to be required as Tavernkeep gains display, audio, and physical controls, but its structure should be designed after the physical control scheme and actual subsystem needs become clearer.

**Displays**

Tavernkeep will require drivers and higher-level rendering paths for the two e-ink panels and the external memory LCD.

Framebuffer strategy, partial-update policy, rendering ownership, and display scheduling remain to be determined.

**EPUB**

Tavernkeep will require EPUB file access, parsing, layout, navigation, persistent reading position, and bookmark support.

The container and package layer (zip, DEFLATE, OCF, the OPF package and spine, navigation, and EPUB CFI parsing and resolution) is Bartap's, in `lib/bartap/`, with draft interfaces and no implementation yet; whether it also produces a styled text stream from XHTML is open (Q17). The internal architecture of the rest - file access through the Bartap adapter, layout, reading position and bookmarks - has not yet been designed.

**Audio**

Tavernkeep will require MP3 decoding and continuous delivery of audio to the hardware audio path.

Audio's read-ahead has priority on the card, and the decoder may run on core 1 (see "Reading and audio are separate modes, with concurrency kept possible" under Important Design Decisions and Rationale). Buffering, DMA use, timing, decoder selection or implementation, and interaction with display activity remain to be determined.

**USB mass storage**

USB mass-storage access to removable storage is required.

The ownership model between the USB host and Tavernkeep's own filesystem access is defined under "Operating Modes and Storage Ownership", and entry and exit under "USB Mode Entry and Exit". The work, in order:

1. done: TinyUSB in device mode with a mass-storage-only configuration, its read, write, capacity and sense callbacks over `storage_usb_device()`, write-through, with block-device results mapped to SCSI sense (see "USB mass-storage disk" under Major Interfaces; VAL-H08, VAL-H09)
2. done: the USB-mode policy with the hand trigger the hardware tests use (`usb_mode_request`, written by GDB), and VBUS sensing for cable removal (see "USB-mode policy" under Major Interfaces)
3. done but for the cable: host tests of the policy's transitions; hardware cases (the `usb` group) for entering by request, a copy from the computer and an eject, leaving by request, and a removal of the card while the host has the storage (VAL-H09). A cable removal is host-tested only, since the bench Pico 2 is powered through its cable
4. automatic entry on configuration, with the charger timeout, once the consumer safe-point hook exists

**Power management**

Power management will eventually coordinate processor activity, peripheral power state, display behavior, audio hardware, removable storage, and system shutdown.

*The peripheral rail.* Every peripheral that can be powered down - the SD socket, and the other parts that need it - runs from one rail switched by a MOSFET under GPIO control. Cutting the rail removes power from everything but the microcontroller and, on Rev F, the PSRAM on its always-on rail; both sleep states below do it. The rail belongs to the power manager, not to any one driver: a driver cannot cut a rail other peripherals depend on, so the SD driver never touches it, and it copes with the card having lost power between a deinit and the next init because init always starts from CMD0, as for a card just inserted. One consequence is that a single peripheral cannot be power-cycled on its own to recover it; doing that means cycling the whole rail.

*Sleep states.* There are two. In soft sleep everything is off except the microcontroller's always-on domain and the PSRAM, which stays powered in a data-retaining mode on an always-on rail; waking resumes the page being read. Since only the always-on domain stays powered, the cores restart on waking and the page is restored from state kept in PSRAM, not from SRAM (to be confirmed against the RP2350 datasheet's power chapter, then on hardware). In deep sleep the PSRAM is off too.

*Boot and waking.* A cold boot, or waking from deep sleep, opens the book selection menu rather than the last book, with recent books first. While the menu is shown, recent books are prefetched into PSRAM in background, bounded steps, most recent first. Selecting a book opens it at its last position.

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
- VBUS reaches a GPIO through a divider powered by VBUS itself, not by the peripheral rail, so that the firmware sees a cable whatever the rail's state. Whether a cable should wake the device is open; if it should, the pin is also a wake source, and the wake sequence then continues into "USB Mode Entry and Exit"
- the VBUS divider is 5.6 kΩ from VBUS to the pin and 8.2 kΩ from the pin to ground, a ratio of about 0.59: 3.1 V at the pin at 5.25 V, the top of the USB range, and about 2.6 V at a sagging 4.4 V, both clear highs, and never above the pin's own 3.3 V supply. The pin is a digital input with its pulls disabled (the pull-down every pad has at reset, in parallel with the 8.2 kΩ, would lower the high level) and an edge interrupt, debounced in software. A 10-100 nF capacitor from the pin to ground may be added to filter the ringing of a plug-in; its time constant, under 0.5 ms, does not slow detection
- the divider's low resistance is deliberate. Erratum RP2350-E9, on the A2 stepping, makes a pad with its input buffer enabled leak enough to hold a weakly pulled-down node near 2 V, where it reads high: with a divider of hundreds of kΩ, removing the cable would go unseen. The errata's workaround is a pull-down of 8.2 kΩ or less (to be confirmed against the errata for the stepping fitted), and the A2 stepping cannot be ruled out for Tome. The cost is about 0.36 mA, drawn from USB and only while a cable is connected. The 5.6 kΩ still limits the current into the pin's clamp diodes from a plug-in overshoot (a cable hot-plugged into ceramic input capacitors can ring to 8-10 V) to about 1 mA; the resistances should not go much lower than a few kΩ
- the VBUS net must carry nothing but VBUS. If anything on the board can drive it from the battery side - a boost converter, USB OTG, a diode-OR into the 5 V net, or a charger that feeds its own input - the divider becomes a constant drain on the battery and reports a cable that is not there. This constrains the choice of charger (see "Battery and charging" under Hardware Assumptions)
- the development socket's detect switch closes to ground when the socket is empty, so an empty socket draws about 66 µA through the card-detect pull-up for as long as the device sleeps. Whether insertion should wake the device is still open; if it should not, either the driver switches that pull-up off during sleep (a small addition that partly reverses the rule that the detect pin is claimed for the life of the device), or the board uses a weaker external pull-up or a socket whose switch is open when empty

*Built so far:* the storage coordinator's release and reclaim (the none owner, in `storage.h`), tested on the host. Not yet built: the power manager itself, the rail switch, the pin safe-state for each peripheral, a forced release for a card that never stops being busy (the "separate forced-shutdown path" under Resource Ownership), and the consumer safe-point hook in `storage_step`. Automatic USB-mode entry needs the same hook, since a cable can arrive at any moment.

*Open: caches across sleep.* Books cached in PSRAM survive soft sleep, and are validated against the card before use rather than dropped on the epoch change that releasing the card causes (see "Book state on the card, device state on the device"), so waking from soft sleep need not reopen the book. Still open: what the validation compares - the card's identity (its CID) with the book file's size and timestamp is the candidate - given that a card edited elsewhere and returned while the device slept must fail it. To be decided with the power manager and the reader's caching.
