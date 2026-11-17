// SPDX-License-Identifier: MIT
// Append the kernel message buffer to the firmware's Advanced Logger buffer.
//
// This is the officially documented log path for a board that has no storage driver yet: the firmware
// collects its own log through AdvLoggerPkg (uke.dsc: AdvancedLogDxe, AdvancedLoggerAccessLib,
// QrEncoderLib) and exports it as rotating QR codes from the menu entry Tools -> Advanced Logger, which
// the host scans into log/<model>-<date>.log. Nothing here needs a partition, a filesystem, a storage
// driver or twrp; the only thing missing was that the *kernel* had no way to add to the same buffer.
//
// Two owners, one geometry. uke.dsc puts the buffer at a fixed physical address
// (gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerBase) with 64 pages
// (gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerPages), and the address it names is the tail of DRAM that
// nothing else uses: this unit's firmware memory map reports 0x9FFE00000..0x9FFFFFFFF as
// EfiReservedMemoryType (the run below it is EfiConventionalMemory), dts/uke.dts ends its memory node at
// 0x9FFE00000 for exactly this reservation, and board.toml owned_memory_size stops a gibibyte lower, so
// XNU's VM never hands the window out. tools/check-adv-log.py fails when the DSC, this file and the
// device tree disagree about any of that.
//
// The record layout is AdvLoggerPkg's, not ours: a packed ADVANCED_LOGGER_INFO header at the base with
// 'ALOG', then entries appended from LogBufferOffset. We write version 2 entries ('ALM2', major 2 minor 1,
// phase ADVANCED_LOGGER_PHASE_RUNTIME) because that is what AdvancedLoggerAccessLib prefers; it walks
// from LOG_BUFFER_FROM_ALI forward to LogCurrentOffset and validates each signature and bounds, so this
// writer only ever appends and never wraps. When the buffer runs out it stops for good: the start of a
// boot is what is worth keeping, and a torn ring would make the exporter reject the whole log.
//
// The firmware's own entries stay untouched: the header is validated, never rebuilt.
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

// One owner of the geometry: these two must equal uke.dsc's PcdAdvancedLoggerBase and
// PcdAdvancedLoggerPages * 4096 (tools/check-adv-log.py enforces it, and also that the window is the
// reserved tail described above).
#define UKE_ADV_BASE        0x9FFE00000ULL
#define UKE_ADV_BYTES       0x40000U     /* 64 pages, PcdAdvancedLoggerPages|64 */
#define UKE_ADV_INTERVAL_MS 2000

#define UKE_ALOG_SIGNATURE  0x474F4C41U  /* 'ALOG' */
#define UKE_ALM2_SIGNATURE  0x324D4C41U  /* 'ALM2', ADVANCED_LOGGER_MESSAGE_ENTRY_V2 */
#define UKE_AL_MSG_MAJOR    2
#define UKE_AL_MSG_MINOR    1
#define UKE_AL_PHASE_RUNTIME 5           /* ADVANCED_LOGGER_PHASE_RUNTIME */
#define UKE_AL_LEVEL_INFO   0x40U        /* AdvancedLoggerAccessLib renders this as "[INFO]" */
#define UKE_AL_INFO_VERSION 6U           /* ADVANCED_LOGGER_INFO_VER */

// AdvancedLoggerInternal.h, packed. Field order and widths are the firmware's; the static asserts below
// pin the two offsets AdvancedLoggerAccessLib computes from them.
struct uke_adv_info {
	uint32_t signature;
	uint16_t version;
	uint16_t reserved[3];
	uint32_t log_buffer_offset;
	uint32_t reserved4;
	uint32_t log_current_offset;
	uint32_t discarded_size;
	uint32_t log_buffer_size;
	uint8_t  in_permanent_ram;
	uint8_t  at_runtime;
	uint8_t  gone_virtual;
	uint8_t  hdw_port_initialized;
	uint8_t  hdw_port_disabled;
	uint8_t  reserved2[3];
	uint64_t timer_frequency;
	uint64_t ticks_at_time;
	uint8_t  time[16];                /* EFI_TIME */
	uint32_t hw_print_level;
	uint32_t reserved3;
	uint64_t deprecated;
} __attribute__((packed));

struct uke_adv_entry {
	uint32_t signature;
	uint8_t  major;
	uint8_t  minor;
	uint32_t level;
	uint64_t timestamp;
	uint16_t phase;
	uint16_t length;
	uint16_t offset;                  /* offset of MessageText from the start of this structure */
} __attribute__((packed));

_Static_assert(sizeof(struct uke_adv_info) == 88, "ADVANCED_LOGGER_INFO layout changed");
_Static_assert((sizeof(struct uke_adv_info) % 8) == 0, "the log buffer offset must stay 8-byte aligned");
_Static_assert(sizeof(struct uke_adv_entry) == 24, "ADVANCED_LOGGER_MESSAGE_ENTRY_V2 layout changed");

static volatile struct uke_adv_info *ukeAdvInfo;
static uint8_t *ukeAdvLog;
static size_t ukeAdvLimit;            /* log_buffer_offset + log_buffer_size */
static uint64_t ukeAdvStamp;
static size_t ukeAdvLast;             /* msgbuf index already published */
static bool ukeAdvPrimed;
static bool ukeAdvRefused;
static bool ukeAdvFull;
static bool ukeAdvStarted;

static bool
uke_adv_open(void)
{
	vm_offset_t va;

	if (ukeAdvInfo != NULL) {
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
	volatile struct uke_adv_info *info = (volatile struct uke_adv_info *)va;
	if (info->signature != UKE_ALOG_SIGNATURE) {
		// Nothing has claimed the window yet: the firmware's DxeCore logger only adopts a fixed address
		// whose header already validates (AdvancedLoggerLib.c:144 and :164) and otherwise allocates its
		// own buffer, so on the first boot this region is still whatever the reset left. The region is
		// reserved for exactly this purpose and nothing else writes it, so lay down a header that the
		// firmware's own ValidateInfoBlock() will accept on the next boot - at which point it appends its
		// log after ours instead of allocating elsewhere.
		IOLog("UkeAdvLogger: no ALOG header at 0x%llx (read 0x%08x), creating one\n",
		    (unsigned long long)UKE_ADV_BASE, info->signature);
		memset((void *)info, 0, sizeof(struct uke_adv_info));
		info->signature = UKE_ALOG_SIGNATURE;
		info->version = UKE_AL_INFO_VERSION;
		info->log_buffer_offset = sizeof(struct uke_adv_info);
		info->log_buffer_size = UKE_ADV_BYTES - (uint32_t)sizeof(struct uke_adv_info);
		info->log_current_offset = sizeof(struct uke_adv_info);
		info->in_permanent_ram = 1;
		info->at_runtime = 1;
	}
	// The firmware's own writer owns the header: accept what it published, refuse anything that would
	// let this writer step outside the window it describes.
	if ((info->log_buffer_offset < sizeof(struct uke_adv_info)) ||
	    ((info->log_buffer_offset % 8) != 0) ||
	    (info->log_buffer_size == 0) ||
	    ((uint32_t)info->log_buffer_offset + info->log_buffer_size > UKE_ADV_BYTES)) {
		IOLog("UkeAdvLogger: refused header offset=%u size=%u window=%u\n",
		    info->log_buffer_offset, info->log_buffer_size, UKE_ADV_BYTES);
		ukeAdvRefused = true;
		return false;
	}
	if ((info->log_current_offset < info->log_buffer_offset) ||
	    (info->log_current_offset > (uint32_t)info->log_buffer_offset + info->log_buffer_size)) {
		IOLog("UkeAdvLogger: refused cursor %u outside the log area\n", info->log_current_offset);
		ukeAdvRefused = true;
		return false;
	}
	ukeAdvInfo = info;
	ukeAdvLog = (uint8_t *)va + info->log_buffer_offset;
	ukeAdvLimit = (size_t)info->log_buffer_offset + info->log_buffer_size;
	// Tell the exporter that the buffer outlived ExitBootServices and is in permanent RAM.
	info->in_permanent_ram = 1;
	info->at_runtime = 1;
	return true;
}

// Append one entry. Append-only on purpose: AdvancedLoggerAccessLib walks the area linearly, so a wrap
// would either be skipped or rejected, and the first seconds of a boot are the part worth keeping.
static void
uke_adv_append(const char *text, size_t length)
{
	const size_t header = sizeof(struct uke_adv_entry);
	size_t total;
	uint32_t offset;

	if ((ukeAdvInfo == NULL) || (text == NULL) || (length == 0)) {
		return;
	}
	if (length > 0xffffU - header) {
		length = 0xffffU - header;
	}
	total = (header + length + 7U) & ~(size_t)7U;
	offset = ukeAdvInfo->log_current_offset;
	if ((size_t)offset + total > ukeAdvLimit) {
		if (!ukeAdvFull) {
			ukeAdvFull = true;
			IOLog("UkeAdvLogger: buffer full at %u of %zu bytes, stopping\n",
			    offset, ukeAdvLimit);
		}
		return;
	}
	struct uke_adv_entry *entry = (struct uke_adv_entry *)(ukeAdvLog + (offset - ukeAdvInfo->log_buffer_offset));
	entry->signature = UKE_ALM2_SIGNATURE;
	entry->major = UKE_AL_MSG_MAJOR;
	entry->minor = UKE_AL_MSG_MINOR;
	entry->level = UKE_AL_LEVEL_INFO;
	entry->timestamp = ukeAdvStamp++;
	entry->phase = UKE_AL_PHASE_RUNTIME;
	entry->length = (uint16_t)length;
	entry->offset = (uint16_t)header;
	memcpy((uint8_t *)entry + header, text, length);
	// Zero the alignment tail: a stale byte here is what a reader would take for the next signature.
	memset((uint8_t *)entry + header + length, 0, total - header - length);
	ukeAdvInfo->log_current_offset = offset + (uint32_t)total;
}

// The newest msgbuf bytes that have not been published yet. msgbuf is a ring with msg_bufx as the write
// cursor, so the delta is one or two contiguous runs; an overrun (more than a full ring since the last
// call) keeps the newest half rather than a torn line.
static void
uke_adv_pump(void)
{
	struct msgbuf *m = msgbufp;

	if ((ukeAdvInfo == NULL) || (m == NULL) || (m->msg_bufc == NULL) ||
	    (m->msg_size <= 1) || (m->msg_magic != MSG_MAGIC)) {
		return;
	}
	const size_t size = (size_t)m->msg_size;
	const int write = m->msg_bufx;
	if ((write < 0) || ((size_t)write >= size)) {
		return;
	}
	const size_t cursor = (size_t)write;
	if (!ukeAdvPrimed) {
		// First call: seed with the whole ring, which is the boot so far.
		ukeAdvPrimed = true;
		ukeAdvLast = cursor;
		uke_adv_append((const char *)m->msg_bufc, size);
		return;
	}
	size_t advance = (cursor + size - ukeAdvLast) % size;
	if (advance == 0) {
		return;
	}
	if (advance >= size - 1) {
		ukeAdvLast = cursor;
		uke_adv_append((const char *)m->msg_bufc, size);
		return;
	}
	size_t first = size - ukeAdvLast;
	if (first > advance) {
		first = advance;
	}
	uke_adv_append((const char *)m->msg_bufc + ukeAdvLast, first);
	if (advance > first) {
		uke_adv_append((const char *)m->msg_bufc, advance - first);
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
