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
 *   BARTAP_BYTE_SOURCE_RESULT_IO_ERROR, keeping the last filesystem_result_t for the log. How detail
 *   travels may change with Q3 and Q4 in lib/bartap/DESIGN-QUESTIONS.md.
 *   Execution model: the filesystem layer's - the storage coordinator's core, foreground only - so
 *   every Bartap call reading through this source runs there too.
 * - Logging Bartap's results through platform/debug.h (RTT), since the library returns codes only.
 * - Choosing where Bartap's working memory lives (the inflate window and tables, stream buffers): Q1
 *   and Q2. */

#include "bartap/byte_source.h"
#include "storage/filesystem.h"
