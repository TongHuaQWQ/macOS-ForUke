// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Kernel side of package/UfsLog.cpp: the ten primitives that file declares, the two register windows it
// needs mapped, one DMA buffer, and one lock.
//
// The split is deliberate. Everything that can be written and checked without a kernel header lives in
// UfsLog.cpp, which this host compiles for aarch64-none-elf in a second. What is left here is only what
// genuinely needs the kernel: ml_io_map for uncached device memory, a physically contiguous allocation
// for the request list and the UPIU/PRDT area, and IOLock for the transport's per-command serialisation.
//
// Facts pinned from drivers/NativeUfs/GccUfs.h and NativeUfs.cpp:
//   host controller  0x01d84000 + 0x3000   (UFS_HC_BASE / UFS_HC_SIZE)
//   apps SMMU        0x15000000 + 0xd0000  (the window smmuBypassStream maps)
//   DMA layout       8192 + 131072 bytes   (kDataOffset + kDataMax, RootTransportLayout.h)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

// The IOKit entry points are declared here rather than by including <IOKit/IOLib.h>, and two things
// make that the right trade:
//
//   * the compile record that builds this file is a bsd unit's, so its -I set has no iokit/ in it.
//     Reaching IOLib.h therefore meant inventing an include path from the record, and getting that
//     wrong is what killed archive-18 on the first writer round ("cannot locate the xnu source root
//     above .../ios-obj/iokit/DEVELOPMENT"). Three headers that every kernel unit can already see
//     beat one that only some can.
//   * the declarations below are copied from iokit/IOKit/IOLib.h (251, 269, 643 and IOLocks.h:78), on
//     arm64, where IOPhysicalAddress is UInt64 -- so every call site in this file is still checked
//     against the real shapes, including the deprecated attribute on IOMallocContiguous.
typedef uint64_t IOPhysicalAddress;
typedef struct IOLock IOLock;
extern void  IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void  IODelay(unsigned microseconds);
extern void *IOMallocContiguous(uintptr_t size, uintptr_t alignment,
                                IOPhysicalAddress *physicalAddress)
  __attribute__((deprecated));
extern IOLock *IOLockAlloc(void);
extern void    IOLockLock(IOLock *lock);
extern void    IOLockUnlock(IOLock *lock);

#define UFSLOG_HC_BASE      0x01d84000ULL
#define UFSLOG_HC_SIZE      0x00003000ULL
#define UFSLOG_SMMU_BASE    0x15000000ULL
#define UFSLOG_SMMU_SIZE    0x000d0000ULL
#define UFSLOG_DMA_BYTES    (8192 + 131072)

struct UfsLogMaps {
  uintptr_t hc;
  uintptr_t smmu;
  uint8_t  *dma;
  uint64_t  dmaPhys;
  void     *lock;
};

extern int ufs_log_attach(const struct UfsLogMaps *maps, const char **why);

static uintptr_t gUfsLogHc;
static uintptr_t gUfsLogSmmu;
static void     *gUfsLogDma;
// IOPhysicalAddress, not uintptr_t: IOMallocContiguous takes IOPhysicalAddress* and on arm64 macOS that
// is `unsigned long long` while uintptr_t is `unsigned long`, which is an incompatible-pointer warning.
static IOPhysicalAddress gUfsLogDmaPhys;
static void     *gUfsLogLock;
static bool      gUfsLogStarted;
static const char *gUfsLogWhy = "not started";

// ---- the ten primitives ---------------------------------------------------------------------------

uint32_t
ufslog_read32(uintptr_t base, unsigned offset)
{
  return *(volatile uint32_t *)(base + offset);
}

void
ufslog_write32(uintptr_t base, unsigned offset, uint32_t value)
{
  *(volatile uint32_t *)(base + offset) = value;
}

// The DMA buffer is written by the CPU and read by the controller, so it has to be cleaned to the point
// of coherency before the doorbell rings. Cache-line granular, exactly as NativeUfs.cpp does it.
void
ufslog_cache_clean(const void *start, size_t length)
{
  uintptr_t address = (uintptr_t)start & ~(uintptr_t)63;
  uintptr_t end     = (uintptr_t)start + length;

  for (; address < end; address += 64) {
    __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

void
ufslog_cache_invalidate(const void *start, size_t length)
{
  uintptr_t address = (uintptr_t)start & ~(uintptr_t)63;
  uintptr_t end     = (uintptr_t)start + length;

  for (; address < end; address += 64) {
    __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

void
ufslog_barrier(void)
{
  __asm__ volatile("dsb sy" ::: "memory");
}

uint64_t
ufslog_absolute_time(void)
{
  return mach_absolute_time();
}

void
ufslog_delay(void)
{
  IODelay(2);
}

bool
ufslog_boot_arg_u32(const char *name, uint32_t *value)
{
  if (name == NULL || value == NULL) {
    return false;
  }
  return PE_parse_boot_argn(name, value, (int)sizeof(*value)) ? true : false;
}

void
ufslog_lock(void *lock)
{
  if (lock != NULL) {
    IOLockLock(lock);
  }
}

void
ufslog_unlock(void *lock)
{
  if (lock != NULL) {
    IOLockUnlock(lock);
  }
}

// ---- start ---------------------------------------------------------------------------------------

/**
  Map the two windows, allocate the transport buffer and attach.
  Idempotent: the log writer calls it on every retry tick and it costs a compare once it has succeeded.

  @param[out] why   short reason when it fails
  @return           1 when the transport is up and native_ufs_log_open can be called
**/
int
ufs_log_start(const char **why)
{
  struct UfsLogMaps maps;

  if (gUfsLogStarted) {
    return 1;
  }
  if (why != NULL) {
    *why = gUfsLogWhy;
  }

  if (gUfsLogHc == 0) {
    gUfsLogHc   = ml_io_map(UFSLOG_HC_BASE, UFSLOG_HC_SIZE);
    gUfsLogSmmu = ml_io_map(UFSLOG_SMMU_BASE, UFSLOG_SMMU_SIZE);
    if (gUfsLogHc == 0 || gUfsLogSmmu == 0) {
      gUfsLogWhy = "controller map";
      if (why != NULL) {
        *why = gUfsLogWhy;
      }
      return 0;
    }
  }

  if (gUfsLogDma == NULL || gUfsLogLock == NULL) {
    // Contiguous, because the transport hands the controller one physical address for the request list
    // and the data area. IOMallocContiguous is what the storage driver uses for the same reason.
    //
    // The declaration is marked deprecated (the replacement, IOBufferMemoryDescriptor, is an IOKit
    // object and this file deliberately has no IOKit object graph), so the one warning it produces is
    // silenced here rather than for the whole file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    gUfsLogDma = IOMallocContiguous(UFSLOG_DMA_BYTES, 4096, &gUfsLogDmaPhys);
#pragma clang diagnostic pop
    if (gUfsLogDma == NULL) {
      gUfsLogWhy = "dma buffer";
      if (why != NULL) {
        *why = gUfsLogWhy;
      }
      return 0;
    }
    if (gUfsLogLock == NULL) {
      gUfsLogLock = IOLockAlloc();
      if (gUfsLogLock == NULL) {
        gUfsLogWhy = "lock";
        if (why != NULL) {
          *why = gUfsLogWhy;
        }
        return 0;
      }
    }
  }

  maps.hc      = gUfsLogHc;
  maps.smmu    = gUfsLogSmmu;
  maps.dma     = (uint8_t *)gUfsLogDma;
  maps.dmaPhys = (uint64_t)gUfsLogDmaPhys;
  maps.lock    = gUfsLogLock;

  if (!ufs_log_attach(&maps, &gUfsLogWhy)) {
    if (why != NULL) {
      *why = gUfsLogWhy;
    }
    return 0;
  }

  gUfsLogStarted = true;
  gUfsLogWhy     = "";
  if (why != NULL) {
    *why = gUfsLogWhy;
  }
  return 1;
}
