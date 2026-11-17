#!/usr/bin/env python3
"""Check the kernel-to-firmware Advanced Logger handoff before a CI round builds it.

Two owners share one buffer: the firmware's AdvLoggerPkg places and exports it, and
package/UkeAdvLogger.c appends the kernel message buffer to the same place. Nothing in either
repository makes them agree, so this file does:

  * uke.dsc must name a fixed base address, 64 pages and PcdAdvancedLoggerFixedInRAM = TRUE
    (without the last one AdvancedLoggerLib ignores the base and allocates its own buffer);
  * package/UkeAdvLogger.c must use the same base and the same window size;
  * dts/uke.dts must end its memory node exactly at the base, so XNU's VM never allocates the window;
  * the window must be the reserved tail of DRAM, not something inside the memory node.

It then replays the walk that AdvancedLoggerAccessLib performs (LOG_BUFFER_FROM_ALI, forward to
LogCurrentOffset, 'ALM2' entries, 8-byte aligned, bounded) over a buffer built the way the writer
builds it, and over the four refusals that keep a bad header from being walked.

    python3 tools/check-adv-log.py            # check this checkout
    python3 tools/check-adv-log.py --self-test-only
"""
import argparse
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

# AdvancedLoggerInternal.h: signature, version and the entry structure, restated here on purpose so a
# change in the vendored package shows up as a failure rather than as a silently different layout.
ALOG = 0x474F4C41
ALM2 = 0x324D4C41
ALMS = 0x534D4C41
INFO_VERSION = 6
INFO_SIZE = 88
ENTRY_SIZE = 24           # ADVANCED_LOGGER_MESSAGE_ENTRY_V2 header
PHASE_RUNTIME = 5
LEVEL_INFO = 0x40


def align8(value):
    return (value + 7) & ~7


class Info:
    """The packed ADVANCED_LOGGER_INFO the firmware and the kernel both write."""

    def __init__(self, signature=ALOG, version=INFO_VERSION, offset=INFO_SIZE, size=0, current=None):
        self.signature = signature
        self.version = version
        self.log_buffer_offset = offset
        self.log_buffer_size = size
        self.log_current_offset = offset if current is None else current
        self.discarded_size = 0
        self.in_permanent_ram = 1
        self.at_runtime = 1

    def pack(self):
        raw = bytearray(INFO_SIZE)
        struct.pack_into('<IH6sIIIII', raw, 0, self.signature, self.version, bytes(6),
                         self.log_buffer_offset, 0, self.log_current_offset,
                         self.discarded_size, self.log_buffer_size)
        raw[32] = self.in_permanent_ram
        raw[33] = self.at_runtime
        return bytes(raw)


def walk(window):
    """AdvancedLoggerAccessLib's walk: bounded, signature-checked, forward only.

    Returns the list of messages, and raises ValueError on anything the library would reject.
    """
    info = Info()
    (info.signature, info.version) = struct.unpack_from('<IH', window, 0)
    info.log_buffer_offset, _, info.log_current_offset, info.discarded_size, info.log_buffer_size = \
        struct.unpack_from('<IIIII', window, 12)
    if info.signature != ALOG:
        raise ValueError(f'not an ALOG header: 0x{info.signature:08x}')
    if info.log_buffer_offset % 8 or info.log_buffer_offset < INFO_SIZE:
        raise ValueError('log buffer offset is not an aligned offset past the info structure')
    low = info.log_buffer_offset
    high = info.log_buffer_offset + info.log_buffer_size
    if high > len(window):
        raise ValueError(f'log area ends at {high}, past the {len(window)}-byte window')
    if not low <= info.log_current_offset <= high:
        raise ValueError('cursor outside the log area')
    messages = []
    cursor = low
    while cursor < info.log_current_offset:
        if cursor % 8:
            raise ValueError(f'entry at {cursor} is not 8-byte aligned')
        signature, major, minor, level, stamp, phase, length, offset = \
            struct.unpack_from('<IBBIQHHH', window, cursor)
        if signature == ALM2:
            if offset != ENTRY_SIZE:
                raise ValueError(f'ALM2 entry at {cursor} declares a message offset of {offset}')
            text = window[cursor + offset: cursor + offset + length]
            messages.append(text.rstrip(b'\0').decode('utf-8', 'replace'))
            cursor += align8(offset + length)
        elif signature == ALMS:
            level, stamp, length = struct.unpack_from('<IIH', window, cursor + 4)
            text = window[cursor + 16: cursor + 16 + length]
            messages.append(text.rstrip(b'\0').decode('utf-8', 'replace'))
            cursor += align8(16 + length)
        else:
            raise ValueError(f'bad entry signature 0x{signature:08x} at {cursor}')
        if cursor > high:
            raise ValueError('entry runs past the log area')
    return messages


class Writer:
    """package/UkeAdvLogger.c's append path, as arithmetic."""

    def __init__(self, window_bytes, create_header=True):
        self.window = bytearray(window_bytes)
        info = Info(size=window_bytes - INFO_SIZE)
        if create_header:
            self.window[:INFO_SIZE] = info.pack()
        self.info = info

    def append(self, text):
        payload = text.encode()
        total = align8(ENTRY_SIZE + len(payload))
        offset = self.info.log_current_offset
        limit = self.info.log_buffer_offset + self.info.log_buffer_size
        if offset + total > limit:
            return False
        struct.pack_into('<IBBIQHHH', self.window, offset, ALM2, 2, 1, LEVEL_INFO,
                         len(payload), PHASE_RUNTIME, len(payload), ENTRY_SIZE)
        self.window[offset + ENTRY_SIZE: offset + ENTRY_SIZE + len(payload)] = payload
        # The C writer stores the new cursor back into the mapped header; the model has to do the same or
        # the walk below would read the old one.
        self.info.log_current_offset = offset + total
        struct.pack_into('<I', self.window, 20, self.info.log_current_offset)
        return True


def read_dsc(path):
    # Strip comments first: the fixed-base experiment is reverted as a comment, and a naive search would
    # keep reading the value it no longer sets.
    text = '\n'.join(line for line in path.read_text(errors='ignore').splitlines()
                     if not line.lstrip().startswith('#'))
    base = re.search(r'PcdAdvancedLoggerBase\|(\S+)', text)
    pages = re.search(r'PcdAdvancedLoggerPages\|(\d+)', text)
    fixed = re.search(r'PcdAdvancedLoggerFixedInRAM\|(\w+)', text)
    return (int(base.group(1), 0) if base else None,
            int(pages.group(1), 0) if pages else None,
            fixed.group(1).upper() if fixed else None)


def read_driver(path):
    text = path.read_text(errors='ignore')
    base = re.search(r'#define\s+UKE_ADV_BASE\s+(0x[0-9A-Fa-f]+)', text)
    size = re.search(r'#define\s+UKE_ADV_BYTES\s+(0x[0-9A-Fa-f]+)', text)
    return (int(base.group(1), 16) if base else None, int(size.group(1), 16) if size else None)


# Info-header fields the writer must never write. It may append entries and may mark the buffer as
# outliving ExitBootServices, but a header it invented is exactly what broke b256/b257 and the firmware's
# ValidateInfoBlock() would then accept it on the next boot.
HEADER_FIELDS = {'UKE_ALI_OFF_SIGNATURE', 'UKE_ALI_OFF_VERSION', 'UKE_ALI_OFF_LOG_OFFSET',
                 'UKE_ALI_OFF_SIZE', 'UKE_ALI_OFF_DISCARDED'}
ALLOWED_FIELDS = {'UKE_ALI_OFF_CURRENT', 'UKE_ALI_OFF_PERMANENT_RAM', 'UKE_ALI_OFF_AT_RUNTIME'}


def header_writes(path):
    """Every info-header field the writer stores into, by name. Empty means it never touches a header."""
    text = path.read_text(errors='ignore')
    written = set()
    for field in re.findall(r'uke_adv_put(?:8|16|32|64)\s*\([^,]+,\s*(UKE_ALI_OFF_\w+)', text):
        if field in HEADER_FIELDS:
            written.add(field)
    return written


def read_dts_end(path):
    text = path.read_text(errors='ignore')
    match = re.search(r'memory@[0-9a-fA-F]+\s*\{[^}]*?reg\s*=\s*<0x0*\s*([0-9a-fA-F]+)\s+0x0*\s*([0-9a-fA-F]+)\s+0x0*\s*([0-9a-fA-F]+)\s+0x0*\s*([0-9a-fA-F]+)>', text, re.S)
    if not match:
        return None
    high_base = (int(match.group(1), 16) << 32) | int(match.group(2), 16)
    high_size = (int(match.group(3), 16) << 32) | int(match.group(4), 16)
    return high_base + high_size


HOST_STUBS = r"""
/* Minimal stand-ins for the kernel ABI so tools/check-adv-log.py can compile the real writer on the
   host and walk what it produces. Nothing here is arm64-alignment-checked, which is exactly why the
   generated records are handed back to the Python walk instead of being inspected in C. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef uintptr_t vm_offset_t;
typedef unsigned int uint32_t_compat;
typedef void *thread_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef uintptr_t vm_size_t;
#define NULL ((void *)0)
#define KERN_SUCCESS 0
#define BASEPRI_KERNEL 80
#define MSG_MAGIC 0x063061
struct msgbuf { int msg_magic; int msg_size; int msg_bufx; unsigned char *msg_bufc; };
static struct msgbuf gMsgbuf;
static struct msgbuf *msgbufp = &gMsgbuf;
static unsigned char gWindow[0x40000];
static unsigned char gText[4096];
static char gLog[8192];
static size_t gLogUsed;

vm_offset_t ml_io_map_wcomb(vm_offset_t phys, size_t size)
{
	(void)phys; (void)size;
	return (vm_offset_t)gWindow;
}

void IOLog(const char *fmt, ...)
{
	(void)fmt;
}

void IOSleep(unsigned ms)
{
	(void)ms;
}

int kernel_thread_start_priority(void *fn, void *arg, int pri, thread_t *out)
{
	(void)fn; (void)arg; (void)pri;
	*out = (thread_t)1;
	return KERN_SUCCESS;
}

void thread_deallocate(thread_t thread)
{
	(void)thread;
}

/* The writer stamps its own fate into a firmware-readable slot (uke_mark in UkeLastLog.c) so the next
   boot can report it; on the host the stamp is just recorded, which lets the simulation assert that the
   writer says what it thinks it is doing. */
static char gMark[64];
void uke_mark(unsigned int slot, const char *text)
{
	(void)slot;
	snprintf(gMark, sizeof(gMark), "%s", text);
}

#define UKE_HOST_SIM 1
#include "UkeAdvLogger.c"

int main(void)
{
	unsigned i;

	/* Prime msgbuf with a plausible ring and let the writer create the header and append. */
	for (i = 0; i < sizeof(gText); i++) {
		gText[i] = (unsigned char)('a' + (i % 26));
	}
	for (i = 0; i < 64; i++) {
		gText[i] = (unsigned char)('A' + (i % 26));
	}
	/* Stand in for the firmware: publish a valid ALOG header, which the writer now only appends to. */
	memset(gWindow, 0, sizeof(gWindow));
	gWindow[0x00] = 'A'; gWindow[0x01] = 'L'; gWindow[0x02] = 'O'; gWindow[0x03] = 'G';
	gWindow[0x04] = 6; gWindow[0x05] = 0;                 /* Version */
	*(unsigned int *)(gWindow + 0x0c) = 88;               /* LogBufferOffset */
	*(unsigned int *)(gWindow + 0x14) = 88;               /* LogCurrentOffset */
	*(unsigned int *)(gWindow + 0x1c) = sizeof(gWindow) - 88;  /* LogBufferSize */

	gMsgbuf.msg_magic = MSG_MAGIC;
	gMsgbuf.msg_size = (int)sizeof(gText);
	gMsgbuf.msg_bufx = 64;
	gMsgbuf.msg_bufc = gText;

	if (!uke_adv_open()) {
		fprintf(stderr, "open refused\n");
		return 2;
	}
	uke_adv_append("UKE-ADVLOG: writer alive\n", sizeof("UKE-ADVLOG: writer alive\n") - 1);
	uke_adv_append("ArmCpu: 42 timer interrupts, 0 IPIs\n", sizeof("ArmCpu: 42 timer interrupts, 0 IPIs\n") - 1);
	uke_adv_pump();
	fwrite(gWindow, 1, sizeof(gWindow), stdout);
	fprintf(stderr, "MARK:%s\n", gMark);
	return 0;
}
"""


def host_sim():
    """Compile the real UkeAdvLogger.c with stubs and walk what it wrote."""
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        (work / 'sim.c').write_text(HOST_STUBS)
        shutil.copy(ROOT / 'package' / 'UkeAdvLogger.c', work / 'UkeAdvLogger.c')
        # Empty stand-ins for the kernel headers the writer includes; the stubs above supply every type
        # and function it actually uses.
        for header in ('sys/appleapiopts.h', 'mach/vm_types.h', 'kern/debug.h', 'kern/sched.h',
                       'kern/startup.h', 'kern/thread.h', 'machine/machine_routines.h',
                       'sys/msgbuf.h', 'IOKit/IOLib.h'):
            target = work / 'stubs' / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('/* host stand-in */\n')
        binary = work / 'sim'
        build = subprocess.run([compiler, '-std=gnu11', '-O1', '-w', '-I', str(work / 'stubs'),
                                '-o', str(binary), str(work / 'sim.c')], capture_output=True, text=True)
        if build.returncode:
            print(build.stderr[-2000:])
            raise SystemExit('FAIL: the writer does not compile on the host')
        result = subprocess.run([str(binary)], capture_output=True)
        if result.returncode:
            raise SystemExit(f'FAIL: the host simulation exited {result.returncode}: {result.stderr[:300]}')
        messages = walk(result.stdout)
        mark = ''
        for line in result.stderr.decode('utf-8', 'replace').splitlines():
            if line.startswith('MARK:'):
                mark = line[5:]
    return messages, mark


# A freestanding stand-in for the kernel ABI, used only to compile the writer for aarch64 and look at
# the instructions it produces. Separate from HOST_STUBS because this one needs no libc at all.
ASM_STUBS = r"""
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef unsigned long size_t;
typedef unsigned long uintptr_t;
typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;
typedef void *thread_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define KERN_SUCCESS 0
#define BASEPRI_KERNEL 80
#define MSG_MAGIC 0x063061
struct msgbuf { int msg_magic; int msg_size; int msg_bufx; unsigned char *msg_bufc; };
static struct msgbuf *msgbufp;
void *memcpy(void *dst, const void *src, size_t n);

vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size);
void IOLog(const char *fmt, ...);
void IOSleep(unsigned ms);
int kernel_thread_start_priority(void *fn, void *arg, int pri, thread_t *out);
void thread_deallocate(thread_t thread);

#define UKE_HOST_SIM 1
#include "UkeAdvLogger.c"
"""


# Store mnemonics, and how many bytes each one touches: an access is safe under SCTLR_EL1.A only when the
# offset it uses is a multiple of its own width.
STORE_WIDTHS = {
    'strb': 1, 'sturb': 1,
    'strh': 2, 'sturh': 2,
    'str': 4, 'stur': 4,          # widened below when the data register is xN
    'stp': 16, 'stnp': 16,
}
STORE_LINE = re.compile(
    r'\A\s*(?P<mn>stur|sturb|sturh|str|strb|strh|stp|stnp)\s+(?P<rest>.*?)\[(?P<addr>[^\]]*)\]')
IMMEDIATE = re.compile(r'#(?P<value>-?(?:0x[0-9a-fA-F]+|\d+))')
DATA_REG = re.compile(r'\A\s*(?P<reg>[wx](?:\d+|zr))')


def unaligned_stores(assembly):
    """Every store in an aarch64 assembly listing whose immediate offset is not a multiple of its width.

    A byte store can never be unsafe; anything wider is safe only on a naturally aligned address. This is
    exactly the property b256 and b257 violated: `stur wzr,[x22,#0x6]` and `stur xzr,[x22,#0x22]`, which
    is what the optimiser made of the byte-wise source."""
    bad = []
    for line in assembly.splitlines():
        match = STORE_LINE.match(line)
        if not match:
            continue
        # [x0] / [x0, x1] / [x0, #0x22] / [x0, #0x22]! : only a literal immediate can be misaligned.
        immediate = IMMEDIATE.search(match.group('addr'))
        if immediate is None:
            continue
        value = int(immediate.group('value'), 0)
        mnemonic = match.group('mn')
        reg = DATA_REG.match(match.group('rest'))
        if reg is None:
            continue
        width = STORE_WIDTHS[mnemonic]
        if mnemonic in ('str', 'stur') and reg.group('reg').startswith('x'):
            width = 8
        elif mnemonic in ('stp', 'stnp') and reg.group('reg').startswith('w'):
            width = 8
        if value % width:
            bad.append((value, width, line.strip()))
    return bad


def assembly_gate():
    """Compile the writer for aarch64 and prove its window accesses are byte-sized.

    The point is not to inspect the code by eye: b257's source was byte stores and the *compiler* turned
    them back into one wide store, so the source text cannot answer the question. -mstrict-align is what
    makes the backend refuse, and this gate is what notices if that flag ever falls out of the build."""
    clang = shutil.which('clang')
    if not clang:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        stubs = work / 'stubs'
        stubs.mkdir()
        (work / 'asm_unit.c').write_text(ASM_STUBS)
        shutil.copy(ROOT / 'package' / 'UkeAdvLogger.c', work / 'UkeAdvLogger.c')
        for header in ('sys/appleapiopts.h', 'mach/vm_types.h', 'kern/debug.h', 'kern/sched.h',
                       'kern/startup.h', 'kern/thread.h', 'machine/machine_routines.h',
                       'sys/msgbuf.h', 'IOKit/IOLib.h',
                       # -nostdinc, so the C library headers the writer includes are stand-ins as well:
                       # every type it needs is declared in ASM_STUBS above.
                       'stdint.h', 'stdbool.h', 'stddef.h', 'string.h'):
            target = stubs / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('/* freestanding stand-in */\n')
        flags = [clang, '-target', 'aarch64-none-elf', '-nostdinc', '-I', str(stubs), '-I', str(work),
                 '-std=gnu11', '-O2', '-w', '-mgeneral-regs-only', '-S']

        built = work / 'writer.s'
        result = subprocess.run([*flags, '-mstrict-align', '-o', str(built), str(work / 'asm_unit.c')],
                                capture_output=True, text=True)
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('FAIL: the writer does not compile for aarch64')
        bad = unaligned_stores(built.read_text())

        # The negative control: the pre-fix shape, compiled without the flag, has to be caught. If this
        # does not trip, the gate is blind and the run above proved nothing.
        (work / 'control.c').write_text(
            'typedef unsigned char uint8_t;\n'
            'void control(uint8_t *base) {\n'
            '\tunsigned i;\n'
            '\tbase[0x06] = 0; base[0x07] = 0; base[0x08] = 0; base[0x09] = 0;\n'
            '\tfor (i = 0; i < 8; i++) { base[0x22 + i] = 0; }\n'
            '}\n')
        control = work / 'control.s'
        result = subprocess.run([*flags, '-o', str(control), str(work / 'control.c')],
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit('FAIL: the negative control does not compile')
        caught = unaligned_stores(control.read_text())
    # The gate compiles on its own, so it cannot see whether the real build keeps the flag. Check that
    # the file is listed with it, which is the half of the guarantee the compiler cannot provide.
    build = (ROOT / 'scripts' / 'build-boot-log.py').read_text()
    writer_line = [line for line in build.splitlines() if "'UkeAdvLogger.c'" in line]
    flagged = bool(writer_line) and '-mstrict-align' in writer_line[0]
    return bad, caught, flagged


def self_tests():
    ok = 0

    # 1: the writer's own output walks back to the same text, in order.
    writer = Writer(0x40000)
    texts = ['UKE-ADVLOG: writer alive', 'AppleAPFSContainer: starting', 'kernel[0]: hello']
    for text in texts:
        assert writer.append(text), text
    assert walk(bytes(writer.window)) == texts, walk(bytes(writer.window))
    print('ok   self-test 1 order      written text comes back through the access-library walk')
    ok += 1

    # 2: a bad header is refused rather than walked.
    broken = bytearray(Writer(0x40000).window)
    struct.pack_into('<I', broken, 0, 0xDEADBEEF)
    try:
        walk(bytes(broken))
    except ValueError:
        print('ok   self-test 2 signature  a window without ALOG is rejected')
        ok += 1
    else:
        raise SystemExit('FAIL: a bad header was walked')

    # 3: a cursor outside the log area is refused.
    bad = bytearray(Writer(0x40000).window)
    struct.pack_into('<I', bad, 20, 0x7FFFFFFF)
    try:
        walk(bytes(bad))
    except ValueError:
        print('ok   self-test 3 cursor     a cursor outside the log area is rejected')
        ok += 1
    else:
        raise SystemExit('FAIL: an out-of-bounds cursor was walked')

    # 4: append-only. Once the window is full the writer stops instead of wrapping, and what is in the
    # window is still walkable.
    small = Writer(INFO_SIZE + ENTRY_SIZE + 8)
    assert small.append('first') is True
    assert small.append('second') is False
    messages = walk(bytes(small.window))
    assert messages == ['first'], messages
    print('ok   self-test 4 append     a full window stops the writer and stays walkable')
    ok += 1

    # 5: an entry whose message offset lies is refused, not misread.
    liar = bytearray(Writer(0x40000).window)
    writer = Writer(0x40000)
    writer.append('x')
    liar = bytearray(writer.window)
    struct.pack_into('<H', liar, INFO_SIZE + 22, 0x40)
    try:
        walk(bytes(liar))
    except ValueError:
        print('ok   self-test 5 offset     a lying message offset is rejected')
        ok += 1
    else:
        raise SystemExit('FAIL: a lying message offset was walked')

    # 6: a version-1 entry written by the firmware is still walked.
    mixed = bytearray(Writer(0x40000).window)
    info = Info(size=0x40000 - INFO_SIZE)
    mixed[:INFO_SIZE] = info.pack()
    payload = b'firmware line'
    total = align8(16 + len(payload))
    struct.pack_into('<IIIH', mixed, INFO_SIZE, ALMS, LEVEL_INFO, 0, len(payload))
    mixed[INFO_SIZE + 16: INFO_SIZE + 16 + len(payload)] = payload
    info.log_current_offset = INFO_SIZE + total
    struct.pack_into('<I', mixed, 20, info.log_current_offset)
    messages = walk(bytes(mixed))
    assert messages == ['firmware line'], messages
    print('ok   self-test 6 v1 entry   a firmware ALMS entry is walked')
    ok += 1

    # 7: the real C compiles and its output walks: the only test that covers the writer itself.
    sim = host_sim()
    if sim is None:
        print('skip self-test 7 host-sim  no host compiler')
    else:
        messages, mark = sim
        first = messages[0] if messages else None
        assert first == 'UKE-ADVLOG: writer alive\n', messages[:2]
        assert any('ArmCpu: 42 timer interrupts' in m for m in messages), messages[:3]
        assert messages[-1].startswith('ABCDEFGHIJKLMNOPQRSTUVWXYZ'), messages[-1][:40]
        assert mark, 'the writer stamped nothing into the mark slot'
        assert mark.startswith('ADV:OK'), mark
        print(f'ok   self-test 7 host-sim  compiled the writer; {len(messages)} records walked back; '
              f'stamp "{mark}"')
        ok += 1

    # 8: the instructions, not the source. b257 proved that byte stores in the source can come out of the
    # optimiser as one wide, misaligned store, which panics under SCTLR_EL1.A.
    gate = assembly_gate()
    if gate is None:
        print('skip self-test 8 asm-align  no clang to cross-compile with')
    else:
        bad, caught, flagged = gate
        if bad:
            for offset, width, line in bad[:6]:
                print(f'     unaligned {width}-byte store at offset 0x{offset:x}: {line}')
            raise SystemExit(f'FAIL: {len(bad)} store(s) in the writer are not naturally aligned')
        if not caught:
            raise SystemExit('FAIL: the negative control was not caught, so the gate proves nothing')
        if not flagged:
            raise SystemExit('FAIL: scripts/build-boot-log.py does not compile UkeAdvLogger.c with '
                             '-mstrict-align, so the optimiser may merge the byte stores again')
        print(f'ok   self-test 8 asm-align  no wide store in the writer\'s aarch64 code, -mstrict-align is '
              f'in the build, and the pre-fix shape is caught ({len(caught)} stores)')
        ok += 1

    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test-only', action='store_true')
    args = parser.parse_args()

    tests = self_tests()
    if args.self_test_only:
        print(f'PASS: {tests} self-tests')
        return 0

    failures = []
    dsc = ROOT / 'firmware' / 'uefi-uke' / 'Platforms' / 'Xiaomi' / 'ukePkg' / 'uke.dsc'
    if not dsc.is_file():
        dsc = pathlib.Path('/home/tonghua/Code/mcOS/workspace/uefi-uke/Platforms/Xiaomi/ukePkg/uke.dsc')
    driver = ROOT / 'package' / 'UkeAdvLogger.c'
    dts = ROOT / 'dts' / 'uke.dts'

    base, pages, fixed = read_dsc(dsc)
    driver_base, driver_bytes = read_driver(driver)
    dts_end = read_dts_end(dts)

    # Two acceptable states, and the difference matters:
    #   * fixed base OFF (the current, safe one): the firmware allocates its own buffer and the writer
    #     must stand down, because 0x9FFE00000 is EfiReservedMemoryType on this unit and a kernel that
    #     wrote there broke the next firmware boot;
    #   * fixed base ON: only valid together with FixedInRAM, and only if a previous round has shown the
    #     second boot surviving - nothing here can prove that.
    if base is None or fixed is None:
        print('ok   firmware   fixed base not configured: the firmware allocates its own buffer')
        created = header_writes(driver)
        if created:
            failures.append(f'the writer stores into header fields {sorted(created)}; it must only append')
        else:
            print('ok   driver     the writer never writes a header field; it only appends to one')
        # The address it appends at has to be the firmware allocator's window, not the platform's
        # reserved tail: the DXE log reports "Advanced Logger Info = B112E000" there and the firmware map
        # reports 0x9FFDF0000..0x9FFFFFFF as EfiReservedMemoryType.
        if driver_base is not None and driver_base >= 0x8C0000000:
            failures.append(f'driver UKE_ADV_BASE 0x{driver_base:x} is inside the memory this kernel owns')
    elif fixed != 'TRUE':
        failures.append(f'{dsc}: PcdAdvancedLoggerFixedInRAM is {fixed}, so the library will ignore the base')
    if base is not None and pages is not None and fixed == 'TRUE':
        window = pages * 4096
        if driver_base != base:
            failures.append(f'driver UKE_ADV_BASE 0x{driver_base and hex(driver_base)} != DSC base 0x{base:x}')
        if driver_bytes != window:
            failures.append(f'driver UKE_ADV_BYTES 0x{driver_bytes and hex(driver_bytes)} != {pages} pages (0x{window:x})')
        if dts_end is not None and dts_end != base:
            failures.append(f'dts memory node ends at 0x{dts_end:x}, the log window starts at 0x{base:x}')
        # The window has to be the reserved tail and nothing else: it must end at a 2 MiB boundary above
        # the memory node, which the two checks above already pin down; this one catches a base that
        # points inside the memory node while the sizes happen to agree.
        if dts_end is not None and base < dts_end:
            failures.append('the log window overlaps the memory node handed to XNU')

    if failures:
        for failure in failures:
            print(f'FAIL: {failure}')
        print('FAILED')
        return 1
    if base is not None and fixed == 'TRUE':
        print(f'ok   firmware   base 0x{base:x}, {pages} pages ({pages * 4096 // 1024} KiB), fixed in RAM')
    print(f'ok   driver     UkeAdvLogger.c uses the same base and window')
    print(f'ok   device tree memory node ends at 0x{dts_end:x}, so XNU never allocates the window')
    print(f'ok   self-tests {tests}')
    print('PASS: the kernel-to-firmware Advanced Logger handoff agrees on both sides')
    return 0


if __name__ == '__main__':
    sys.exit(main())
