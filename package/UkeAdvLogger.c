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
// ALIGNMENT, and why there is no struct below. XNU runs arm64 with SCTLR_EL1.A set, so a misaligned
// access faults, while the firmware's record layout puts a UINT64 TimeStamp at offset 10 and 32-bit
// fields at 12 and 20. A packed struct therefore cannot be used on this buffer: the first version of this
// file did, the compiler emitted `stur xzr, [x22, #0xe]` for the header clear, and b256 panicked with
// "unaligned kernel data abort" (esr 0x96000061 = write + alignment fault, FAR ending in 0xe). Every
// access now goes through memcpy at an explicit byte offset, which is alignment-agnostic by construction.
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

// One owner of the geometry: these must equal uke.dsc's PcdAdvancedLoggerBase and
// PcdAdvancedLoggerPages * 4096 (tools/check-adv-log.py enforces it, and that the window is the reserved
// tail described above).
#define UKE_ADV_BASE        0x9FFE00000ULL
#define UKE_ADV_BYTES       0x40000U     /* 64 pages, PcdAdvancedLoggerPages|64 */
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

static uint8_t *ukeAdvWindow;         /* mapped virtual address of the info header */
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
uke_adv_put8(uint8_t *base, size_t offset, uint8_t value)
{
	base[offset] = value;
}

static void
uke_adv_put16(uint8_t *base, size_t offset, uint16_t value)
{
	base[offset + 0] = (uint8_t)(value);
	base[offset + 1] = (uint8_t)(value >> 8);
}

static void
uke_adv_put32(uint8_t *base, size_t offset, uint32_t value)
{
	base[offset + 0] = (uint8_t)(value);
	base[offset + 1] = (uint8_t)(value >> 8);
	base[offset + 2] = (uint8_t)(value >> 16);
	base[offset + 3] = (uint8_t)(value >> 24);
}

static void
uke_adv_put64(uint8_t *base, size_t offset, uint64_t value)
{
	unsigned byte;

	for (byte = 0; byte < 8; byte++) {
		base[offset + byte] = (uint8_t)(value >> (8 * byte));
	}
}

static uint32_t
uke_adv_get32(const uint8_t *base, size_t offset)
{
	return ((uint32_t)base[offset + 0]) |
	       ((uint32_t)base[offset + 1] << 8) |
	       ((uint32_t)base[offset + 2] << 16) |
	       ((uint32_t)base[offset + 3] << 24);
}

static bool
uke_adv_open(void)
{
	vm_offset_t va;
	uint8_t *window;
	uint32_t offset, size, current;

	if (ukeAdvWindow != NULL) {
		return true;
	}
	if (ukeAdvRefused) {
		return false;
	}
	va = ml_io_map_wcomb((vm_offset_t)UKE_ADV_BASE, (vm_size_t)UKE_ADV_BYTES);
	if (va == 0) {
		IOLog("UkeAdvLogger: cannot map 0x%llx\n", (unsigned long long)UKE_ADV_BASE);
		ukeAdvRefused = true;
		return false;
	}
	window = (uint8_t *)va;
	if (uke_adv_get32(window, UKE_ALI_OFF_SIGNATURE) != UKE_ALOG_SIGNATURE) {
		// The writer used to lay a header down here when it found none, and that is what broke b256/b257:
		// 0x9FFE00000 is NOT free memory. This unit's firmware memory map reports
		//   0x8C0000000-0x9FFDEFFFF   EfiConventionalMemory (7)
		//   0x9FFDF0000-0x9FFFFFFF   EfiReservedMemoryType (0)
		// so the tail belongs to the platform, and a kernel that wrote there made the NEXT firmware boot
		// take a synchronous exception in ArmCpuDxe (CpuExceptionHandlerLib's ASSERT(FALSE)), with the
		// same stack contents every time. It also explains the old "the pram region kills the boot"
		// rounds (b156/b158/b175): that region was never ours to write.
		// So: append to a header the firmware published, never create one. With uke.dsc reverted
		// (PcdAdvancedLoggerFixedInRAM off, base 0) the firmware allocates its own buffer and this writer
		// stands down with one line - which is the correct behaviour until a safe handoff exists (the
		// address the firmware allocated has to reach the kernel, e.g. as an FDT /chosen property).
		IOLog("UkeAdvLogger: no ALOG header at 0x%llx (read 0x%08x); not creating one in reserved memory\n",
		    (unsigned long long)UKE_ADV_BASE, uke_adv_get32(window, UKE_ALI_OFF_SIGNATURE));
		ukeAdvRefused = true;
		return false;
	}
	// Whoever produced the header, only accept geometry this writer can stay inside of.
	offset = uke_adv_get32(window, UKE_ALI_OFF_LOG_OFFSET);
	size = uke_adv_get32(window, UKE_ALI_OFF_SIZE);
	current = uke_adv_get32(window, UKE_ALI_OFF_CURRENT);
	if ((offset < UKE_ALI_BYTES) || ((offset % 8) != 0) || (size == 0) ||
	    (offset + size > UKE_ADV_BYTES) || (current < offset) || (current > offset + size)) {
		IOLog("UkeAdvLogger: refused header offset=%u size=%u current=%u window=%u\n",
		    offset, size, current, UKE_ADV_BYTES);
		ukeAdvRefused = true;
		return false;
	}
	ukeAdvWindow = window;
	ukeAdvLimit = offset + size;
	// Tell the exporter that the buffer outlived ExitBootServices and is in permanent RAM.
	uke_adv_put8(window, UKE_ALI_OFF_PERMANENT_RAM, 1);
	uke_adv_put8(window, UKE_ALI_OFF_AT_RUNTIME, 1);
	return true;
}

// Append one entry. Append-only on purpose: AdvancedLoggerAccessLib walks the area linearly, so a wrap
// would either be skipped or rejected, and the first seconds of a boot are the part worth keeping.
static void
uke_adv_append(const char *text, size_t length)
{
	size_t total, tail;
	uint32_t offset;
	uint8_t *entry;

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
	memcpy(entry + UKE_ALM_BYTES, text, length);
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
