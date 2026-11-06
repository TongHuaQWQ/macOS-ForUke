// SPDX-License-Identifier: MIT
//
// Keep kernel functions that the prelinked fileset calls by name from being
// inlined and dead-stripped in this build.
//
// The fileset (Apple's own kexts) reaches into the kernel by symbol: the
// prelinker matches each call site to a function in the rebuilt kernel and
// leaves a call to the panic stub when it cannot find one. `csr_check` is such a
// case — AMFI, apfs and sandbox call it 87 times — because LTO inlines every
// in-tree caller and `-dead_strip` then drops the out-of-line body, so the
// rebuilt kernel ships no `_csr_check` symbol at all and the call has nowhere to
// bind.
//
// Taking the address into a global that nothing optimises away keeps the body:
// the function is live because a live object points at it. The prototype must
// match bsd/sys/csr.h exactly, or LTO rejects the merged module for a type
// mismatch.
//
// Nothing calls through these pointers; they exist so the linker keeps the
// definition.

#include <stdint.h>

/* bsd/sys/csr.h: typedef uint32_t csr_config_t; int csr_check(csr_config_t) */
extern int csr_check(uint32_t mask);

int (*const uke_keep_csr_check) (uint32_t mask) = csr_check;
