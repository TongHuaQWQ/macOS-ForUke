/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The board half of drivers/QcomDwc3/QcomDwc3.cpp: the two things that driver expects a board to supply and that are not part
 * of the controller itself.
 *
 * Why the names say sm8550: they are the driver's own imports, taken verbatim from the sheng board (UniXNU-sheng,
 * kits/Sm8550Platform/Sm8550Gcc.c) this driver was first written for. Renaming them here would put a five-line diff between
 * the vendored driver and its upstream, and would hide that the two boards supply the same three hooks. The implementation
 * below is this board's, and it is generic: a GCC register update is a locked read-modify-write that then proves it landed,
 * which every Cliffs and SM8550 clock controller takes the same way.
 *
 * sm8550_gcc_update32 is the driver's only way to release a GDSC or a branch clock in the GCC block: gdsc() and branch() in
 * QcomDwc3.cpp pass (register, clear, set) and fail the bring-up stage when the readback does not match. The lock is what keeps
 * that read-modify-write atomic against the other clock owners in this kernel (the display and GPU probes share the same block).
 * The apps-SMMU pair backs AppsSmmuLock.h, the ordering between the UFS stream and the USB stream; one IOLock, taken once. */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdbool.h>
#include <stdint.h>

static IOLock *gGccLock;
static IOLock *gSmmuLock;

static IOLock *ukeLock(volatile void *slot)
{
    IOLock *candidate;
    for (;;) {
        IOLock *held = __atomic_load_n ((IOLock *const volatile *) slot, __ATOMIC_ACQUIRE);
        if (held != NULL) { return held; }
        candidate = IOLockAlloc ();
        if (candidate == NULL) { return NULL; }
        if (__atomic_compare_exchange_n ((IOLock *volatile *) slot, &held, candidate, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            return candidate;
        }
        /* Another caller published first: its lock is the one, and this allocation becomes garbage. Leaking it would be a
         * per-boot 48-byte loss for a race that happens at most twice; freeing it makes the caller's slot aliasing auditable. */
        IOLockFree (candidate);
    }
}

bool sm8550_gcc_update32 (volatile uint32_t *reg, uint32_t clear, uint32_t set)
{
    IOLock *lock;
    bool ok;
    if (reg == NULL) { return false; }
    lock = ukeLock (&gGccLock);
    if (lock == NULL) { return false; }
    IOLockLock (lock);
    *reg = (*reg & ~clear) | set;
    __asm__ volatile ("dsb sy" ::: "memory");
    /* Both halves of the request are read back: the bits asked for are set and the cleared ones that were not re-set are gone. */
    ok = (*reg & set) == set && (*reg & (clear & ~set)) == 0;
    IOLockUnlock (lock);
    return ok;
}

bool sm8550_apps_smmu_lock (void)
{
    IOLock *lock = ukeLock (&gSmmuLock);
    if (lock == NULL) { return false; }
    IOLockLock (lock);
    return true;
}

void sm8550_apps_smmu_unlock (void)
{
    IOLock *lock = __atomic_load_n (&gSmmuLock, __ATOMIC_ACQUIRE);
    if (lock != NULL) { IOLockUnlock (lock); }
}
