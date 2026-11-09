// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// A UFS writer for the kernel message log, built without the verified-root storage component.
//
// Why this file exists
// --------------------
// `package/UkeLastLog.c` keeps two copies of the message buffer: one in the reserved pram window, which
// dies with the next reset, and one appended to the last 32 MiB of userdata, which survives a crash and
// can be read from recovery with dd. The second copy is behind UKE_LOG_USERDATA and calls three symbols:
//
//     native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why)
//     native_ufs_log_write(uint64_t relBlock, uint32_t blocks, const void *src)
//     native_ufs_log_sync(void)
//
// Those are implemented in drivers/NativeUfs/NativeUfs.cpp, in the same file as the verified root volume,
// and that file cannot be built for this profile: the verified half needs a signed policy directory that
// does not exist for 25F84, and the file as a whole is written against the iPadOS 17 IOStorageFamily
// headers and does not compile against the 26 train (board.toml records that wall).
//
// The transport, however, is IOKit-free. RootCommands.h, SmmuTakeover.h, TransferPoll.h and
// RootTransportLayout.h contain the request-list and SCSI layer with no IOKit types at all -- the
// driver's own tests compile them for a bare-metal target. So this file is that transport plus the
// kernel-side glue it needs, and nothing else: it serves no root volume, it appends log records.
//
// What it does NOT do: read, parse or authenticate the root image. The write window is the last
// kLogReserveBlocks blocks of userdata, its bounds arrive as boot arguments (board.toml knows them from
// the tablet's own GPT), and every write is checked against that window again inside hostWrite4k.

#include <stdint.h>
#include <stddef.h>

// ---- kernel-facing glue ---------------------------------------------------------------------------
//
// The real definitions live in UfsLogKernel.c, which is compiled with the kernel headers. Keeping them
// apart is what lets this file be compiled and tested on the host with clang --target=aarch64-none-elf,
// the same way drivers/NativeUfs/tests/ does it.
extern "C" {
uint32_t ufslog_read32(uintptr_t base, unsigned offset);
void ufslog_write32(uintptr_t base, unsigned offset, uint32_t value);
void ufslog_cache_clean(const void *start, size_t length);
void ufslog_cache_invalidate(const void *start, size_t length);
void ufslog_barrier(void);
uint64_t ufslog_absolute_time(void);
void ufslog_delay(void);
bool ufslog_boot_arg_u32(const char *name, uint32_t *value);
// Physical window for the request list and the UPIU/PRDT area, and the controller base. The kernel glue
// maps them; this file only uses the virtual addresses it is handed.
uint8_t *ufslog_map(uint64_t phys, uint64_t bytes);
}

// ---- the shim the transport headers expect --------------------------------------------------------
// Taken from drivers/NativeUfs/tests/RootCommandsArm64.cpp, which is the same shim the driver's own
// bare-metal test uses.
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 0xe00002bc,
                   kIOReturnBadArgument = 0xe00002c2, kIOReturnIOError = 0xe00002ca,
                   kIOReturnBusy = 0xe00002d5, kIOReturnTimeout = 0xe00002d6,
                   kIOReturnNotReady = 0xe00002d8;
constexpr unsigned kMicrosecondScale = 1, kMillisecondScale = 1000;

static uint32_t read32(uintptr_t base, unsigned offset) { return ufslog_read32(base, offset); }
static void write32(uintptr_t base, unsigned offset, uint32_t value) { ufslog_write32(base, offset, value); }
static void cacheClean(const void *start, size_t length) { ufslog_cache_clean(start, length); }
static void cacheInvalidate(const void *start, size_t length) { ufslog_cache_invalidate(start, length); }
static void cacheCivac(const void *start, size_t length)
{
  ufslog_cache_clean(start, length);
  ufslog_cache_invalidate(start, length);
}
static uint64_t mach_absolute_time(void) { return ufslog_absolute_time(); }
static void IODelay(unsigned) { ufslog_delay(); }
static void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
  *deadline = mach_absolute_time() + uint64_t(interval) * uint64_t(scale);
}
// RootDiagnostics.h formats its one-line state dump with snprintf; the kernel glue supplies the kernel's.
extern "C" int snprintf(char *out, size_t capacity, const char *format, ...);
extern "C" void ufslog_lock(void *lock);
extern "C" void ufslog_unlock(void *lock);
static void IOLockLock(void *lock) { ufslog_lock(lock); }
static void IOLockUnlock(void *lock) { ufslog_unlock(lock); }
// Freestanding, like the driver's own bare-metal harness: no libc in the kernel link.
extern "C" void *memcpy(void *out, const void *in, size_t n)
{
  auto d = static_cast<uint8_t *>(out); auto s = static_cast<const uint8_t *>(in);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
  return out;
}
extern "C" void *bzero(void *out, size_t n)
{
  auto d = static_cast<uint8_t *>(out);
  for (size_t i = 0; i < n; ++i) d[i] = 0;
  return out;
}

#include "RootDiagnostics.h"

struct Host {
  uint8_t *virt;
  uint64_t phys;
  bool ready, poisoned;
  uintptr_t hc;
  uintptr_t smmu;
  uintptr_t cb;
  void *lock;
};
static Host gHost;
static UfsRootDiagnostics gRootDiagnostic;

// The two helpers NativeUfs.cpp defines outside its transport headers; RootCommands.h calls both.
static void hostWrite(unsigned offset, uint32_t value)
{
  ufslog_barrier();
  write32(gHost.hc, offset, value);
  ufslog_barrier();
}

static IOReturn recordTransport(IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = uint32_t(result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gHost.hc) {
    gRootDiagnostic.hcs = read32(gHost.hc, 0x30);
    gRootDiagnostic.hce = read32(gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32(gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32(gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t(read32(gHost.hc, 0x50)) | (uint64_t(read32(gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32(gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32(gHost.hc, 0x18);
  }
  return result;
}

// The transport headers: request list, SCSI command construction, the two 4096-byte windows and the
// window check every write goes through.
#include "RootTransportLayout.h"
#include "TransferPoll.h"
#include "RootCommands.h"

// ---- the log window ------------------------------------------------------------------------------

constexpr uint64_t kLogWindowBlocks = 8192;   // 32 MiB, the same window board.toml records
// gLogFirst / gLogBlocks belong to RootCommands.h: hostLogOpen() sets them and hostWrite4k() checks every
// write against them, so this file reads them instead of keeping its own copy.

extern "C" int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  uint32_t first = 0, last = 0;

  if (!ufslog_boot_arg_u32("sheng-log-first-lba", &first) ||
      !ufslog_boot_arg_u32("sheng-log-last-lba", &last)) {
    reason = "no geometry";
  } else if (last <= first || (uint64_t(last) - first + 1) < kLogWindowBlocks) {
    reason = "partition too small";
  } else if (gLogBlocks) {
    reason = "already open";
  } else {
    // The window is the last kLogWindowBlocks of userdata, counted inclusively: the same first block the
    // recovery-side dd reads (last_lba - 8192 + 1).
    uint64_t start = uint64_t(last) + 1 - kLogWindowBlocks;
    if (!hostLogOpen(start, kLogWindowBlocks)) {
      reason = "window refused";
    } else {
      gLogFirst = start;
      gLogBlocks = kLogWindowBlocks;
      if (firstLba) *firstLba = start;
      if (blocks) *blocks = kLogWindowBlocks;
      ok = 1;
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogBlocks || !src || !blocks || rel >= gLogBlocks || blocks > gLogBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? uint32_t(kDataMax / 4096) : blocks;
    if (hostWrite4k(0, gLogFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *>(src) + uint64_t(chunk) * 4096;
    rel += chunk;
    blocks -= chunk;
  }
  ufslog_barrier();
  return 1;
}

extern "C" int native_ufs_log_sync(void)
{
  if (!gLogBlocks) return 0;
  return hostNop() == kIOReturnSuccess;
}
