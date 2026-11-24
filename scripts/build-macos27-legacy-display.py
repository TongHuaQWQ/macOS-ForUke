#!/usr/bin/env python3
"""Bind the owned GOP backend to the original IOMobileFramebufferLegacy of the macOS 27.3 (26C5035r) fileset.

Adapted for Xiaomi Pad 7 (uke) from UniXNU-sheng/scripts/build-gop-legacy-display-27.py.
Produces build/native-legacy-display/libNativeLegacyDisplay.late.a and build-report.json with
NativeLegacyDisplay, NativeGOPPresenter, LegacyDisplayProbe, LegacyDisplaySupport27,
KeyStoreProbe, and NativeKeyStoreEvents.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from kernel_target import ARCH, TRIPLE

BUILD = os.environ.get('UNIXNU_IPSW_BUILD') or '26C5035r'
MACOS = (BUILD == '26C5035r')
if ARCH != 'arm64e' or BUILD != '26C5035r':
    raise SystemExit(f'build-macos27-legacy-display.py requires arm64e 26C5035r (got {ARCH} {BUILD}).')

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

ABI_PATH = Path(__file__).resolve().with_name(f'legacy-display-{BUILD}.json')
abi = json.loads(ABI_PATH.read_text())
BASE = abi.get('base_slots', 272)
slide = int(abi['collection_slide'], 16)
slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
D, R, N = abi['display'], abi['request'], abi.get('notify', {})


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


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


PROTOTYPES = abi['prototypes']
by_slot = {slot: name for name, slot in abi['methods'].items()}
PURE_TARGET = int(abi['pure_target'], 16) if 'pure_target' in abi else slots[214][0]
pure = {i for i, (t, _) in enumerate(slots) if t == PURE_TARGET}
renames = []
for base, (name, suffix) in abi['names'].items():
    mangled = f'_ZN25IOMobileFramebufferLegacy{len(name)}{name}{suffix}'
    slot = abi['methods'].get(base) or int(base[len('opaque'):])
    require(discriminator(mangled) == slots[slot][1], f'Reviewed name {name} has the diversity of slot {slot}')
    renames.append(f'#define {base} {name}\n')
leaves = ' '.join(f'LEAF({i})' for i in abi['leaves'])

MACOS_CONSTANTS = (
    '#define NATIVE_LEGACY_MACOS 1\n'
    f"static constexpr unsigned kLegacyCurrentSwap={D['current']};\n"
    f"static constexpr unsigned kRequestDropReasons={R.get('drop_reasons', 0)},kRequestSwapInfoA={R.get('swap_info_a', 0)},kRequestSwapInfoB={R.get('swap_info_b', 0)};\n"
    f"static constexpr unsigned kLegacySwapInfoTime={D.get('swap_info_time', 0)},kLegacySwapInfoHeld={D.get('swap_info_held', 0)};\n"
    f"static constexpr unsigned kNotifyAsyncReference={N.get('async_reference', 0)},kNotifyMask={N.get('mask', 0)},kNotifyLastTime={N.get('last_time', 0)};\n"
) if MACOS else ''

header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the original macOS 27.3 ({BUILD}) class, generated from {ABI_PATH.name}.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
extern "C" OSObject *native_legacy_default_surface_create(OSObject *,uint32_t,uint32_t);
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={D['vsync_clients']};
static constexpr unsigned kRequestRect0={R['rect0']},kRequestRect1={R['rect1']},kRequestMasks={R['masks']},kRequestOptions={R['options']},kRequestDims={R['dims']},kRequestSurfaces={R['surfaces']};
#define NATIVE_LEGACY_LEAVES(LEAF) {leaves}
#define NATIVE_LEGACY_DEFAULT_SURFACE 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
{MACOS_CONSTANTS}static constexpr unsigned kLegacyDefaultSurface={D['default_surface']};
{''.join(renames)}class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{abi['instance_size']}-sizeof(IOService)];
public:
 bool installDefaultSurface(uint32_t width,uint32_t height) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  OSObject *root=nullptr,*surface=nullptr;
  memcpy(&surface,self+kLegacyDefaultSurface,sizeof(surface));
  if(surface)return true;
  memcpy(&root,self+{D['surface_root']},sizeof(root));
  if(!root)return false;
  surface=native_legacy_default_surface_create(root,width,height);
  if(!surface)return false;
  memcpy(self+kLegacyDefaultSurface,&surface,sizeof(surface));
  return true;
 }}
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{D['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{D['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{D['head']},sizeof(request));return request;
 }}
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{D['current']},8);memcpy(&head,self+{D['head']},8);memcpy(&tail,self+{D['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{R['next']},8);memcpy(&previous,bytes+{R['prev']},8);
  if(previous!=self+{D['head']})return false;
  if(next) {{
   const void *link=self+{D['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{R['prev']},&link,8);
  }} else if(tail!=bytes+{R['next']})return false;
  memcpy(self+{D['current']},&request,8);
  if(next) {{memcpy(self+{D['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{D['head']};memcpy(self+{D['head']},&empty,8);memcpy(self+{D['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(BASE, len(slots)):
    if i in by_slot:
        header += f' virtual {PROTOTYPES[by_slot[i]]};\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={abi["instance_size"]},"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')


def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
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
                sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va, 'size': length,
                                 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
entries = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                    word = struct.unpack_from('<Q', raw, sec['fileoff'] + offset)[0]
                    entries[sec['va'] + offset] = (symbols[bits & 0xffffff][0], word)
compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
require(all(compiled) and vtable + 16 + len(slots) * 8 not in entries, f'Exact owned {len(slots)}-entry vtable')
for i, (name, word) in enumerate(compiled):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
            f'Slot {i} ({name}) signed with IA, address diversity and the original diversity {slots[i][1]:#x}')
family = range(*(int(x, 16) for x in abi['family_text']))
bindings = {name: int(va, 16) + slide for name, va in abi['bindings'].items()}
owned, native = [], []
for i, (name, _) in enumerate(compiled):
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, f'No unresolved original abstract target at slot {i}')
        require(bindings.get(name, slots[i][0] + slide) == slots[i][0] + slide, 'One original address per name')
        bindings[name] = slots[i][0] + slide
    else:
        require(i < BASE and slots[i][0] not in family, f'Slot {i} ({name}) is inherited unchanged by the original family')
        native.append(i)
require(pure <= set(owned), 'Every original abstract slot is implemented by the owned backend')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined <= set(bindings), 'Every original-class import is bound: ' + ', '.join(sorted(undefined - set(bindings))))

asm = OUT / 'original-bindings.S'
DATA = {'__ZN25IOMobileFramebufferLegacy10gMetaClassE', '__ZTV25IOMobileFramebufferLegacy'}
text, addresses = '.text\n.p2align 2\n', ''
for name, va in sorted(bindings.items()):
    target = f'{va:#x}'
    if name in DATA:
        addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
    else:
        addresses += f'.globl {name}.original\n.set {name}.original, {target}\n'
        text += (f'.globl {name}\n{name}:\n bti c\n'
                 f' adrp x16, {name}.slot@PAGE\n ldr x16, [x16, {name}.slot@PAGEOFF]\n br x16\n'
                 f'.p2align 3\n{name}.slot:\n .quad {name}.original\n.p2align 2\n')
asm.write_text(text)
(OUT / 'original-addresses.S').write_text(addresses)
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])
run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', OUT / 'original-addresses.S', '-o', OUT / 'original-addresses.o'])
llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
late = OUT / 'NativeLegacyDisplay.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, late])

r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/uke/LegacyDisplayProbe.cpp'


def compile_cpp(src, dst, extra=()):
    cmd, args, i = [], r['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == r['file']:
            cmd.append(str(src))
        elif value == '-o':
            cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MF', '-MT', '-MJ'):
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-I', str(ROOT / '.cache/xnu-work/osfmk'), *extra]
    run(cmd, cwd=r['directory'])


compile_cpp(source, OUT / 'LegacyDisplayProbe.o')
support = OUT / 'LegacyDisplaySupport27.cpp'
support.write_text('''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generated by build-macos27-legacy-display.py.
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" OSObject *native_legacy_surface_create(OSObject *,void *,OSDictionary *);
extern "C" IOReturn native_legacy_surface_allocate(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" OSObject *native_legacy_default_surface_create(OSObject *root,uint32_t width,uint32_t height) {
    if(!root || !width || !height || width>16384 || height>16384)return nullptr;
    const uint64_t stride=(uint64_t(width)*4+127)&~uint64_t(127);
    const uint64_t bytes=(stride*height+4095)&~uint64_t(4095);
    OSDictionary *properties=OSDictionary::withCapacity(6);
    if(!properties)return nullptr;
    const char *keys[]={"IOSurfaceWidth","IOSurfaceHeight","IOSurfaceBytesPerRow",
                        "IOSurfaceBytesPerElement","IOSurfacePixelFormat","IOSurfaceAllocSize"};
    const uint64_t values[]={width,height,stride,4,0x42475241,bytes};
    bool valid=true;
    for(unsigned i=0;i<6 && valid;++i) {
        OSNumber *number=OSNumber::withNumber(values[i],64);
        valid=number && properties->setObject(keys[i],number);
        if(number)number->release();
    }
    OSObject *surface=valid?native_legacy_surface_create(root,nullptr,properties):nullptr;
    properties->release();
    if(!surface){printf("uke-fb: default surface factory failed\\n");return nullptr;}
    IOReturn status=native_legacy_surface_allocate(surface);
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
    valid=status==kIOReturnSuccess && memory && memory->getLength()>=bytes &&
          native_legacy_surface_width(surface)==width && native_legacy_surface_height(surface)==height &&
          native_legacy_surface_stride(surface)==stride && native_legacy_surface_format(surface)==0x42475241 &&
          native_legacy_surface_planes(surface)==0;
    printf("uke-fb: default surface %p status 0x%x valid %u %ux%u stride %llu bytes %llu\\n",
           surface,status,valid,width,height,stride,bytes);
    if(!valid){surface->release();return nullptr;}
    return surface;
}
extern "C" uint64_t native_legacy_surface_allocation(OSObject *surface) {
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);return memory?memory->getLength():0;
}
extern "C" {char boot_progress_ext[8][48];}
extern "C" int boot_progress_prepare_scanout(void) {return 1;}
extern "C" void boot_progress_cancel_scanout(void) {}
extern "C" void boot_progress_stage(const char *) {}
extern "C" int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) {return 0;}
extern "C" bool native_adreno_fences(uint32_t *submitted,uint32_t *retired) {
    if(submitted)*submitted=0;
    if(retired)*retired=0;
    return false;
}
extern "C" bool native_adreno_fences_detail(uint32_t *a,uint32_t *b,uint32_t *c) {
    if(a)*a=0;
    if(b)*b=0;
    if(c)*c=0;
    return false;
}
extern "C" int native_gpu_trace_enabled(void) {return 0;}
extern "C" void native_gpu_trace_record(uint32_t,uint32_t,const uint64_t *,uint32_t) {}
''')
compile_cpp(support, OUT / 'LegacyDisplaySupport27.o')

keystore_source = ROOT / 'kits/uke/KeyStoreProbe.cpp'
compile_cpp(keystore_source, OUT / 'KeyStoreProbe.o')
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeKeyStoreEvents'], text=True), end='')
keyevents = ROOT / 'build/apple-key-store-state'
keyevents_late = keyevents / 'NativeKeyStoreEvents.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
     keyevents / 'NativeKeyStoreEvents.o', keyevents_late])

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
gop = ROOT / 'build/native-gop-presenter'
gop_late = gop / 'NativeGOPPresenter.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     gop / 'NativeGOPPresenter.o', gop_late])

archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o', OUT / 'original-addresses.o',
     OUT / 'LegacyDisplayProbe.o', OUT / 'LegacyDisplaySupport27.o', gop_late,
     OUT / 'KeyStoreProbe.o', keyevents_late])

sources = [
    ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
    ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
    ROOT / 'drivers/GopFramebuffer/NativeHalfCodes.h',
    source,
    ROOT / 'scripts' / ABI_PATH.name,
    keystore_source,
    ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
]


def source_name(path):
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={source_name(p): sha(p) for p in sources},
    generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    kernelcache_sha256=abi['kernelcache_sha256'],
    compiled_object_sha256=sha(obj),
    archive_sha256=sha(archive),
    bindings={n: hex(v) for n, v in bindings.items()},
    virtual_entries=len(slots),
    owned_entries=owned,
    inherited_native_entries=native,
    original_bindings='fixed-collection-addresses',
    original_instructions_modified=False,
), indent=2) + '\n')
print(f'Built the owned Legacy backend on the {BUILD} class: {len(owned)} owned, {len(native)} native, {len(bindings)} original bindings')
