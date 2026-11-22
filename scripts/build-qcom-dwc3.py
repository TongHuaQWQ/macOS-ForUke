#!/usr/bin/env python3
"""Build libQcomDwc3.a: the native DWC3 subclasses of Apple's USB device and host controllers.

What goes in
------------
  drivers/QcomDwc3/QcomDwc3.cpp   QcomDwc3Xdci (AppleUSBXDCI) and QcomDwc3Xhci (AppleUSBXHCI)
  package/UsbProbe.cpp            the board entry: uke_usb_probe() loads the two system kexts, then
                                  registers the subclasses (ApplexxxXDCI has no personality, so nothing
                                  else ever loads it)
  package/GeniI2c.c               the retimer's I2C engine (PS5169 on the board's GENI bus)
  package/QcomRpmh.c              the AOP command database and one active-only RPMh request
  package/QcomDwc3Platform.c      the two board hooks the driver imports (GCC register update, apps-SMMU
                                  lock); see that file for why they keep the driver's sm8550_ names

Why the ABI headers are generated here instead of shipped
---------------------------------------------------------
The driver derives from two classes whose interfaces are private: only the original collection knows
their vtable. `scripts/usb-xdci-26C5035r.json` / `usb-xhci-26C5035r.json` record that vtable slot by
slot — target and pointer-authentication diversity — as read out of *the kernelcache this board
prelinks against* (26C5035r, sha256 in every fixture). This script turns each fixture into the base
class the driver compiles against: one declaration per slot, named so that clang's own
ptrauth_string_discriminator for the mangled name equals that slot's recorded diversity.

The kernel is arm64e, so that equality is not cosmetic: a vtable entry signed with a different
discriminator traps the moment an original caller authenticates it. The check below therefore reads the
*compiled* object back — both vtables, entry by entry — and stops with the slot number when a signature
does not land on the recorded diversity. A wrong fixture is a build failure here, not a fault at
bring-up on the device.

What it cannot check
--------------------
That the classes match at run time: whether IOUSBDeviceFamily publishes, whether UCSI answers, whether
the PHY sequence suits this board. Stage numbers for that come from `native_usb_stats`, which
package/UsbProbe.cpp prints after bring-up.

Linking
-------
configs/uke-macos_defconfig force-loads build/qcom-dwc3/libQcomDwc3.a through
CONFIG_XNU_EXTRA_LD_ARCHIVES, so every object here reaches the kernel link whether or not anything
references it — `uke_usb_probe` is called from package/LegacyDisplayProbe.cpp with weak_import, and a
runtime boot argument (`usb=1`) decides whether it does anything.

Running it without a runner
---------------------------
Only the generation half is machine-independent:

    python3 scripts/build-qcom-dwc3.py --check-headers

writes both headers into a temporary directory and checks the fixture invariants (slot count, macros
naming pure slots, the two fixtures agreeing on the kernelcache). Everything after that needs this
tree's own compile records and Xcode, so it only runs on the macOS builder.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/qcom-dwc3'
RECORDS = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
# C++ units take a .cpo record when the tree has one. The C units go to an *osfmk* record, not a bsd one:
# package/QcomDwc3Platform.c includes <IOKit/IOLib.h> and <IOKit/IOLocks.h>, and a bsd record's include set
# has no iokit/ on it -- the wall build-ufs-log.py records ("a bsd record has no iokit/ on its include
# path and deriving one from the record cost the first writer round"). osfmk/DEVELOPMENT/startup.o.json is
# the record package/UkeLastLog.c already compiles through, and that unit includes <IOKit/IOLib.h> too.
CXX_RECORD = f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
CXX_RECORD_ALTERNATES = (f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.o.json',
                         f'{RECORDS}/libkern/DEVELOPMENT/OSObject.cpo.json',
                         f'{RECORDS}/libkern/c++/DEVELOPMENT/OSObject.cpo.json',
                         f'{RECORDS}/osfmk/DEVELOPMENT/startup.cpo.json')
C_RECORD = f'{RECORDS}/osfmk/DEVELOPMENT/startup.o.json'
C_RECORD_ALTERNATES = (f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.o.json',
                       f'{RECORDS}/bsd/DEVELOPMENT/bsd_init.o.json',
                       f'{RECORDS}/pexpert/DEVELOPMENT/pe_kprintf.o.json')
# What the archive must define for the force-load to be more than dead weight.
EXPORTS = ('qcom_dwc3_bring_up', 'qcom_dwc3_publish', 'qcom_dwc3_diag', 'qcom_dwc3_host_load',
           'qcom_dwc3_vbus', 'uke_usb_probe', 'native_usb_stats')


def run(command, **kw):
    return subprocess.run([str(c) for c in command], check=True, **kw)


def capture(command, **kw):
    return subprocess.run([str(c) for c in command], check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, **kw).stdout


def tool(name):
    """A binutils tool from the platform LLVM, wherever this runner keeps it.

    Xcode's toolchain carries llvm-objcopy and llvm-ar; Homebrew's llvm does too. The kernel link itself
    uses Xcode's, so preferring the same bin directory keeps the objects and the archive in one flavour.
    """
    found = shutil.which(name)
    if found:
        return Path(found)
    probes = [['xcrun', '-f', name], ['brew', '--prefix', 'llvm']]
    for probe in probes:
        try:
            out = capture(probe).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        candidate = Path(out) / name if probe[0] == 'brew' else Path(out)
        if candidate.is_file():
            return candidate
    for base in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin',
                 '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin'):
        candidate = Path(base) / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f'{name} not found (PATH, xcrun, Homebrew LLVM and the Xcode toolchain all missed)')


def module(relative, marker):
    """A source directory, from the build tree first and the device checkout second.

    feeds.toml registers this board's QcomDwc3 as `local = "drivers/QcomDwc3"`, so a build tree that
    resolved the feed has it at <tree>/drivers/QcomDwc3. The device checkout is handed to every archive
    step as UNIXNU_DEVICE and is the fallback for a tree built before the feed entry existed.
    """
    device = os.environ.get('UNIXNU_DEVICE')
    candidates = [ROOT / relative] + ([Path(device) / relative] if device else [])
    if relative.startswith('drivers/'):
        candidates.append(ROOT / 'kits/uke' / relative.split('/', 1)[1])
    for candidate in candidates:
        if (candidate / marker).is_file():
            return candidate
    raise SystemExit(f'cannot find {relative} ({marker}); probed '
                     + ', '.join(str(c) for c in candidates))


def fixture(name):
    """The reviewed ABI fixture: next to this script, then in the device checkout, then in the tree."""
    here = Path(__file__).resolve().with_name(name)
    device = os.environ.get('UNIXNU_DEVICE')
    candidates = [here, ROOT / 'scripts' / name]
    if device:
        candidates.insert(1, Path(device) / 'scripts' / name)
    for candidate in candidates:
        if candidate.is_file():
            return json.loads(candidate.read_text()), candidate
    raise SystemExit(f'cannot find the {name} ABI fixture; probed '
                     + ', '.join(str(c) for c in candidates))


def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


# ---------------------------------------------------------------------------------------------------
# The generated base class
# ---------------------------------------------------------------------------------------------------

# The IOService-level overrides both originals re-declare. They are IOService's own signatures, so they
# do not move between OS builds; the slot each lands in does, and that is what base_slots records.
SERVICE_OVERRIDES = ('void free() override;\n'
                     'bool init(OSDictionary *properties=nullptr) override;\n'
                     'IOReturn setProperties(OSObject *) override;\n')


def header(name, fixture_data, extra_overrides):
    """One base class: macros for the slots the driver overrides, then a declaration per original slot."""
    slots = [(int(t, 16), int(d, 16)) for t, d in fixture_data['slots']]
    methods = {int(k): v for k, v in fixture_data['methods'].items()}
    macros = {int(k): v for k, v in fixture_data['macros'].items()}
    pure_target = int(fixture_data['pure_target'], 16)
    pure = {i for i, (target, _) in enumerate(slots) if target == pure_target}
    base = fixture_data.get('base_slots', 168)
    for slot in macros:
        if slot < base:
            raise SystemExit(f'{name}: macro slot {slot} is below base_slots {base}, so the driver would '
                             f'be renaming an IOService method it inherits read-only')
    # Every abstract slot has to be implemented or the native class cannot be instantiated at all; a macro
    # on a concrete slot is the other kind of override, the two the driver documents (no transport object
    # means no configurations; the device stack asks for no power in host mode).
    abstract_without_override = sorted(pure - set(macros))
    if abstract_without_override:
        raise SystemExit(f'{name}: abstract slots {abstract_without_override} have no native override')
    concrete = sorted(set(macros) - pure)
    for slot in range(base, len(slots)):
        if slot not in methods:
            raise SystemExit(f'{name}: no signature for original slot {slot}')
    text = ['// Generated by scripts/build-qcom-dwc3.py from the reviewed ABI fixture. Do not edit.\n'
            '//\n'
            f'// The original class this driver derives from, as slot {base}..{len(slots) - 1} of the\n'
            '// prelinked collection\'s own vtable. Each declaration is named so that its mangled name\n'
            '// authenticates with that slot\'s recorded diversity, which is what lets an original caller\n'
            '// call the native override. Slots the driver overrides are reached through the macros below.\n'
            '#pragma once\n'
            '#include <IOKit/IOService.h>\n\n']
    for slot, macro in sorted(macros.items()):
        text.append(f'#define {macro} {methods[slot]["name"]}\n')
    text.append(f'\nclass {name} : public IOService {{\n'
                f' OSDeclareAbstractStructors({name})\n'
                f' unsigned char originalPrivate[{fixture_data["instance_size"]}-sizeof(IOService)];\n'
                'public:\n'
                + SERVICE_OVERRIDES + extra_overrides)
    for slot in range(base, len(slots)):
        method = methods[slot]
        abstract = '=0' if slot in pure else ''
        text.append(f' virtual {method["returns"]} {method["name"]}({method["parameters"]}){abstract};\n')
    text.append(f'}};\n'
                f'static_assert(sizeof({name})=={fixture_data["instance_size"]},"Original controller instance size");\n')
    return ''.join(text), slots, pure, base, concrete


XDCI_OVERRIDES = ('bool start(IOService *) override;\n'
                  'void stop(IOService *) override;\n'
                  'bool terminate(IOOptionBits options=0) override;\n'
                  'IOWorkLoop *getWorkLoop() const override;\n'
                  'IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;\n'
                  'IOReturn setPowerState(unsigned long,IOService *) override;\n'
                  'unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;\n'
                  'unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;\n'
                  'IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;\n'
                  'IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;\n')

# The host controller re-declares those plus the open/close and property surface its family calls, so the
# native vtable reaches the original overrides of each. XHCI only ever runs in host mode here, and the
# driver's own overrides are the three macros.
XHCI_OVERRIDES = ('bool start(IOService *) override;\n'
                  'void stop(IOService *) override;\n'
                  'bool didTerminate(IOService *,IOOptionBits,bool *) override;\n'
                  'void systemWillShutdown(IOOptionBits) override;\n'
                  'void registerService(IOOptionBits options=0) override;\n'
                  'bool open(IOService *,IOOptionBits options=0,void *arg=nullptr) override;\n'
                  'void close(IOService *,IOOptionBits options=0) override;\n'
                  'bool handleOpen(IOService *,IOOptionBits,void *) override;\n'
                  'void handleClose(IOService *,IOOptionBits) override;\n'
                  'bool handleIsOpen(const IOService *) const override;\n'
                  'bool terminate(IOOptionBits options=0) override;\n'
                  'bool matchPropertyTable(OSDictionary *,SInt32 *) override;\n'
                  'bool attach(IOService *) override;\n'
                  'IOWorkLoop *getWorkLoop() const override;\n'
                  'IOReturn message(UInt32,IOService *,void *arg=nullptr) override;\n'
                  'void PMstop() override;\n'
                  'IOReturn setPowerState(unsigned long,IOService *) override;\n'
                  'unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;\n'
                  'unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;\n'
                  'IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;\n'
                  'IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;\n')


# ---------------------------------------------------------------------------------------------------
# Mach-O: the compiled vtable, which is the only place the diversities can be read
# ---------------------------------------------------------------------------------------------------

# Flags that make clang emit an LLVM bitcode wrapper instead of an object. CONFIG_XNU_BUILD_LTO=y makes the
# compile records carry them, and the archive this script produces is native on purpose: it is force-loaded
# into the same link as the UFS log and the component archives, which are native too.
BITCODE_FLAGS = ('-flto', '-emit-llvm', '-fembed-bitcode', '-fembed-bitcode-marker')


def is_bitcode_flag(value):
    return value in BITCODE_FLAGS or value.startswith(('-flto=', '-fembed-bitcode'))


def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise SystemExit('not an arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', data, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va,
                                 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


def object_symbols(data):
    commands = macho(data)['commands']
    symtab = next(p for c, p, _ in commands if c == 2)
    symbols, count, strings, _ = struct.unpack_from('<4I', data, symtab + 8)
    found = []
    for i in range(count):
        index, kind, _, _, va = struct.unpack_from('<IBBHQ', data, symbols + i * 16)
        name = data[strings + index:data.index(b'\0', strings + index)].decode()
        found.append((name, va, kind))
    return found


def object_vtable(data, found, name, count):
    """[(symbol, signed word)] for the first count entries of vtable name (authenticated relocations)."""
    va = next(v for n, v, _ in found if n == name)
    entries = {}
    for segment in macho(data)['segments']:
        for section in segment['sections']:
            if section['va'] <= va < section['va'] + section['size']:
                rebase, relocs = struct.unpack_from('<2I', data, section['command_offset'] + 56)
                for i in range(relocs):
                    offset, bits = struct.unpack_from('<2I', data, rebase + i * 8)
                    # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        word = struct.unpack_from('<Q', data, section['fileoff'] + offset)[0]
                        entries[section['va'] + offset] = (found[bits & 0xffffff][0], word)
    return [entries.get(va + 16 + i * 8) for i in range(count)]


def verify_controller(label, original_class, compiled, slots, pure, base):
    """Every entry carries the original slot's diversity, and only the original's concrete slots are bound."""
    vtable = f'__ZTV{len(label)}{label}'
    original_prefix = f'{len(original_class)}{original_class}'
    if len(compiled) != len(slots) or any(entry is None for entry in compiled):
        raise SystemExit(f'{vtable}: the object does not carry {len(slots)} authenticated entries')
    for slot, entry in enumerate(compiled):
        name, word = entry
        signed = word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0
        if not signed or (word >> 32) & 0xffff != slots[slot][1]:
            raise SystemExit(f'{vtable} slot {slot} ({name}) authenticates with {((word >> 32) & 0xffff):#x}; '
                             f'the original slot uses {slots[slot][1]:#x}')
        owned = name.startswith((f'__ZN{len(label)}{label}', f'__ZNK{len(label)}{label}'))
        original = original_prefix in name
        if original and slot in pure:
            raise SystemExit(f'{vtable} slot {slot} ({name}) binds an abstract original slot')
        if not owned and not original and slot >= base:
            raise SystemExit(f'{vtable} slot {slot} ({name}) is neither the native class nor an original '
                             f'slot, but it sits above the IOService surface ({base})')


# ---------------------------------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------------------------------

def load_record(candidates, what):
    for name in candidates:
        if (ROOT / name).is_file():
            return name
    found = sorted((path.relative_to(ROOT).as_posix()
                    for path in (ROOT / RECORDS).rglob('*.json')
                    if path.name.endswith(('.o.json', '.cpo.json'))),
                   key=lambda name: (0 if name.endswith('.cpo.json') else 1, name))
    if found:
        print(f'{what}: no record at an expected path, using {found[0]}')
        return found[0]
    raise SystemExit(f'no compile record for the {what} unit under {RECORDS}; '
                     'the kernel pass must run before the archives')


def compile_unit(source, record_name, obj, extra, language):
    """The record's own command line with our source, our -o, our language and the flags this port needs.

    The language is forced *in front of* the source rather than appended: `-x` applies to the inputs that
    follow it, and clang says so ("'-x assembler' after last input file has no effect") while happily
    compiling a `.S` file as C -- which is how the second CI round failed at `usb-bindings.S:1:1:
    expected identifier or '('` on the `.text` directive. The record's own `-x` pair is dropped for the
    same reason: it describes the unit the record was taken from, not the one we are compiling.
    """
    record = json.loads((ROOT / record_name).read_text().strip().rstrip(','))
    argument_list, command, index, dropped = record['arguments'], [], 0, []
    while index < len(argument_list):
        value = argument_list[index]
        if value == record['file']:
            command += ['-x', language, str(source)]
        elif value == '-x':
            dropped.append(value + ' ' + argument_list[index + 1])
            index += 1
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(obj)]
            index += 1
        elif value == '-Xclang' and index + 1 < len(argument_list) and argument_list[index + 1].startswith('-W'):
            index += 1
        elif is_bitcode_flag(value):
            dropped.append(value)
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command += extra
    command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-unused-function', '-Wno-deprecated-declarations', '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=900)
    (OUT / (obj.stem + '-compile-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    (OUT / (obj.stem + '-compile.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{source.name} failed to compile:\n{result.stdout[-6000:]}')
    if '-Werror' not in command:
        errors = [line for line in result.stdout.splitlines() if re.search(r'(^|: )error: ', line)]
        if errors:
            raise SystemExit(f'{source.name} produced errors:\n' + '\n'.join(errors[:40]))
    if dropped:
        # Reported, not silent: "the archive is native although the record is an LTO unit" is the kind of
        # difference that has to be visible in the log when the next link behaves unexpectedly.
        print(f'{source.name}: compiled as {language} (record carried {", ".join(sorted(set(dropped)))})')
    return hashlib.sha256(obj.read_bytes()).hexdigest()


def read_object(path, what):
    """A compiled unit, refusing anything but a native arm64 Mach-O object.

    The record this port compiles with is an *LTO* unit when CONFIG_XNU_BUILD_LTO is on, so the same
    command line also produces a bitcode wrapper (magic 0x0b17c0de) instead of an object -- which the
    first CI round reported only as "not an arm64 Mach-O object". Every archive force-loaded into this
    kernel link is native (the UFS log, the CDC-ACM probe, the component archives), so the bitcode flags
    are dropped above and the file is checked here, with the magic spelled out when it is not an object.
    """
    data = path.read_bytes()
    if len(data) < 32:
        raise SystemExit(f'{what}: {path} is {len(data)} bytes, not an object')
    magic = struct.unpack_from('<I', data)[0]
    if magic == 0x0b17c0de:
        raise SystemExit(f'{what}: {path} is an LLVM bitcode wrapper (the compile kept -flto)')
    if magic != 0xfeedfacf:
        raise SystemExit(f'{what}: {path} starts with {magic:#010x}, not a Mach-O 64 magic')
    macho(data)   # cputype/width, and the load-command sanity macho() checks
    return data


def symbols(archive):
    defined, undefined = set(), set()
    for line in capture([tool('llvm-nm'), '-g', archive]).splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        kind, name = fields[-2], fields[-1]
        (undefined if kind == 'U' else defined).add(name.lstrip('_'))
    return defined, undefined


# Itanium parameter codes for the C types the fixtures carry. A reviewed slot's generated name reproduces the
# original declaration's diversity only if both the SipHash above and this reproduction are right, so the
# check below proves the pair on every reviewed slot instead of trusting it on the first binding emitted.
PARAMETER_CODES = {'': '', 'void': 'v', 'void *': 'Pv', 'uint32_t': 'j', 'uint64_t': 'y', 'bool': 'b', 'IOReturn': 'i'}


def check_discriminators(original_class, data):
    """Every reviewed method name must authenticate with its own slot's recorded diversity."""
    slots = [(int(t, 16), int(d, 16)) for t, d in data['slots']]
    checked, skipped = 0, []
    for key, method in data['methods'].items():
        types = [t.strip() for t in method['parameters'].split(',')] if method['parameters'] else []
        if any(kind not in PARAMETER_CODES for kind in types):
            skipped.append(int(key))
            continue
        suffix = ''.join(PARAMETER_CODES[kind] for kind in types) or 'v'
        name = method['name']
        got = discriminator(f'_ZN{len(original_class)}{original_class}{len(name)}{name}E{suffix}')
        if got != slots[int(key)][1]:
            raise SystemExit(f'{original_class} slot {key} ({name}) authenticates with {got:#x}, but the '
                             f'fixture recorded {slots[int(key)][1]:#x}: the fixture and this hashing disagree')
        checked += 1
    return checked, skipped


def controllers():
    """The two fixtures, in the order the driver's classes appear: (header stem, original class, …)."""
    xdci, xdci_path = fixture('usb-xdci-26C5035r.json')
    xhci, xhci_path = fixture('usb-xhci-26C5035r.json')
    if xdci['kernelcache_sha256'] != xhci['kernelcache_sha256']:
        raise SystemExit('the two fixtures were read from different kernelcaches')
    if xdci['collection_slide'] != xhci['collection_slide']:
        raise SystemExit('the two fixtures record different collection slides')
    return [(('UsbXdci', 'AppleUSBXDCI', xdci, XDCI_OVERRIDES, xdci_path),
             ('UsbXhci', 'AppleUSBXHCI', xhci, XHCI_OVERRIDES, xhci_path)),
            xdci['kernelcache_sha256'], xdci['collection_slide']]


def check_headers(out):
    """The machine-independent half: fixture invariants and both headers, no toolchain."""
    entries, kernelcache, slide = controllers()
    for stem, original_class, data, overrides, path in entries:
        text, slots, pure, base, concrete = header(original_class, data, overrides)
        (out / f'Original{stem}ABI.h').write_text(text)
        checked, skipped = check_discriminators(original_class, data)
        print(f'{path.name}: {len(slots)} slots ({base} IOService + {len(slots) - base} original), '
              f'{len(pure)} abstract, {len(data["macros"])} native overrides '
              f'({len(concrete)} of them concrete), instance {data["instance_size"]}, '
              f'{checked} names re-derive their diversity'
              + (f', {len(skipped)} skipped (unsupported parameter types)' if skipped else ''))
    print(f'kernelcache {kernelcache[:16]}…, collection slide {slide}')
    return entries, kernelcache, int(slide, 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check-headers', action='store_true',
                        help='generate both ABI headers into a temporary directory and stop (no toolchain)')
    arguments = parser.parse_args()
    if arguments.check_headers:
        with tempfile.TemporaryDirectory() as directory:
            check_headers(Path(directory))
        print('headers: OK')
        return

    driver = module('drivers/QcomDwc3', 'QcomDwc3.cpp')
    kits = module('kits/uke', 'UsbProbe.cpp')
    print(f'driver: {driver}\nkits:   {kits}')
    OUT.mkdir(parents=True, exist_ok=True)
    entries, kernelcache, slide = check_headers(OUT)
    cxx_record = load_record((CXX_RECORD, *CXX_RECORD_ALTERNATES), 'C++')
    c_record = load_record((C_RECORD, *C_RECORD_ALTERNATES), 'C')

    objects = []
    common = ['-I', str(OUT), '-I', str(driver), '-I', str(kits)]
    # The kernel's own include roots, named here as well as carried by the record: load_record() can fall
    # back to a unit whose include set does not cover IOKit, and three of these units include it. The XNU
    # checkout sits under .cache/xnu-work in a prepared tree (that is the path the sheng board's own builder
    # uses) and at the tree root in a tree that was built in place, so both spellings are probed.
    kernel_includes = []
    for base in (ROOT / '.cache/xnu-work', ROOT):
        for name in ('osfmk', 'iokit', 'libkern', 'bsd'):
            if (base / name).is_dir():
                kernel_includes += ['-I', str(base / name)]
    objects.append(OUT / 'QcomDwc3.o')
    compile_unit(driver / 'QcomDwc3.cpp', cxx_record, OUT / 'QcomDwc3.o', common, 'c++')
    compile_unit(kits / 'UsbProbe.cpp', cxx_record, OUT / 'UsbProbe.o', common, 'c++')
    objects.append(OUT / 'UsbProbe.o')
    for unit in ('GeniI2c.c', 'QcomRpmh.c', 'QcomDwc3Platform.c'):
        compile_unit(kits / unit, c_record, OUT / (Path(unit).stem + '.o'),
                     kernel_includes + ['-I', str(kits)], 'c')
        objects.append(OUT / (Path(unit).stem + '.o'))

    # Every unit is a native object, and the driver's two vtables are read out of it.
    for path in objects:
        read_object(path, path.name)
    raw = read_object(OUT / 'QcomDwc3.o', 'QcomDwc3.cpp')
    found = object_symbols(raw)
    bindings = {}
    for stem, original_class, data, _overrides, _path in entries:
        label = 'QcomDwc3' + stem[3:]                     # UsbXdci -> QcomDwc3Xdci
        slots = [(int(t, 16), int(d, 16)) for t, d in data['slots']]
        pure = {i for i, (target, _) in enumerate(slots) if target == int(data['pure_target'], 16)}
        base = data.get('base_slots', 168)
        entries_of = object_vtable(raw, found, f'__ZTV{len(label)}{label}', len(slots))
        verify_controller(label, original_class, entries_of, slots, pure, base)
        # The fixture's own four (constructors, metaclass, vtable) come first, then every original entry
        # the native vtable reaches: clang emits those as imports of the original method, and one import
        # is bound to its slot's original address.
        for name, va in data['bindings'].items():
            bindings[name] = int(va, 16) + slide
        mangled = f'{len(original_class)}{original_class}'
        imported = {name for name, _va, kind in found if kind == 1}
        # The vtable is the authority on which slot an import belongs to: when this class leaves a slot to
        # the base implementation, clang names that entry after the *base class we declared*, e.g.
        # AppleUSBXDCI::setPowerState, while the original slot's diversity belongs to whatever introduced
        # the method (IOUSBDeviceController in this collection). A diversity lookup alone therefore misses
        # every inherited slot and the kernel link fails with exactly those names undefined -- which is what
        # run 43892185873 reported. Reading the slot index off the compiled vtable cannot miss them.
        for slot, (name, _word) in enumerate(entries_of):
            if name in imported and mangled in name:
                address = slots[slot][0] + slide
                if bindings.setdefault(name, address) != address:
                    raise SystemExit(f'{name}: one original address per symbol')
        # Anything the vtables do not name is bound by its own diversity, which covers a direct call such as
        # AppleUSBXHCI::someMethod() that never passes through a slot.
        by_diversity = {d: i for i, (_, d) in enumerate(slots)}
        for name in imported:
            if mangled not in name or name in bindings:
                continue
            slot = by_diversity.get(discriminator(name[1:]))
            if slot is None:
                continue
            address = slots[slot][0] + slide
            if bindings.setdefault(name, address) != address:
                raise SystemExit(f'{name}: one original address per symbol')
    # Nothing may reach the kernel link still undefined: the archive's own imports of the two original
    # classes are exactly what the stubs above exist to answer, and a missed one is a link failure twenty
    # minutes later instead of a build failure here (run 43892185873 lost a round to that).
    original_imports = {name for name in imported if '12AppleUSBXDCI' in name or '12AppleUSBXHCI' in name}
    uncovered = sorted(original_imports - set(bindings))
    if uncovered:
        raise SystemExit('original-class imports with no binding (the kernel link would fail):\n  '
                         + '\n  '.join(uncovered[:20]))
    print(f'bindings: {len(bindings)} ({len(original_imports)} original-class imports, all covered)')
    # Names reached as data (a metaclass pointer, the vtable itself): absolute, no stub.
    data_only = {'__ZN12AppleUSBXDCI10gMetaClassE', '__ZTV12AppleUSBXDCI',
                 '__ZN12AppleUSBXHCI10gMetaClassE', '__ZTV12AppleUSBXHCI'}
    # A trampoline per bound method, and the address it jumps to *baked as data* instead of branched to
    # directly. That is not a style choice: `b <name>.original` where the symbol is an absolute .set makes the
    # linker encode a PC-relative branch to an address it then also moves with the section, and the branch
    # lands one __TEXT_EXEC relocation (0x4610000 on this board) past Apple's function. The 2026-11-22 usb13
    # boot measured exactly that: the first Apple call of this driver, `AppleUSBXDCI::AppleUSBXDCI(OSMetaClass
    # const*)`, jumped to 0xfffffff00f7da6a0 while the fixture binds it to 0xfffffff00b1ca6a0, and the
    # difference is the section's own shift. Data pointers do not go through that path -- `&gMetaClass` read
    # back the right Apple metaclass in the same boot -- so the trampoline loads its target from a literal and
    # branches through a register, which is the representation that is already proven correct.
    # The trampoline's target lives in the *same* section the trampolines already use. Two builds that
    # introduced a section of their own into this archive (run 44929858813, which instantiated an abstract
    # Apple class and so dragged Apple's abstract vtable in, and the first version of this change, which put
    # the literals in __DATA_CONST) both failed the kernel link with six storage symbols undefined --
    # _mockfs_*, _corecrypto_kext_start and the two _native_verified_block_* -- while builds that only
    # changed code inside __text linked. Nothing here needs a section of its own, so nothing here adds one.
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in data_only:
            # Absolute, reached through GOT entries: a plain pointer the emitter neither signs nor moves.
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += (f'.globl {name}\n{name}:\n bti c\n'
                     f' adrp x16, {name}.slot@PAGE\n ldr x16, [x16, {name}.slot@PAGEOFF]\n br x16\n'
                     f'.p2align 3\n{name}.slot:\n .quad {name}.original\n.p2align 2\n')
    (OUT / 'usb-bindings.S').write_text(text)
    (OUT / 'usb-addresses.S').write_text(addresses)
    for stem in ('usb-bindings', 'usb-addresses'):
        compile_unit(OUT / f'{stem}.S', c_record, OUT / f'{stem}.o', [], 'assembler-with-cpp')
    objects += [OUT / 'usb-bindings.o', OUT / 'usb-addresses.o']

    # The static initialisers must not run before uke_usb_probe asks for them (the driver registers its
    # classes only after the hardware answered), so they leave __mod_init_func and the one constructor
    # UsbProbe.cpp calls by name becomes global.
    late = OUT / 'QcomDwc3.late.o'
    run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
         '--globalize-symbol=__GLOBAL__sub_I_QcomDwc3.cpp', OUT / 'QcomDwc3.o', late])
    objects = [late if o == OUT / 'QcomDwc3.o' else o for o in objects]

    archive = OUT / 'libQcomDwc3.a'
    archive.unlink(missing_ok=True)
    run([tool('llvm-ar'), 'rcs', archive, *objects])
    defined, undefined = symbols(archive)
    missing = [name for name in EXPORTS if name not in defined]
    if missing:
        raise SystemExit(f'{archive.name} does not define {", ".join(missing)}')
    report = {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'archive_bytes': archive.stat().st_size,
              'objects': [o.name for o in objects],
              'bindings': len(bindings),
              'kernelcache_sha256': kernelcache,
              'collection_slide': hex(slide),
              'slots': {stem: len(data['slots']) for stem, _class, data, _o, _p in entries},
              'compile_records': {'cxx': cxx_record, 'c': c_record},
              'exports': list(EXPORTS),
              'undefined': sorted(undefined)}
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'QcomDwc3: {len(bindings)} original bindings, {len(objects)} objects, '
          f'{archive.stat().st_size} bytes, sha {report["archive_sha256"][:16]}')


if __name__ == '__main__':
    main()
