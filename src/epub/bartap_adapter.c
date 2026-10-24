/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

/* Bartap adapter: tavernkeep's side of the Bartap EPUB library (lib/bartap/).
 *
 * Bartap does no I/O and no logging of its own; everything platform-specific it needs is supplied from
 * here. TODO: nothing is implemented yet. What belongs here:
 *
 * - A byte source over a file on the card: a bartap_byte_source_t whose read callback seeks and reads
 *   through filesystem.h. As drafted before Bartap was split out, it would take a file already opened
 *   with FILESYSTEM_OPEN_READ (and not own it: the caller closes it after it has finished with the
 *   source), read the file's size once with filesystem_size when it is made, and report every
 *   filesystem failure (STORAGE_ERROR, STALE_HANDLE after a removal or USB mode) as
 *   BARTAP_BYTE_SOURCE_RESULT_IO_ERROR, keeping the last filesystem_result_t for the log. That detail
 *   stays here, in the source's context: Bartap reports codes only (D14, D18 in
 *   lib/bartap/DECISIONS.md).
 *   Execution model: the filesystem layer's - the storage coordinator's core, foreground only - so
 *   every Bartap call reading through this source runs there too.
 * - Logging Bartap's results through platform/debug.h (RTT), since the library returns codes only.
 * - Choosing where Bartap's working memory lives - the inflate window (D11), the scratch buffer for
 *   opening an archive (D15), each stream's input buffer (D16) - and sizing the structs that embed
 *   the rest (Q2). */

#include "bartap/byte_source.h"
#include "storage/filesystem.h"
