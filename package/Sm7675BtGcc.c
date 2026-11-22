/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared GCC vote serialization for SM7675 QUPv3 wrap1 SE2 (Bluetooth UART10). */
#include "Sm7675GeniUart.h"
#include <IOKit/IOLocks.h>

static IOLock *bt_gcc_lock;
static uint32_t uart10_initial_bus;
static bool uart10_claimed;

static IOLock *get_bt_gcc_lock(void) {
    IOLock *lock = __atomic_load_n(&bt_gcc_lock, __ATOMIC_ACQUIRE);
    if (!lock) {
        IOLock *candidate = IOLockAlloc();
        if (!candidate) return NULL;
        if (__atomic_compare_exchange_n(&bt_gcc_lock, &lock, candidate, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            lock = candidate;
        } else {
            IOLockFree(candidate);
        }
    }
    return lock;
}

bool sm7675_bt_gcc_update32(volatile uint32_t *reg, uint32_t clear, uint32_t set) {
    if (!reg) return false;
    IOLock *lock = get_bt_gcc_lock();
    if (!lock) return false;
    IOLockLock(lock);
    *reg = (*reg & ~clear) | set;
    __asm__ volatile("dsb sy" ::: "memory");
    bool ok = (*reg & set) == set && (*reg & (clear & ~set)) == 0;
    IOLockUnlock(lock);
    return ok;
}

bool sm7675_geni_uart10_vote(volatile uint32_t *reg, bool enabled) {
    if (!reg) return false;
    IOLock *lock = get_bt_gcc_lock();
    if (!lock) return false;
    IOLockLock(lock);
    uint32_t value = *reg;
    if (!enabled && !uart10_claimed) {
        IOLockUnlock(lock);
        return true;
    }
    const uint32_t active = SM7675_GCC_QUP1_S2_BIT | SM7675_GCC_QUP1_BUS_MASK;
    if (enabled) {
        if (!uart10_claimed) {
            uart10_initial_bus = value & SM7675_GCC_QUP1_BUS_MASK;
            uart10_claimed = true;
        }
        value |= active;
    } else {
        value &= ~SM7675_GCC_QUP1_S2_BIT;
        if (!(value & SM7675_GCC_QUP1_OTHER_MASK)) {
            value &= ~(SM7675_GCC_QUP1_BUS_MASK & ~uart10_initial_bus);
        }
    }
    *reg = value;
    __asm__ volatile("dsb sy" ::: "memory");
    bool ok = (*reg & active) == (value & active);
    IOLockUnlock(lock);
    return ok;
}
