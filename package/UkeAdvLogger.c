// SPDX-License-Identifier: MIT
// Append the kernel message buffer to the firmware's Advanced Logger buffer.
//
// This is the officially documented log path for a board that has no storage driver yet: the firmware
// collects its own log through AdvLoggerPkg (uke.dsc: AdvancedLogDxe, AdvancedLoggerAccessLib,
// QrEncoderLib) and exports it as rotating QR codes from the menu entry Tools -> Advanced Logger, which
// the host scans into log/<model>-<date>.log. Nothing here needs a partition, a filesystem, a storage
// driver or twrp; the only thing missing was that the *kernel* could not add to the same buffer.
//
// Two owners, one geometry. uke.dsc puts the buffer at a fixed physical address
// (gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerBase), 64 pages, with PcdAdvancedLoggerFixedInRAM = TRUE
// (without the feature flag AdvancedLoggerLib ignores the base and allocates its own buffer). The address
// is the tail of DRAM that nothing else uses: this unit's firmware memory map reports
// 0x9FFE00000..0x9FFFFFFFF as EfiReservedMemoryType while the run below it is EfiConventionalMemory,
// dts/uke.dts ends its memory node at 0x9FFE00000 for exactly this reservation, and board.toml stops
// owned_memory_size a gibibyte lower so XNU's VM never hands the window out. tools/check-adv-log.py
// fails when the DSC, this file and the device tree disagree.
//
// ALIGNMENT, and why there is neither a struct nor a plain pointer below. XNU runs arm64 with
// SCTLR_EL1.A set, so a misaligned access faults, while the firmware's record layout puts a UINT64
// TimeStamp at offset 10 and 32-bit fields at 6 and 20. The first version of this file used a packed
// struct: b256 died with `stur xzr,[x22,#0xe]` (esr 0x96000061, write + alignment fault, FAR ending in
// 0xe). The second version used byte stores through a plain uint8_t* - and b257 died anyway, because the
// optimiser is allowed to merge adjacent non-volatile byte stores into one wide store, and it did:
// `stur wzr,[x22,#0x6]`, `strh wzr,[x22,#0xa]`, `stur xzr,[x22,#0x22]`, FAR ending in 6. ARMv8 permits
// unaligned access to normal memory, so the backend has no reason to keep the bytes apart - SCTLR_EL1.A
// is a runtime setting it cannot see. Two things fix it and both are required: the window is accessed
// through volatile pointers (the language forbids merging or reordering those), and this file is built
// with -mstrict-align (scripts/build-boot-log.py), so the backend cannot invent a wide access either.
// tools/check-adv-log.py compiles the file for aarch64 and fails if any store into the window is wider
// than the alignment of the offset it uses; that gate is what the first two versions lacked.
//
// The record layout is AdvLoggerPkg's, not ours: an ADVANCED_LOGGER_INFO header at the base with 'ALOG',
// then entries appended from LogBufferOffset. We write version 2 entries ('ALM2', major 2 minor 1, phase
// ADVANCED_LOGGER_PHASE_RUNTIME) because that is what AdvancedLoggerAccessLib prefers; it walks from
// LOG_BUFFER_FROM_ALI forward to LogCurrentOffset and validates each signature and bound, so this writer
// only ever appends and never wraps. When the buffer runs out it stops for good: the start of a boot is
// what is worth keeping, and a torn ring would make the exporter reject the whole log. The firmware's own
// entries stay untouched: an existing header is validated, never rebuilt.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/appleapiopts.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <sys/msgbuf.h>
#include <IOKit/IOLib.h>

extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
// uke_mark (package/UkeLastLog.c) stamps 31 characters into a slot of a fixed physical page that the
// firmware prints on the NEXT boot ("UKEMARK: no kernel boot marks in ..." is it saying the slots were
// empty). That is the only channel this writer can use to report its own fate: IOLog goes to msgbuf,
// msgbuf is what this writer is trying to publish, and publishing is the thing under test. Slot 7 is the
// one the boot ladder leaves spare. Same archive, so a direct call links.
extern void uke_mark(unsigned int slot, const char *text);

#define UKE_ADV_MARK_SLOT 7

// One owner of the geometry: these must equal uke.dsc's PcdAdvancedLoggerBase and
// PcdAdvancedLoggerPages * 4096 (tools/check-adv-log.py enforces it, and that the window is the reserved
// tail described above).
// Where the firmware's Advanced Logger buffer actually is. This is NOT guessed: the DXE driver prints
// its own geometry into the log it exports, and the ScanLine below came straight out of it
//
//   00:00:11.659 : [DXE  ] [LOAD] Loading driver at ... AdvancedLogDxe.efi
//   00:00:11.671 : [DXE  ] [ERR ] Advanced Logger Info = B112E000, Min = B112E058, Max = B116E000
//
// i.e. INFO at 0xB112E000, first log byte at 0xB112E058 (= INFO + 88, exactly EXPECTED_LOG_BUFFER_OFFSET)
// and the buffer ending at 0xB116E000 (256 KiB, PcdAdvancedLoggerPages|64). Two consecutive boots
// (log/uke-20190100-000038 (6).log and log/uke-20190100-000037 (5).log) report the same address, and it
// sits in the firmware's own low-DRAM allocator window (the DXE log shows Allocations from 0xA7000000
// upwards), which is nowhere near the memory this kernel was handed: the firmware map reports the XNU
// run as EfiConventionalMemory from 0x8C0000000, two and a half decades of bytes above this.
//
// The previous round named 0x9FFE00000 instead, which the firmware map reports as EfiReservedMemoryType
// and which the DXE log shows is full of platform state (UKELOG slots at 0x9FFFC0000 and 0x9FFFE0000, a
// UKEMARK window at 0x9FFE30000, a kernel-snapshot window at 0x9FFE00000+0x1C0000). Writing there is what
// made the next firmware boot die in ArmCpuDxe.
//
// UKE_ADV_SCAN_* is the belt: the address is an allocator's choice, so if it ever moves, find the header
// instead of trusting the constant. Reads only - the writer still refuses to create anything.
#define UKE_ADV_BASE        0xB112E000ULL
#define UKE_ADV_BYTES       0x40000U     /* 64 pages, PcdAdvancedLoggerPages|64 */
#define UKE_ADV_SCAN_BASE   0xB1000000ULL
#define UKE_ADV_SCAN_BYTES  0x00400000U
#define UKE_ADV_SCAN_STEP   0x1000U
#define UKE_ADV_INTERVAL_MS 2000

// A 128 KiB DEVELOPMENT msgbuf seeded as one record would hit the 64 KiB length limit of the v2 entry
// and be truncated silently, so every publish is split into records of this size.
#define UKE_ADV_CHUNK       0x8000U

#define UKE_ALOG_SIGNATURE  0x474F4C41U  /* 'ALOG' */
#define UKE_ALM2_SIGNATURE  0x324D4C41U  /* 'ALM2', ADVANCED_LOGGER_MESSAGE_ENTRY_V2 */
#define UKE_AL_MSG_MAJOR    2
#define UKE_AL_MSG_MINOR    1
#define UKE_AL_PHASE_RUNTIME 5           /* ADVANCED_LOGGER_PHASE_RUNTIME */
#define UKE_AL_LEVEL_INFO   0x40U        /* AdvancedLoggerAccessLib renders this as "[INFO]" */
#define UKE_AL_INFO_VERSION 6U           /* ADVANCED_LOGGER_INFO_VER */

// AdvancedLoggerInternal.h as byte offsets. The info header is 88 bytes (8-byte aligned, which is also
// where EXPECTED_LOG_BUFFER_OFFSET puts the log); the v2 entry is 24 bytes with its message at 24.
#define UKE_ALI_OFF_SIGNATURE        0
#define UKE_ALI_OFF_VERSION          4
#define UKE_ALI_OFF_LOG_OFFSET      12
#define UKE_ALI_OFF_CURRENT         20
#define UKE_ALI_OFF_DISCARDED       24
#define UKE_ALI_OFF_SIZE            28
#define UKE_ALI_OFF_PERMANENT_RAM   32
#define UKE_ALI_OFF_AT_RUNTIME      33
#define UKE_ALI_BYTES               88

#define UKE_ALM_OFF_SIGNATURE        0
#define UKE_ALM_OFF_MAJOR            4
#define UKE_ALM_OFF_MINOR            5
#define UKE_ALM_OFF_LEVEL            6
#define UKE_ALM_OFF_TIMESTAMP       10   /* the firmware's own layout: 4 + 1 + 1 + 4 */
#define UKE_ALM_OFF_PHASE           18
#define UKE_ALM_OFF_LENGTH          20
#define UKE_ALM_OFF_MESSAGE_OFFSET  22
#define UKE_ALM_BYTES               24

_Static_assert(UKE_ALI_BYTES == 88, "ADVANCED_LOGGER_INFO is 88 bytes");
_Static_assert((UKE_ALI_BYTES % 8) == 0, "the log area has to start 8-byte aligned");
_Static_assert(UKE_ALM_BYTES == 24, "ADVANCED_LOGGER_MESSAGE_ENTRY_V2 is 24 bytes");


// NEVER call memcpy here. The kernel's memcpy copies eight bytes at a time and does not check
// alignment, because on arm64 a misaligned access to Normal memory is normally permitted - but this
// kernel runs with SCTLR_EL1.A set, so it is not. b263 died inside `_memcpy + 0xb4` (`str x6,[x3],#8`)
// with esr 0x96000061 and a FAR ending in 0xb2, called from the Advanced Logger's message copy, whose
// source is a byte offset into msgbuf. A volatile byte loop is the only form the compiler may not widen:
// plain `d[i] = s[i]` is merged back into wide stores, exactly like the packed-struct bug of b256.
static void
uke_byte_copy(void *destination, const void *source, size_t count)
{
	volatile unsigned char *to = (volatile unsigned char *)destination;
	const volatile unsigned char *from = (const volatile unsigned char *)source;
	size_t index;

	for (index = 0; index < count; index++) {
		to[index] = from[index];
	}
}

static volatile uint8_t *ukeAdvWindow;  /* mapped virtual address of the info header */
static uint32_t ukeAdvLimit;          /* LogBufferOffset + LogBufferSize */
static uint64_t ukeAdvStamp;
static size_t ukeAdvLast;             /* msgbuf index already published */
static bool ukeAdvPrimed;
static bool ukeAdvRefused;
static bool ukeAdvFull;
static bool ukeAdvStarted;

// Every window access goes through these, and they are BYTE stores/loads on purpose. The firmware's own
// record layout has DebugLevel at offset 6 and TimeStamp at 10, so a 4- or 8-byte store there would be
// misaligned, and even memcpy of a 4-byte value to an unaligned address may be lowered to a single `str w`.
// With SCTLR_EL1.A set, only byte accesses are safe on this buffer. arm64 is little-endian.
static void
uke_adv_put8(volatile uint8_t *base, size_t offset, uint8_t value)
{
	base[offset] = value;
}

static void
uke_adv_put16(volatile uint8_t *base, size_t offset, uint16_t value)
{
	base[offset + 0] = (uint8_t)(value);
	base[offset + 1] = (uint8_t)(value >> 8);
}

static void
uke_adv_put32(volatile uint8_t *base, size_t offset, uint32_t value)
{
	base[offset + 0] = (uint8_t)(value);
	base[offset + 1] = (uint8_t)(value >> 8);
	base[offset + 2] = (uint8_t)(value >> 16);
	base[offset + 3] = (uint8_t)(value >> 24);
}

static void
uke_adv_put64(volatile uint8_t *base, size_t offset, uint64_t value)
{
	unsigned byte;

	for (byte = 0; byte < 8; byte++) {
		base[offset + byte] = (uint8_t)(value >> (8 * byte));
	}
}

static uint32_t
uke_adv_get32(const volatile uint8_t *base, size_t offset)
{
	return ((uint32_t)base[offset + 0]) |
	       ((uint32_t)base[offset + 1] << 8) |
	       ((uint32_t)base[offset + 2] << 16) |
	       ((uint32_t)base[offset + 3] << 24);
}

// Geometry a header has to satisfy before this writer will touch it. Kept as a separate predicate so the
// scan and the fixed probe are held to exactly the same rule.
static bool
uke_adv_geometry_ok(volatile uint8_t *window, uint32_t *offset_out, uint32_t *limit_out)
{
	uint32_t offset, size, current;

	if (uke_adv_get32(window, UKE_ALI_OFF_SIGNATURE) != UKE_ALOG_SIGNATURE) {
		return false;
	}
	offset = uke_adv_get32(window, UKE_ALI_OFF_LOG_OFFSET);
	size = uke_adv_get32(window, UKE_ALI_OFF_SIZE);
	current = uke_adv_get32(window, UKE_ALI_OFF_CURRENT);
	if ((offset < UKE_ALI_BYTES) || ((offset % 8) != 0) || (size == 0) ||
	    (offset + size > UKE_ADV_BYTES) || (current < offset) || (current > offset + size)) {
		return false;
	}
	*offset_out = offset;
	*limit_out = offset + size;
	return true;
}

// "ADV:<tag> <8 hex> <8 hex>", no printf: this runs before anything guarantees a formatting library, and
// uke_mark only takes 31 printable characters plus a terminator.
static void
uke_adv_mark(const char *tag, uint32_t first, uint32_t second)
{
	char text[32];
	const char hex[] = "0123456789abcdef";
	unsigned at = 0;
	int shift;

	text[at++] = 'A';
	text[at++] = 'D';
	text[at++] = 'V';
	text[at++] = ':';
	while ((*tag != '\0') && (at < 12)) {
		text[at++] = *tag++;
	}
	text[at++] = ' ';
	for (shift = 28; shift >= 0; shift -= 4) {
		text[at++] = hex[(first >> shift) & 0xfu];
	}
	text[at++] = ' ';
	for (shift = 28; shift >= 0; shift -= 4) {
		text[at++] = hex[(second >> shift) & 0xfu];
	}
	text[at] = '\0';
	uke_mark(UKE_ADV_MARK_SLOT, text);
}

static bool
uke_adv_open(void)
{
	vm_offset_t va;
	volatile uint8_t *window;
	uint32_t offset, size, current;
	uint32_t limit = 0;

	if (ukeAdvWindow != NULL) {
		return true;
	}
	if (ukeAdvRefused) {
		return false;
	}
	// 1. The address the firmware printed in its own log.
	uke_adv_mark("open 0b112e000", (uint32_t)(UKE_ADV_BASE >> 32), (uint32_t)UKE_ADV_BASE);
	va = ml_io_map_wcomb((vm_offset_t)UKE_ADV_BASE, (vm_size_t)UKE_ADV_BYTES);
	if (va == 0) {
		uke_adv_mark("NOMAP", (uint32_t)(UKE_ADV_BASE >> 32), (uint32_t)UKE_ADV_BASE);
	} else {
		window = (volatile uint8_t *)va;
		if (uke_adv_geometry_ok(window, &offset, &limit)) {
			goto found;
		}
		// The read signature is the whole diagnosis: 0x474F4C41 means the header is there and the
		// geometry is what this writer disagrees with; anything else means the address is wrong.
		uke_adv_mark("NOHEAD", uke_adv_get32(window, UKE_ALI_OFF_SIGNATURE),
		    uke_adv_get32(window, UKE_ALI_OFF_SIZE));
		IOLog("UkeAdvLogger: no usable ALOG header at 0x%llx (read 0x%08x)\n",
		    (unsigned long long)UKE_ADV_BASE, uke_adv_get32(window, UKE_ALI_OFF_SIGNATURE));
	}
	// 2. The address is the firmware allocator's choice, so do not trust the constant: look for the header
	//    inside the allocator's window. This reads only; a header is never created, because a header this
	//    writer invented is exactly what broke b256/b257.
	va = ml_io_map_wcomb((vm_offset_t)UKE_ADV_SCAN_BASE, (vm_size_t)UKE_ADV_SCAN_BYTES);
	if (va != 0) {
		uint32_t scanned;
		for (scanned = 0; scanned + UKE_ADV_BYTES <= UKE_ADV_SCAN_BYTES; scanned += UKE_ADV_SCAN_STEP) {
			window = (volatile uint8_t *)va + scanned;
			if (uke_adv_geometry_ok(window, &offset, &limit)) {
				uke_adv_mark("SCAN", (uint32_t)((UKE_ADV_SCAN_BASE + scanned) >> 32),
				    (uint32_t)(UKE_ADV_SCAN_BASE + scanned));
				IOLog("UkeAdvLogger: found the firmware buffer at 0x%llx by scan\n",
				    (unsigned long long)(UKE_ADV_SCAN_BASE + scanned));
				goto found;
			}
		}
		IOLog("UkeAdvLogger: no ALOG header in 0x%llx..0x%llx either\n",
		    (unsigned long long)UKE_ADV_SCAN_BASE,
		    (unsigned long long)(UKE_ADV_SCAN_BASE + UKE_ADV_SCAN_BYTES));
	}
	// Standing down is a result, not a failure: it means this booting image has no firmware buffer to
	// append to, and inventing one in memory this kernel does not own is what cost us b256 and b257.
	uke_adv_mark("NONE", 0, 0);
	IOLog("UkeAdvLogger: standing down; the log channel needs the firmware's own buffer\n");
	ukeAdvRefused = true;
	return false;

found:
	// Re-read through the same accessors so the values are the ones this writer will use.
	offset = uke_adv_get32(window, UKE_ALI_OFF_LOG_OFFSET);
	size = uke_adv_get32(window, UKE_ALI_OFF_SIZE);
	current = uke_adv_get32(window, UKE_ALI_OFF_CURRENT);
	ukeAdvWindow = window;
	ukeAdvLimit = limit;
	// Tell the exporter that the buffer outlived ExitBootServices and is in permanent RAM.
	uke_adv_put8(window, UKE_ALI_OFF_PERMANENT_RAM, 1);
	uke_adv_put8(window, UKE_ALI_OFF_AT_RUNTIME, 1);
	uke_adv_mark("OK", current, size);
	IOLog("UkeAdvLogger: appending at offset=%u size=%u current=%u of %u\n",
	    offset, size, current, UKE_ADV_BYTES);
	return true;
}

// Append one entry. Append-only on purpose: AdvancedLoggerAccessLib walks the area linearly, so a wrap
// would either be skipped or rejected, and the first seconds of a boot are the part worth keeping.
static void
uke_adv_append(const char *text, size_t length)
{
	size_t total, tail;
	uint32_t offset;
	volatile uint8_t *entry;

	if ((ukeAdvWindow == NULL) || (text == NULL) || (length == 0)) {
		return;
	}
	if (length > 0xffffU - UKE_ALM_BYTES) {
		length = 0xffffU - UKE_ALM_BYTES;
	}
	total = (UKE_ALM_BYTES + length + 7U) & ~(size_t)7U;
	offset = uke_adv_get32(ukeAdvWindow, UKE_ALI_OFF_CURRENT);
	if ((size_t)offset + total > ukeAdvLimit) {
		if (!ukeAdvFull) {
			ukeAdvFull = true;
			IOLog("UkeAdvLogger: buffer full at %u of %u bytes, stopping\n", offset, ukeAdvLimit);
		}
		return;
	}
	entry = ukeAdvWindow + offset;
	uke_adv_put32(entry, UKE_ALM_OFF_SIGNATURE, UKE_ALM2_SIGNATURE);
	uke_adv_put8(entry, UKE_ALM_OFF_MAJOR, UKE_AL_MSG_MAJOR);
	uke_adv_put8(entry, UKE_ALM_OFF_MINOR, UKE_AL_MSG_MINOR);
	uke_adv_put32(entry, UKE_ALM_OFF_LEVEL, UKE_AL_LEVEL_INFO);
	uke_adv_put64(entry, UKE_ALM_OFF_TIMESTAMP, ukeAdvStamp++);
	uke_adv_put16(entry, UKE_ALM_OFF_PHASE, UKE_AL_PHASE_RUNTIME);
	uke_adv_put16(entry, UKE_ALM_OFF_LENGTH, (uint16_t)length);
	uke_adv_put16(entry, UKE_ALM_OFF_MESSAGE_OFFSET, UKE_ALM_BYTES);
	// The destination is 8-byte aligned (offset is, UKE_ALM_BYTES is 24) but the SOURCE is not: it is a
	// byte offset into msgbuf. memcpy is not safe in this kernel for that reason - see uke_byte_copy.
	uke_byte_copy((void *)(entry + UKE_ALM_BYTES), text, length);
	// Zero the alignment tail: a stale byte here is what a reader would take for the next signature.
	for (tail = length; tail < total - UKE_ALM_BYTES; tail++) {
		uke_adv_put8(entry, UKE_ALM_BYTES + tail, 0);
	}
	uke_adv_put32(ukeAdvWindow, UKE_ALI_OFF_CURRENT, offset + (uint32_t)total);
}

// Split a run into records the entry format can hold.
static void
uke_adv_publish(const char *text, size_t length)
{
	while (length > 0) {
		size_t part = length > UKE_ADV_CHUNK ? UKE_ADV_CHUNK : length;

		uke_adv_append(text, part);
		text += part;
		length -= part;
	}
}

// The newest msgbuf bytes that have not been published yet. msgbuf is a ring with msg_bufx as the write
// cursor, so the delta is one or two contiguous runs; an overrun (more than a full ring since the last
// call) keeps the newest ring rather than a torn line.
static void
uke_adv_pump(void)
{
	struct msgbuf *m = msgbufp;
	size_t size, cursor, advance, first;
	int write;

	if ((ukeAdvWindow == NULL) || (m == NULL) || (m->msg_bufc == NULL) ||
	    (m->msg_size <= 1) || (m->msg_magic != MSG_MAGIC)) {
		return;
	}
	size = (size_t)m->msg_size;
	write = m->msg_bufx;
	if ((write < 0) || ((size_t)write >= size)) {
		return;
	}
	cursor = (size_t)write;
	if (!ukeAdvPrimed) {
		// First call: seed with the whole ring, which is the boot so far.
		ukeAdvPrimed = true;
		ukeAdvLast = cursor;
		uke_adv_publish((const char *)m->msg_bufc, size);
		return;
	}
	advance = (cursor + size - ukeAdvLast) % size;
	if (advance == 0) {
		return;
	}
	if (advance >= size - 1) {
		ukeAdvLast = cursor;
		uke_adv_publish((const char *)m->msg_bufc, size);
		return;
	}
	first = size - ukeAdvLast;
	if (first > advance) {
		first = advance;
	}
	uke_adv_publish((const char *)m->msg_bufc + ukeAdvLast, first);
	if (advance > first) {
		uke_adv_publish((const char *)m->msg_bufc, advance - first);
	}
	ukeAdvLast = cursor;
}

static void
uke_adv_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	uke_adv_append("UKE-ADVLOG: writer alive\n", sizeof("UKE-ADVLOG: writer alive\n") - 1);
	for (;;) {
		uke_adv_pump();
		IOSleep(UKE_ADV_INTERVAL_MS);
	}
}

// Called from UkeLastLog's own start path rather than from a STARTUP of its own: the STARTUP registration
// alone has never produced observable output in this kernel, while the bsd_init hook that calls
// uke_last_log_start() is registered in patches/xnu/series and does run.
void
uke_adv_log_start(void)
{
	thread_t thread;

	if (ukeAdvStarted) {
		return;
	}
	ukeAdvStarted = true;
	if (!uke_adv_open()) {
		return;
	}
	if (kernel_thread_start_priority(uke_adv_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
