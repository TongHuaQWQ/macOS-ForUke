// SPDX-License-Identifier: MIT
// Keep a persistent dump of the kernel message buffer in the reserved pram.
//
// The window is the one XNU maps for its embedded panic log (`[payload.adt] panic_region`,
// 0x9FFE00000 for 0x200000 bytes, published as the root `pram` node). This file writes a *log record*
// of its own at +0x1C0000 - see the constants below - and the firmware that boots next prints it as
// UKELOG: lines in its own log file, which is how a kernel log becomes readable without a photograph
// of the panel and without a storage driver inside the kernel.
//
// The low half of the window stays XNU's embedded-panic FUNK record; the ramoops half at +0x200000 is
// a Linux pstore console ring (persistent_ram_buffer, sig 0x43474244) and is inert while the window is
// 2 MiB (its guard below skips it). The same snapshot can also be appended to the userdata log window
// through NativeUfs, which is what makes it readable from recovery; that half needs sheng-log=1 and
// the writer never allocates, changes translation, or touches a device register.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/appleapiopts.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include <IOKit/IOLib.h>

extern vm_offset_t gPanicBase;
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern unsigned int gPanicSize;
extern int desiredvnodes;
extern uint64_t sane_size;
extern void uke_crash_log_start(void);
extern void pe_print_last_panic_again(void);

#define UKE_LAST_LOG_INTERVAL_MS 500

// The userdata (UFS) half is built only when the configuration asks for it
// (build-boot-log.py passes -DUKE_LOG_USERDATA=1 when CONFIG_XNU_LOG_USERDATA=y). Without it only the
// pram record is written, which is the half the firmware reads back on the next boot.
#ifndef UKE_LOG_USERDATA
#define UKE_LOG_USERDATA 0
#endif

// The ramoops console zone: a Linux pstore region at the window's second MiB, which the uke Linux
// ramoops backend exposes at /sys/fs/pstore/console-ramoops-0. Skipped unless the window is >= 4 MiB.
#define UKE_RAMOOPS_OFFSET  0x200000u
#define UKE_RAMOOPS_SIZE    0x200000u
#define UKE_RAMOOPS_MAGIC   0x43474244u /* "DBGC", see Linux pstore_ram.h */

// ---- the log record ------------------------------------------------------------------------
//
// Why this is not the embedded-panic FUNK header any more. Until b150 this writer published the msgbuf
// tail *as* an embedded panic record (eph_magic = FUNK, text after the header, eph_panic_log_len =
// length). Three readers take that header at its word - XNU's own check_for_panic_log /
// pe_print_last_panic, the firmware's ReadPanicAt (UkeBootMemoryDxe.c) and the boot menu's "last
// panic:" line - so a plain log snapshot was indistinguishable from a real panic: the menu showed a
// random kernel log line as the panic, and the firmware's UKEMEM: panic line did the same. The FUNK
// header now belongs to the panic path alone (XNU's PE_init_panicheader / SavePanicInfo and the port's
// uke_stage_last_panic), and the log record has its own magic, in its own area of the same window.
//
// Layout of the record area, at the tail of the 2 MiB window:
//
//   0x9FFE00000 + 0x000000   XNU's panic header (0xE1 bytes) and panic text, growing forward
//   0x9FFE00000 + 0x1C0000   slot 0: 32-byte header + up to 131040 bytes of msgbuf
//   0x9FFE00000 + 0x1E0000   slot 1: the same
//   0x9FFE00000 + 0x200000   end of the window = end of physical DRAM
//
//   arena      = 0x200000 - 0x1C0000 = 0x40000 = 262144 bytes, two slots of 0x20000 = 131072
//   text/slot  = 0x20000 - 32 = 131040 bytes; msgbuf is 128 KiB in a DEVELOPMENT kernel
//                (CONFIG_MSG_BSIZE_DEV, config/MASTER:282), so a full ring fits with 32 bytes spare
//   collision  = a panic text would have to exceed 0x1C0000 - 0xE1 = 1834783 bytes (1.75 MiB) to reach
//                slot 0, and XNU caps it at gPanicSize - 0xE1 = 2096927; the writer also stops for good
//                as soon as the header reads FUNK, so the only way to touch it is a panic that both ran
//                past 1.75 MiB and happened after this writer's last snapshot.
//
// Mirrored, field for field, in the firmware reader
// workspace/uefi-uke/Platforms/Xiaomi/ukePkg/Include/UkePramLog.h (also archived as
// firmware/files/Platforms/Xiaomi/ukePkg/Include/UkePramLog.h); work/analysis/check-log-pram.py
// parses both sources and fails when they drift.
#define UKE_LOG_ARENA_OFFSET   0x1C0000u
#define UKE_LOG_SLOT_BYTES     0x20000u
#define UKE_LOG_SLOTS          2u
#define UKE_LOG_HDR_BYTES      32u
#define UKE_LOG_TEXT_CAPACITY  (UKE_LOG_SLOT_BYTES - UKE_LOG_HDR_BYTES)
#define UKE_LOG_MAGIC          0x474F4C55u /* 'ULOG' */
#define UKE_LOG_COMMIT         0x54494D43u /* 'CMIT' */
#define UKE_LOG_VERSION        1u

struct uke_log_header {
	uint32_t magic;
	uint32_t version;
	uint32_t sequence;
	uint32_t length;
	uint32_t capacity;
	uint32_t commit;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed));

static uint32_t ukeLogSequence;
static uint64_t ukeLogLastTick;
#if UKE_LOG_USERDATA
// sheng-log=1: the userdata (UFS) half only. It is a boot arg of its own because that transport maps
// the UFS controller and programs its SMMU before any kext runs, which is a separate experiment from
// "keep a log in DRAM" and must stay switchable without touching this writer.
static int ukeShengLog;
#endif

struct uke_ramoops_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	uint8_t data[];
};

static uint32_t uke_ramoops_last_write;
static int uke_ramoops_initialized;
/* Set by the panic screen before it stages its text at gPanicBase; the two pram
 * writers below must leave that block alone once it is set. */
static volatile int ukePanicStaged;

static void
uke_ramoops_append(struct uke_ramoops_buffer *buffer, size_t capacity,
    const uint8_t *src, size_t count)
{
	size_t start, size, first;

	if (count == 0) {
		return;
	}
	if (count > capacity) {
		src += count - capacity;
		count = capacity;
	}
	start = buffer->start;
	size = buffer->size;
	if (start >= capacity) {
		start = 0;
	}
	if (size > capacity) {
		size = capacity;
	}
	first = capacity - start;
	if (first > count) {
		first = count;
	}
	memcpy(buffer->data + start, src, first);
	if (count > first) {
		memcpy(buffer->data, src + first, count - first);
	}
	size += count;
	if (size > capacity) {
		size = capacity;
	}
	start = (start + count) % capacity;
	// The Linux reader runs after a reset, but publish the bytes before the
	// header so a concurrent reader never sees a size that covers stale data.
	__asm__ volatile("dsb sy" ::: "memory");
	buffer->size = (uint32_t)size;
	buffer->start = (uint32_t)start;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
uke_ramoops_write(struct msgbuf *m)
{
	struct uke_ramoops_buffer *buffer;
	size_t capacity, size, start, first, tail;
	int write;

	if (gPanicBase == 0 || ukePanicStaged ||
	    gPanicSize < UKE_RAMOOPS_OFFSET + UKE_RAMOOPS_SIZE) {
		return;
	}
	buffer = (struct uke_ramoops_buffer *)(uintptr_t)(gPanicBase + UKE_RAMOOPS_OFFSET);
	capacity = UKE_RAMOOPS_SIZE - sizeof(*buffer);
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}
	size = (size_t)m->msg_size;
	if (!uke_ramoops_initialized) {
		buffer->sig = UKE_RAMOOPS_MAGIC;
		buffer->start = 0;
		buffer->size = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		uke_ramoops_initialized = 1;
		// Seed the ring with the current msgbuf tail: the display/root work
		// runs before the first 500 ms tick, so a delta-only start would lose
		// the interesting beginning of the boot.
		tail = capacity;
		if (tail > size) {
			tail = size;
		}
		if (tail > (size_t)write) {
			tail = (size_t)write;
		}
		start = ((size_t)write + size - tail) % size;
		first = size - start;
		if (first > tail) {
			first = tail;
		}
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + start, first);
		if (tail > first) {
			uke_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
			    tail - first);
		}
		uke_ramoops_last_write = (uint32_t)write;
		return;
	}
	if ((uint32_t)write == uke_ramoops_last_write) {
		return;
	}
	if ((uint32_t)write > uke_ramoops_last_write) {
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + uke_ramoops_last_write,
		    (size_t)write - uke_ramoops_last_write);
	} else {
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + uke_ramoops_last_write,
		    size - uke_ramoops_last_write);
		uke_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
		    (size_t)write);
	}
	uke_ramoops_last_write = (uint32_t)write;
}

// Copy the newest bytes of msgbuf into the log record's next slot. Plain stores into the
// write-combining mapping XNU already made for this window, plus a barrier; no allocation, no lock, and
// nothing that can sleep, so this is safe from a thread call, from a probe and from a lock path.
static void
uke_log_record_write(void)
{
	struct uke_log_header *hdr;
	struct msgbuf *m = msgbufp;
	uintptr_t slot;
	char *text;
	size_t size, cap, start, first;
	int write;
	uint32_t sequence;

	if (gPanicBase == 0 || ukePanicStaged) {
		return;
	}
	if ((uint64_t)gPanicSize <
	    (uint64_t)UKE_LOG_ARENA_OFFSET + UKE_LOG_SLOTS * UKE_LOG_SLOT_BYTES) {
		return;
	}
	// Once the header reads FUNK the window belongs to the panic, and a snapshot here would be a log
	// line written over a panic record. See the arena arithmetic above for the only case where the two
	// can meet at all; the panic wins.
	if (*(volatile uint32_t *)(uintptr_t)gPanicBase == (uint32_t)EMBEDDED_PANIC_MAGIC) {
		return;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}

	size = (size_t)m->msg_size;
	cap = UKE_LOG_TEXT_CAPACITY;
	if (cap > size) {
		cap = size;
	}
	sequence = ukeLogSequence + 1u;
	slot = (uintptr_t)gPanicBase + UKE_LOG_ARENA_OFFSET +
	    (uintptr_t)(sequence % UKE_LOG_SLOTS) * UKE_LOG_SLOT_BYTES;
	hdr = (struct uke_log_header *)slot;
	text = (char *)(slot + UKE_LOG_HDR_BYTES);

	// Publish order: clear Commit first, copy the text, fill the header, set Commit last. A reset
	// anywhere in between leaves this slot rejected by the firmware reader and the other slot holding
	// the previous snapshot, which is why the slot alternates.
	hdr->commit = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	// msg_bufr only advances when a reader drains the log, so it may point at stale bytes after the
	// ring wraps. The newest cap bytes before msg_bufx are the ones a reset needs.
	start = ((size_t)write + size - cap) % size;
	first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy(text, m->msg_bufc + start, first);
	if (cap > first) {
		memcpy(text + first, m->msg_bufc, cap - first);
	}
	__asm__ volatile("dsb sy" ::: "memory");

	hdr->magic = UKE_LOG_MAGIC;
	hdr->version = UKE_LOG_VERSION;
	hdr->sequence = sequence;
	hdr->length = (uint32_t)cap;
	hdr->capacity = (uint32_t)UKE_LOG_TEXT_CAPACITY;
	hdr->flags = 0;
	hdr->reserved = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	hdr->commit = UKE_LOG_COMMIT;
	__asm__ volatile("dsb sy" ::: "memory");

	ukeLogSequence = sequence;
}

// Entry point for call sites outside this file. `why` names the caller in a future record version and is
// not stored yet; it exists so a reader of the call site does not have to guess what it is for.
//
// Rate limited to one snapshot per second, because the callers are independent of each other (the
// heartbeat thread call every 2 s, the writer thread below every 500 ms, and any probe added later) and
// each snapshot is up to 128 KiB through a write-combining mapping.
void
uke_log_snapshot(const char *why)
{
	uint64_t now, frequency;

	(void)why;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
	if (ukeLogLastTick != 0) {
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
		if (frequency == 0) {
			frequency = 24000000u;   // the platform's reference clock; backstop for a bad CNTFRQ
		}
		if ((now - ukeLogLastTick) < frequency) {
			return;
		}
	}
	ukeLogLastTick = now;
	uke_log_record_write();
}

// The writer thread's tick: one snapshot, plus the ramoops half when the window is large enough.
static void
uke_last_log_write(void)
{
	if (gPanicBase != 0 && !ukePanicStaged) {
		uke_ramoops_write(msgbufp);
	}
	uke_log_snapshot("thread");
}

// ---- userdata copy --------------------------------------------------------------------------
//
// The pram ring above survives only until the next reset and cannot be read
// from outside the device. The same snapshot is therefore also appended to the
// log window NativeUfs keeps at the end of userdata, which a recovery image or
// a host can read directly with dd.
//
// Layout: the window is a ring of 16 KiB records. Each record is
//
//   [ 0] magic "UKELLOG1"
//   [ 8] sequence number (u64, monotonic)
//   [16] payload length (u32)
//   [20] payload crc32 (u32)
//   [24] reserved
//   [32] payload (the newest bytes of the kernel message buffer)
//
// A reader sorts by sequence and concatenates the contiguous run it finds.
//
// The userdata window needs NativeUfs (`CONFIG_XNU_NATIVE_UFS`), which the macOS
// prelink configuration turns off. Defining UKE_LOG_USERDATA=1 in a configuration
// that links it brings the second copy back; without it only the pram half is
// built, which is the half the UEFI side reads back over USB.
//
#define UKE_UFS_RECORD_BYTES   (16 * 1024)
#define UKE_UFS_RECORD_BLOCKS  (UKE_UFS_RECORD_BYTES / 4096)
#define UKE_UFS_PAYLOAD_BYTES  (UKE_UFS_RECORD_BYTES - 32)
#define UKE_UFS_INTERVAL_MS    2000

#if UKE_LOG_USERDATA
int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why);
int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src);
int native_ufs_log_sync(void);
// package/UfsLogKernel.c: maps the host controller and apps-SMMU windows, allocates the transport
// buffer and lock, stops the firmware's request list, programs the UFS stream's SMMU context and
// proves the result with a NOP OUT. Three symbols above then serve the window it attached to.
//
// Called once, not on every tick: it rewrites SMMU routing and stops the boot chain's request list,
// so an unbounded retry would keep re-entering that sequence. UKE_UFS_START_ATTEMPTS spaced ticks is
// enough to ride out an early failure and still stop for good afterwards.
#define UKE_UFS_START_ATTEMPTS 5
int ufs_log_start(const char **why);
// The pram beacon slot (+0x1100) is what the firmware's menu prints on the NEXT boot when it finds no
// panic text, so it is the one channel that says whether the userdata writer got anywhere without a
// working USB console, a readable window, or a surviving msgbuf. Declared here because its definition
// is below this half of the file, and a helper used above its definition is what broke b106.
static void uke_last_log_beacon(const char *text);
#endif

#if UKE_LOG_USERDATA
static uint32_t
uke_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffU;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

static uint8_t ukeUfsBuffer[UKE_UFS_RECORD_BYTES] __attribute__((aligned(4096)));
static bool ukeUfsStarted;
static unsigned ukeUfsStartAttempts;
static bool ukeUfsOpen;
static bool ukeUfsRefused;
static uint64_t ukeUfsBlocks;
static uint64_t ukeUfsSeq;
static uint64_t ukeUfsCursor;
#endif /* UKE_LOG_USERDATA */

#if UKE_LOG_USERDATA
// One record: the newest UKE_UFS_PAYLOAD_BYTES of the message buffer.
static size_t
uke_snapshot(uint8_t *out, size_t capacity)
{
	struct msgbuf *m = msgbufp;
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	const int write = m->msg_bufx;
	if (write <= 0) {
		return 0;
	}
	size_t size = (size_t)m->msg_size;
	size_t cap = capacity;
	if (cap > size) {
		cap = size;
	}
	size_t start = ((size_t)write + size - cap) % size;
	size_t first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy(out, m->msg_bufc + start, first);
	if (cap > first) {
		memcpy(out + first, m->msg_bufc, cap - first);
	}
	return cap;
}

static void
uke_ufs_publish(void)
{
	if (ukeUfsRefused) {
		return;
	}
	// Before the window: the transport that serves it. UfsLogKernel.c owns everything the kernel has
	// to do itself -- map 0x01d84000+0x3000 and the apps SMMU, allocate the contiguous request list,
	// stop the firmware's list, program stream 0x60 and send one NOP OUT. It is idempotent once it
	// succeeds, and it is what makes native_ufs_log_* exist at all without CONFIG_XNU_NATIVE_UFS.
	if (!ukeUfsStarted) {
		const char *why = "";
		if (!ufs_log_start(&why)) {
			if (++ukeUfsStartAttempts < UKE_UFS_START_ATTEMPTS) {
				return;
			}
			IOLog("UkeLastLog: userdata log transport unavailable: %s\n",
			    why != NULL && why[0] != '\0' ? why : "(no reason)");
			uke_last_log_beacon("UKELOG-NOTRANSPORT");
			ukeUfsRefused = true;
			return;
		}
		ukeUfsStarted = true;
		IOLog("UkeLastLog: userdata log transport attached\n");
		uke_last_log_beacon("UKELOG-TRANSPORT-OK");
	}
	if (!ukeUfsOpen) {
		const char *why = "";
		uint64_t first = 0, blocks = 0;
		if (!native_ufs_log_open(&first, &blocks, &why)) {
			// The geometry arrives as uke-log-first/uke-log-last from board.toml, and hostLogOpen
			// already knows the transport answered a NOP OUT, so none of these reasons is going to
			// change on a later tick.
			IOLog("UkeLastLog: userdata log window unavailable: %s\n",
			    why != NULL && why[0] != '\0' ? why : "(no reason)");
			uke_last_log_beacon("UKELOG-NOWINDOW");
			ukeUfsRefused = true;
			return;
		}
		ukeUfsBlocks = blocks;
		ukeUfsCursor = 0;
		ukeUfsSeq = 0;
		ukeUfsOpen = true;
		IOLog("UkeLastLog: userdata log window at lba %llu, %llu blocks\n",
		    (unsigned long long)first, (unsigned long long)blocks);
		// Distinguishable from TRANSPORT-OK on the menu even when the window then never fills.
		uke_last_log_beacon("UKELOG-WINDOW-OPEN");
	}

	bzero(ukeUfsBuffer, sizeof(ukeUfsBuffer));
	const size_t length = uke_snapshot(ukeUfsBuffer + 32, UKE_UFS_PAYLOAD_BYTES);
	if (length == 0) {
		return;
	}
	memcpy(ukeUfsBuffer, "UKELLOG1", 8);
	uint64_t *seq = (uint64_t *)(ukeUfsBuffer + 8);
	*seq = ukeUfsSeq;                       // little-endian AArch64
	uint32_t *len = (uint32_t *)(ukeUfsBuffer + 16);
	*len = (uint32_t)length;
	uint32_t *crc = (uint32_t *)(ukeUfsBuffer + 20);
	*crc = uke_crc32(ukeUfsBuffer + 32, length);
	__asm__ volatile("dsb sy" ::: "memory");

	if (!native_ufs_log_write(ukeUfsCursor, UKE_UFS_RECORD_BLOCKS, ukeUfsBuffer)) {
		IOLog("UkeLastLog: userdata log write failed at record %llu\n",
		    (unsigned long long)ukeUfsSeq);
		return;
	}
	(void)native_ufs_log_sync();
	ukeUfsSeq++;
	ukeUfsCursor = (ukeUfsCursor + UKE_UFS_RECORD_BLOCKS) % ukeUfsBlocks;
	ukeUfsCursor -= ukeUfsCursor % UKE_UFS_RECORD_BLOCKS;
}
#endif /* UKE_LOG_USERDATA */

/* Panic-time staging in the pram: [magic][length][text]. It is only a hand-off:
 * the next boot's writer copies the text into the userdata log window (disk). */
#define UKE_PANIC_SAVE_MAGIC 0x31564153u
static char ukePanicText[1024];
static boolean_t ukePanicTextValid = FALSE;

/* The writers run on other CPUs while the panic screen is parked; let a
 * snapshot that started before the staging flag retire before overwriting
 * the block it is about to keep. */
static void
uke_panic_settle(void)
{
	uint64_t start, now;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r" (start));
	do {
		__asm__ volatile("mrs %0, cntvct_el0" : "=r" (now));
	} while (now - start < 1000000);
}

void
uke_panic_flush(const char *message)
{
	static volatile int flushing;
	const char prefix[] = "UKE PANIC: ";
	size_t used = sizeof(prefix) - 1;
	if (flushing || message == NULL || gPanicBase == 0 || gPanicSize < 8 + used) {
		return;
	}
	flushing = 1;
	ukePanicStaged = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	uke_panic_settle();
	const size_t capacity = (size_t)gPanicSize - 8;
	size_t length = strlen(message);
	if (length > capacity - used) {
		length = capacity - used;
	}
	uint32_t *header = (uint32_t *)(uintptr_t)gPanicBase;
	memcpy((char *)(header + 2), prefix, used);
	memcpy((char *)(header + 2) + used, message, length);
	header[1] = (uint32_t)(used + length);
	__asm__ volatile("dsb sy" ::: "memory");
	header[0] = UKE_PANIC_SAVE_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
	flushing = 0;
}

static void
uke_panic_restore(void)
{
	if (gPanicBase == 0 || gPanicSize < 8) {
		return;
	}
	uint32_t *header = (uint32_t *)(uintptr_t)gPanicBase;
	if (header[0] != UKE_PANIC_SAVE_MAGIC) {
		return;
	}
	const uint32_t length = header[1];
	if (length == 0 || (uint64_t)length > (uint64_t)gPanicSize - 8) {
		header[0] = 0;
		return;
	}
	const size_t copy = length < sizeof(ukePanicText) - 1 ? length : sizeof(ukePanicText) - 1;
	memcpy(ukePanicText, (const char *)(header + 2), copy);
	ukePanicText[copy] = '\0';
	ukePanicTextValid = TRUE;
	header[0] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
uke_last_log_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	uke_panic_restore();
	if (ukePanicTextValid) {
		IOLog("%s\n", ukePanicText);
		ukePanicTextValid = FALSE;
	}

	unsigned tick = 0;
	for (;;) {
		// The first seconds of msgbuf are flooded by boot logs; report the
		// vnode sizing once the window is long past the ring's early noise.
		if (tick == 240) {
			IOLog("UkeLastLog: vnodes desired=%d sane=%llu\n",
			    desiredvnodes, (unsigned long long)sane_size);
		}
		if (tick == 4) {
			/* Now that kprintf output reaches msgbuf, repeat the previous
			 * boot's panic text so it lands in the userdata window. */
			pe_print_last_panic_again();
		}
		uke_last_log_write();
		// The userdata copy is paced separately: the 32 MiB window only holds
		// 2048 records, so at the pram writer's 500 ms rate it would wrap every
		// seventeen minutes and discard the start of the boot, which is what
		// this copy exists to keep. Two seconds stretches the ring to about an
		// hour.
#if UKE_LOG_USERDATA
		if (ukeShengLog && ((tick % (UKE_UFS_INTERVAL_MS / UKE_LAST_LOG_INTERVAL_MS)) == 0)) {
			uke_ufs_publish();
		}
#endif
		tick++;
		IOSleep(UKE_LAST_LOG_INTERVAL_MS);
	}
}

/* A pram beacon of its own: this STARTUP and the USB one are the two places whose threads have
 * never produced observable output, so each says whether it ran at all in a slot the firmware's
 * printable-run scan can read on the next boot. 0xFF800000 is the /pram window the ADT declares. */
static void
uke_last_log_beacon(const char *text)
{
	static vm_offset_t va;
	char *slot;
	unsigned i = 0;

	if (va == 0) {
		va = ml_io_map_wcomb(0xFF800000ull, 0x2000);
		if (va == 0) {
			return;
		}
	}
	slot = (char *)(va + 0x1100);
	while (i < 63 && text[i] != '\0') {
		slot[i] = text[i];
		i++;
	}
	while (i < 64) {
		slot[i++] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Defined below, but called by the external entry above it: without this prototype the call is an
 * implicit declaration error, which is exactly how the last round's archive build failed. */
static void uke_last_log_start(void);

/* External entry, for the csr_check and bsd_init hooks: the STARTUP registration alone has never
 * produced observable output in this kernel. */
void
uke_last_log_hook_entry(const char *who)
{
	uke_last_log_beacon(who);
	uke_last_log_start();
}

static void
uke_last_log_start(void)
{
	static int started;
	thread_t thread;

	if (started) {
		// uke_last_log_hook_entry() is called from csr_check and from bsd_init, and the STARTUP()
		// registration is a third path: one writer thread is all this needs, and the first call that
		// gets here owns it.
		return;
	}
	started = 1;
	uke_last_log_beacon("UKELASTLOG-STARTUP-ENTERED");
	// The pram record runs unconditionally, and sheng-log no longer switches it on. Two reasons:
	// the boot argument does not fit (the command line is 998 of DarwinBoot's 1000 bytes, so a new
	// one costs a byte that is not there), and the record is what makes a kernel log readable without
	// a photograph - a diagnostic channel that has to be enabled by a boot arg is one that will be off
	// on the boot that mattered. sheng-log=1 now selects only the userdata (UFS) half below, which is
	// the experiment that reaches out to a controller before any kext runs.
#if UKE_LOG_USERDATA
	{
		uint32_t enabled = 0;

		ukeShengLog = PE_parse_boot_argn("sheng-log", &enabled, (int)sizeof(enabled)) && enabled == 1;
	}
#endif
	if (kernel_thread_start_priority(uke_last_log_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		uke_last_log_beacon("UKELASTLOG-THREAD-STARTED");
	} else {
		uke_last_log_beacon("UKELASTLOG-THREAD-FAILED");
	}
	// One snapshot here regardless of the thread, so a kernel that dies before the first tick still
	// leaves its log up to this point in the window.
	uke_log_snapshot("start");
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_last_log_start);
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_crash_log_start);
