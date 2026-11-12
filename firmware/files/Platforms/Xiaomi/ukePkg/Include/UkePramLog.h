/** @file
  The kernel's own log, left in the reserved pram window for the next boot to print.

  The panel is this board's only console and the only way to read it is a photograph. The pram window
  that already carries XNU's panic record (see UkeBootMemoryDxe's ReadPanicAt) is real DRAM that no OS
  allocates and that survives a reset, so the kernel writes a snapshot of its message buffer there on a
  timer, and the firmware that boots next lifts it into its own log - the one the host collects as
  log/<model>-<date>.log. No photograph, no QR, no storage driver inside the kernel.

  One owner for the geometry. The window base/size are `[payload.adt] panic_region` in the device
  repository's board.toml, emitted by the core generator as the root `pram` node XNU maps and as
  /chosen/embedded-panic-log-size. A second copy of these two numbers is how this port has lost four
  rounds already, so both drivers that read the window include this header, and the record constants
  below are mirrored in exactly one kernel file (package/UkeLastLog.c). work/analysis/check-log-pram.py
  parses both sources and fails when they disagree.

  Layout of the window (physical addresses; the kernel has it mapped write-combining):

    0x9FFE00000  + 0x000000  XNU's struct embedded_panic_header (sizeof 0xE1, packed,
                             osfmk/kern/debug.h) followed by the panic text, which grows forward
                             without wrapping and is capped by gPanicSize - 0xE1
    0x9FFE00000  + 0x1C0000  UKE_LOG_ARENA_OFFSET: this record. Two 128 KiB slots, each a 32-byte
                             header followed by the newest bytes of msgbuf
    0x9FFE00000  + 0x200000  end of the window = end of physical DRAM (dts/uke.dts: the memory node
                             stops at 0x9FFE00000 for exactly this reservation)

  Why 0x1C0000 and not "the free half": the panic text starts at +0xE1 and is bounded by
  gPanicSize - 0xE1 = 0x1BFF1F bytes, so a panic whose text exceeded 1,835,807 bytes would reach the
  arena. Nothing observed comes close (the panic records seen on this board are a few KiB), the kernel
  writer stands down as soon as the header says FUNK, and a real panic outrankning the log snapshot is
  the correct priority anyway.

  Why two slots: the writer re-copies msgbuf through write-combining memory while the machine may be
  reset at any instant. Slot = Sequence & 1, and Commit is cleared before the text is written and set
  only after, so a reset mid-copy damages one slot and the reader still finds the previous snapshot in
  the other. No CRC is needed for that: a torn slot is detected by its own Commit word.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UKE_PRAM_LOG_H_
#define UKE_PRAM_LOG_H_

#include <Uefi.h>
#include <Library/BaseLib.h>

/* [payload.adt] panic_region in the device repository's board.toml (0x9FFE00000:0x200000). The last
 * 2 MiB of the managed 5 GiB run: real DRAM, never handed to XNU's VM (owned_memory_size stops a
 * gibibyte below), and outside DarwinBoot's window. */
#define UKE_PRAM_WINDOW_BASE  0x9FFE00000ULL
#define UKE_PRAM_WINDOW_SIZE  0x200000ULL

#define UKE_LOG_ARENA_OFFSET  0x1C0000ULL  /* 256 KiB tail of the window */
#define UKE_LOG_SLOT_BYTES    0x20000ULL   /* 128 KiB per slot, two slots end exactly at 0x200000 */
#define UKE_LOG_SLOTS         2

#define UKE_LOG_MAGIC     0x474F4C55U /* 'ULOG' little-endian, i.e. the bytes 55 4C 4F 47 */
#define UKE_LOG_COMMIT    0x54494D43U /* 'CMIT': the text below this header is complete */
#define UKE_LOG_VERSION   1U
#define UKE_LOG_FLAG_PANIC_ENTRY  0x1U /* snapshot taken on the way into panic() */

/**
  The record header. The kernel writes exactly these fields in this order
  (package/UkeLastLog.c, uke_log_record_write()); 32 bytes, no padding.
**/
typedef struct {
  UINT32    Magic;      /* UKE_LOG_MAGIC */
  UINT32    Version;    /* UKE_LOG_VERSION */
  UINT32    Sequence;   /* snapshot counter, 1-based, monotonic; slot = Sequence & 1 */
  UINT32    Length;     /* text bytes that follow this header */
  UINT32    Capacity;   /* text capacity of this slot */
  UINT32    Commit;     /* UKE_LOG_COMMIT only once the text is fully written */
  UINT32    Flags;      /* UKE_LOG_FLAG_* */
  UINT32    Reserved;   /* 0 */
} UKE_LOG_RECORD_HEADER;

STATIC_ASSERT (sizeof (UKE_LOG_RECORD_HEADER) == 32, "log record header must stay 32 bytes");
STATIC_ASSERT (OFFSET_OF (UKE_LOG_RECORD_HEADER, Sequence) == 0x08, "log record offset moved");
STATIC_ASSERT (OFFSET_OF (UKE_LOG_RECORD_HEADER, Length) == 0x0C, "log record offset moved");
STATIC_ASSERT (OFFSET_OF (UKE_LOG_RECORD_HEADER, Capacity) == 0x10, "log record offset moved");
STATIC_ASSERT (OFFSET_OF (UKE_LOG_RECORD_HEADER, Commit) == 0x14, "log record offset moved");

#define UKE_LOG_TEXT_CAPACITY  ((UINT32)(UKE_LOG_SLOT_BYTES - sizeof (UKE_LOG_RECORD_HEADER)))

/* Arena + slots must stay inside the window, and the last slot must end exactly at its end. */
STATIC_ASSERT (
  (UKE_LOG_ARENA_OFFSET + UKE_LOG_SLOTS * UKE_LOG_SLOT_BYTES) == UKE_PRAM_WINDOW_SIZE,
  "log arena does not end at the end of the pram window"
  );

#endif /* UKE_PRAM_LOG_H_ */
