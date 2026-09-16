# Requirements

This document is the maintained source for Tavernkeep requirements. Keep each
requirement concise, unambiguous, and testable where practical. Use stable,
simple identifiers: `FR-NNN` for functional requirements, `NFR-NNN` for
non-functional requirements, and `CON-NNN` for constraints. Do not reuse an
identifier after removing a requirement.

## Purpose / Scope

Tavernkeep is the firmware for Tome, an embedded dual-display ereader and mp3 player. Tavernkeep is responsible for managing the device's storage, EPUB reading functionality, audio playback, display output, physical controls, USB functionality, and system-level device operation.

## Functional Requirements
<!--
Record observable behaviors. Prefer "Tavernkeep shall ..." statements with
explicit inputs, conditions, and outcomes where those details are known.
-->

- **FR-001 — EPUB Reading:** Tavernkeep shall allow the user to discover, open, read, and navigate EPUB files stored on the device.

- **FR-002 — Plain-Text Reading:** Tavernkeep shall allow the user to discover, open, read, and navigate plain-text (`.txt`) files stored on the device.

- **FR-003 — Reading State:** Tavernkeep shall persist sufficient reading state for supported books to allow the user to resume reading after closing a book or restarting the device.

- **FR-004 — Bookmarks:** Tavernkeep shall allow the user to create and remove bookmarks within supported electronic books and retain those bookmarks across restarts.

- **FR-005 — Music Playback:** Tavernkeep shall allow the user to discover and play MP3 audio files stored on the device.

- **FR-006 — Audio Controls:** Tavernkeep shall provide controls for playback, pause, track navigation, seeking, and volume adjustment during music playback.

- **FR-007 — Dual-Display Output:** Tavernkeep shall operate both of Tome's displays and use them as part of the device's reading and user-interface experience.

- **FR-008 — Physical User Input:** Tavernkeep shall accept physical user input sufficient to navigate books, music playback, menus, and other device functions.

- **FR-009 — Removable Storage:** Tavernkeep shall read content from and write persistent device data to removable storage.

- **FR-010 — USB Mass Storage:** Tavernkeep shall allow a connected USB host to access the device's removable storage for transferring and managing books, audio, and other supported files. USB mass-storage access is an exclusive operating mode: while a host has the storage, reading and playback are unavailable, and the device may otherwise be used while charging from the same connection.

- **FR-011 — Device Navigation:** Tavernkeep shall provide an interface through which the user can browse available books and audio content and select content to open or play.

- **FR-012 — System Operation:** Tavernkeep shall manage initialization, normal operation, shutdown, and recoverable error conditions required for Tome to operate as a standalone device.

## Non-Functional Requirements
<!--
Record qualities or performance expectations such as timing, reliability,
resource use, maintainability, safety, or usability. Include measurable limits
and operating conditions when they become known.
-->

- **NFR-001 — Responsiveness:** Tavernkeep shall respond to physical user input promptly enough that interaction feels immediate during normal operation, independent of the slower refresh characteristics of the e-ink displays.

- **NFR-002 — Audio Continuity:** Tavernkeep shall maintain continuous audio playback during normal operation without user-perceptible interruptions caused by routine storage access, user input, or display activity.

- **NFR-003 — Data Integrity:** Tavernkeep shall minimize the risk of corruption to user content and persistent device state during normal operation, recoverable errors, shutdown, and storage access.

- **NFR-004 — Error Isolation:** Errors involving an individual peripheral, file, or unsupported/malformed piece of content shall not unnecessarily prevent continued operation of unrelated device functionality.

- **NFR-005 — Resource Constraints:** Tavernkeep shall operate within the processing, memory, and storage resources available on Tome's embedded hardware without requiring external application memory or network-hosted services.

- **NFR-006 — Power Efficiency:** Tavernkeep shall allow unused hardware and firmware subsystems to enter reduced-power or inactive states where practical, and shall avoid unnecessary activity that materially reduces battery life.

- **NFR-007 — Offline Operation:** Tavernkeep shall provide all required device functionality without reliance on network connectivity, cloud services, or remote computation.

- **NFR-008 — Recoverability:** Tavernkeep shall recover gracefully from expected transient failures where practical, including removable-storage errors and peripheral communication failures, without requiring unnecessary full-device resets.

## Constraints
<!--
Record externally imposed limits that restrict the solution space, such as
hardware, interfaces, standards, dependencies, power budgets, or toolchains.
-->
- **CON-001 — Dual Displays:** Tome shall use two e-ink displays. Tavernkeep shall be designed around this dual-display hardware configuration.

- **OPEN-001 — Physical Control Scheme:** The final physical control scheme has not yet been selected. It shall be resolved before hardware and firmware development progresses far enough for the choice to impose significant redesign. The selected controls shall support comfortable navigation of reading, music playback, menus, and system functions.

## Out of Scope
<!--
List capabilities or concerns intentionally excluded from the current project
scope. State boundaries clearly enough to prevent accidental scope expansion.
-->

## Open / Unresolved Requirements
<!--
Track requirement questions that need investigation or a stakeholder decision.
For each item, record the question, why it matters, and what would resolve it.
Move resolved items into the appropriate section rather than keeping a history
here; Git retains the chronological record.
-->

- **OPEN-002 — USB Mode Selection:** How the device enters USB mass-storage mode has not been decided: automatically whenever a USB host enumerates, by a prompt on enumeration, or by an explicit menu action. A charger-only supply does not enumerate, so the choice only affects behaviour when a host is present. It matters because it decides whether reading or playback can be interrupted by connecting a cable, which in turn decides whether the reader and audio modes must be able to survive an externally triggered storage handover (they can, through the same path as card removal, but the user-visible behaviour differs). Resolved by a product decision recorded against FR-010.

- **STRETCH-001 — Audiobook Support:** Tavernkeep may support M4B audiobook files as first-class book content, accessible through the book-oriented interface rather than requiring the user to enter the music-player interface.

