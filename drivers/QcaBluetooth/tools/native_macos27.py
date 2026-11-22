# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Version-locked native macOS 27.3 Bluetooth routing and image metadata.

The arm64e slice of 26C5035r's bluetoothd is the same MobileBluetooth code as iPadOS 27's (native_ios27.py), laid out differently and
with a few additions. Every address here was compared with its iPadOS counterpart instruction by instruction. What differs:
- the factory always builds a controller (its last branch is a default), so only the constructor thunk is routed;
- registration takes a third argument and keeps the type in w8;
- the advertising address refresh has one more gate, a flag that a block new in this build clears for controller types up to 3;
  that one call is removed, so the flag keeps its default and each advertising set still gets its address;
- the vendor operation table is 0x60 bytes longer and the model's advertising set count sits in another slot;
- the chipset assumed without an external controller is a Broadcom part; it becomes 0, the daemon's value for a third-party one.
"""
import hashlib
import struct
from pathlib import Path

import native_ios27 as ios
from native_ios27 import PACIBSP, RET, add, adrp, branch, cbnz, memory, mov_w, relative

ORIGINAL_SHA256 = '82e244d5dcafbc150e820416ac7037debc9def106eae043ee51ed4e2c84e7721'
ORIGINAL_UUID = '01204E5C-F295-35D8-9ED6-62ECF79D62DF'
CAVE = 0x100b2e310
CAVE_END = 0x100b30000
DESCRIPTORS = 0x100ba61c8
DESCRIPTOR_COUNT = 0x118
SUPPORT_TABLE = 0x100be19f8
SUPPORT_BYTES = 64
LE_FEATURES = 0x100be18c6
CHIPSET_DEFAULT = 0x1002001f8      # mov w0, #1 before the chipset setter: the default when no external controller was found
CHIPSET_SETTER = 0x100183784
THIRD_PARTY_LE_EVENT_MASK = 0x10095d54b
LE_EXTENDED_ADVERTISING_REPORT_BIT = 1 << (0x0d - 1)
CONTROLLER_THUNK = 0x100390790
CONTROLLER_INIT = 0x10038fcf4
VIRTUAL_MACHINE_CHECK = 0x100493b64
HOST_BUFFER_STATUS = 0x100196648
HOST_FLOW_UNSUPPORTED = 0x100196680
ERRONEOUS_STATUS = 0x10019673c
ADDRESS_REFRESH = 0x10024d6d0
ADDRESS_REFRESH_TARGET = 0x10024a744
ADV_ADDRESS_CACHED = 0x10022abf4
ADV_ADDRESS_FLAG_CLEAR = 0x100591454   # bl to the flag's setter with 0, in the block for controller types up to 3
ADV_ADDRESS_FLAG_SETTER = 0x1002285c0
ADDRESS_FALLBACK = 0x10024dae8
ADDRESS_FINISH = 0x10024da50
ADDRESS_RETURN = 0x10024da64
COMMAND_SUBMIT = 0x100196a24
SET_RANDOM_ADDRESS = 0xa5
REGISTER_SITE = 0x100211eb0      # sub w9, w8, #1
REGISTER_ACCEPT = 0x100211ec0
VENDOR_SITE = 0x100211b28        # sub w8, w0, #1
VENDOR_EPILOGUE = 0x100211ca8
VENDOR_OPS = 0x100bb4060
VENDOR_OPS_BYTES = 0xb68
VENDOR_HOOKS = (0x100bb3798, 0x100bb37b8, 0x100bb33b8, 0x100bb4bc8)
VENDOR_OPCODES = 0x100bb4bd0
CALLBACKS = ((0x10021229c, 0x100212384), (0x1002123c4, 0x100212474))   # site, epilogue
OPTIONAL_STEP = 0x100195114
CHAIN_NEXT = 0x10028552c
CHAIN_CONTEXT = 0x100bb3348
MODEL_GETTER = 0x100404a04
MODEL_VTABLE = 0x100b42d78
MODEL_CAPABILITIES = dict(ios.MODEL_CAPABILITIES, adv_sets=0x1170)
H4_PRESTOP = 0x10016d644
ROOT_KEY_STORE_CHECK = 0x100824864
ROOT_KEY_FAILURE = 0x100824930
ROOT_KEY_LOAD_RESULT = 0x1008249b4
ROOT_KEY_READS = (0x100824354, 0x100824410, 0x1008244c8, 0x100824518, 0x1008245a4, 0x100824898)
ROOT_KEY_WRITES = (0x1008243bc, 0x100824474, 0x1008245e4, 0x1008246c0, 0x100824704, 0x1008247ac,
                   0x100824834, 0x100824908)
TRANSPORT = 0x100ba6138
H4_TABLE = 0x100ba5598
NATIVE_STATE = 0x100be3f80
NATIVE_STATE_BYTES = 128
# The daemon's sandbox maps libraries only from system locations. The addon's lib directory is shown over /usr/lib (as on iPadOS).
NATIVE_DYLIB = b'/usr/lib/QcaBTType0.dylib'
ADV_START = 0x10074a120
ADV_MODE = 0x90
ADV_CAPACITY = 6
FEATURE_CALLBACKS = ((0, 0x100195fa4, 0x100196090, 0x1001960a0, 1), (1, 0x100195e0c, 0x100195f74, 0x100195f84, 2))
ADVANCE = 0x100191ff0
LEGACY_STEP = 0x1009a914b
ALL_STEP = 0x1009a90db
CAP_OFFSETS = ios.CAP_OFFSETS
NATIVE_BINDINGS = dict(STATE=NATIVE_STATE, STATE_BYTES=NATIVE_STATE_BYTES,
    DESCRIPTORS=DESCRIPTORS, DESCRIPTOR_COUNT=DESCRIPTOR_COUNT,
    SUPPORT_TABLE=SUPPORT_TABLE, SUPPORT_BYTES=SUPPORT_BYTES, LE_FEATURES=LE_FEATURES,
    SUBMIT=COMMAND_SUBMIT, ADVANCE=ADVANCE, CHAIN_FAIL=0x100285620,
    CHAIN_CONTEXT=CHAIN_CONTEXT, INIT_SUBMIT_FLAG=0x100be19dc, MODEL=MODEL_GETTER,
    SET_EXT_ADV=0x10022858c, SET_SPEC_ADV=0x100228580, SET_ADV_HANDLES=0x1002285a8,
    SET_ENH_CONNECTION=0x10023b478, SET_EXT_SCAN=0x100259494, SET_EXT_SCAN_HCI=0x100259420,
    SET_CODED_SCAN=0x10025969c,
    SET_PERIODIC_ADV=0x1002299c0, SET_PERIODIC_SYNC=0x100103150,
    CACHE_EXT_ADV=0x100bc8fe0, CACHE_EXT_SCAN=0x100bc8fd0, CACHE_SPEC_ADV=0x100bc9148,
    CACHE_ENH_CONNECTION=0x100bc9158, CACHE_PERIODIC_ADV=0x100bc8ff0,
    CACHE_PERIODIC_SYNC=0x100bc9000, CACHE_PAST_SEND=0x100bc9010,
    CACHE_PAST_RECV=0x100bc9020, CACHE_CODED_PHY=0x100bc9100, CACHE_PHY_2M=0x100ba8510,
    LEGACY_STEP=LEGACY_STEP, ALL_STEP=ALL_STEP, MODEL_MULTIPLE_ADV=0xa0,
    ADV_MODE=ADV_MODE, ADV_CAPACITY=ADV_CAPACITY, ID_ADV_SETS=0xd1)
BZERO = 0x1008cde44


class NativeImage(ios.NativeImage):
    """The iPadOS adapter's image handling (fixups, cave, reports) over the macOS addresses."""

    def __init__(self, path):
        ios._inspect.Image.__init__(self, path)
        if hashlib.sha256(self.data).hexdigest() != ORIGINAL_SHA256:
            raise ValueError('bluetoothd must be the arm64e slice of the locked macOS 27.3 original (lipo -thin arm64e)')
        if self.uuid != ORIGINAL_UUID or self.subtype != 0x80000002:
            raise ValueError('bluetoothd UUID or arm64e subtype differs')
        self.original = self.data
        self.data = bytearray(self.data)
        self.cursor = CAVE
        self.patches = []
        self.entries = {}
        self.fixups = self.chained_fixups()
        start, end = self.offset(CAVE), self.offset(CAVE_END - 1) + 1
        if any(self.data[start:end]):
            raise ValueError('executable tail is occupied')
        for section in self.sections:
            if max(CAVE, section['address']) < min(CAVE_END, section['address'] + section['size']):
                raise ValueError('executable tail overlaps a section')
        text = next(s for s in self.segments if s[0] == '__TEXT')
        if CAVE_END > text[1] + text[4]:
            raise ValueError('executable tail is not file-backed __TEXT')
        if self.stubs.get('_bzero') != BZERO:
            raise ValueError('bzero import differs')

    def emit(self, name, words):
        start = self.cursor
        if start + 4 * len(words) > CAVE_END:
            raise ValueError('native trampoline tail exhausted')
        self.put(start, [word(start + n * 4) if callable(word) else word for n, word in enumerate(words)])
        self.cursor += 4 * len(words)
        self.entries[name] = hex(start)
        return start

    def capability_slots(self):
        result = {}
        for name, slot in MODEL_CAPABILITIES.items():
            address = MODEL_VTABLE + slot
            raw = self.fixups.get(address)
            if raw is None or raw >> 62 != 2 or (raw >> 49) & 3 != 0:
                raise ValueError(f'{name} is not an authenticated IA rebase')
            result[name] = dict(slot=hex(slot), address=hex(address), target=hex(self.base + (raw & 0xffffffff)),
                                diversity=hex((raw >> 32) & 65535), address_diversity=bool((raw >> 48) & 1))
        # The slots are told apart by the diversity of their method names, which both builds share.
        ours = {name: slot['diversity'] for name, slot in result.items()}
        expected = dict(ext_adv='0x72f9', periodic_adv='0xe7eb', periodic_sync='0xf53c', past_sender='0xc042',
                        past_recipient='0xfd9', ext_scan='0x5842', enhanced_connection='0x8b17', spec_adv='0x84f5',
                        coded_phy='0x8393', phy_2m='0x163c', adv_sets='0x74ff')
        if ours != expected:
            raise ValueError(f'model capability slots differ: {ours}')
        return result

    def descriptor(self, opcode):
        matches = []
        for index in range(DESCRIPTOR_COUNT):
            op, bits = struct.unpack_from('<HH', self.data, self.offset(DESCRIPTORS + index * 24) + 2)
            if op == opcode:
                matches.append((index, bits))
        if len(matches) != 1:
            raise ValueError(f'opcode {opcode:#x} has no unique native descriptor')
        return matches[0]

    def here(self, va, count=1):
        return [self.word(va + 4 * n) for n in range(count)]

    def vendor_routing(self):
        if self.descriptor(0x2005) != (SET_RANDOM_ADDRESS, 0x1910) or self.descriptor(0x203b)[0] != NATIVE_BINDINGS['ID_ADV_SETS']:
            raise ValueError('command descriptor numbers differ')
        # The chipset this daemon assumes when it finds no external controller is 1 (a Broadcom part): its initialization then asks
        # for the chip's version with a Broadcom vendor command, which this controller refuses, and the daemon restarted every ten
        # seconds ("HCI initialization failed", sheng run52). Chipset 0 is the daemon's own value for a third-party controller, whose
        # initialization uses no vendor command; it becomes the default.
        if self.here(CHIPSET_DEFAULT + 4, 1) != [branch(CHIPSET_DEFAULT + 4, CHIPSET_SETTER, True)]:
            raise ValueError('chipset default site differs')
        self.put(CHIPSET_DEFAULT, [mov_w(0, 0)], [mov_w(0, 1)])
        # Type zero's default LE event mask predates standard extended scanning.
        # Enable its report event when the Type0 adapter selects extended scan commands.
        self.put(THIRD_PARTY_LE_EVENT_MASK, [0x1f | LE_EXTENDED_ADVERTISING_REPORT_BIT, 0], [0x1f, 0])
        # Every factory branch reaches this thunk: the board has a standard H4 controller whatever product the OS image names.
        entry = self.emit('controller_type', [mov_w(1, 0), lambda pc: branch(pc, CONTROLLER_INIT)])
        self.put(CONTROLLER_THUNK, [branch(CONTROLLER_THUNK, entry)], [branch(CONTROLLER_THUNK, CONTROLLER_INIT)])
        # The board's device tree can carry vmm-present for other components; the daemon reads it as "skip chip initialization".
        check = self.here(VIRTUAL_MACHINE_CHECK, 4)
        if check[0] & 0x9f00001f != 0x90000008 or check[1] & 0xffc003ff != 0xf9400108 or check[2] != 0xb100051f:
            raise ValueError('virtual machine check differs')
        self.put(VIRTUAL_MACHINE_CHECK, [mov_w(0, 0), RET], check[:2])
        # Host flow control and erroneous data reporting are optional for the stack.
        self.put(HOST_BUFFER_STATUS, [0xaa1f03e0], [0xaa1303e0])
        word = self.word(HOST_FLOW_UNSUPPORTED)
        if word & 0xff00001f != 0x54000001:
            raise ValueError('host flow control branch differs')
        self.put(HOST_FLOW_UNSUPPORTED, [0xd503201f], [word])
        self.put(ERRONEOUS_STATUS, [0xaa1f03e0], [0xaa1303e0])
        # Queue the scan's random address (0x2005) before a scan can be enabled; the native completion stays on the last one.
        start = self.cursor
        skip = start + 8 * 4
        entry = self.emit('address_first', [lambda pc: branch(pc, ADDRESS_REFRESH_TARGET, True), 0x39400268,
            lambda pc: 0x34000008 | relative(pc, skip, 19) << 5, 0x91000668, 0xf90003e8,
            0xaa1f03e1, mov_w(0, SET_RANDOM_ADDRESS), lambda pc: branch(pc, COMMAND_SUBMIT, True),
            lambda pc: branch(pc, ADDRESS_REFRESH + 4)])
        if self.here(ADDRESS_REFRESH + 16, 2) != [0x39400268, 0x7100051f]:   # ldrb w8, [x19]; cmp w8, #1: x19 is the address record
            raise ValueError('address refresh register use differs')
        self.put(ADDRESS_REFRESH, [branch(ADDRESS_REFRESH, entry)], [branch(ADDRESS_REFRESH, ADDRESS_REFRESH_TARGET, True)])
        # Always send a set's address (0x2035): the refresh caches it even when the controller rejected it for a set that did not exist yet.
        word = self.word(ADV_ADDRESS_CACHED)
        if word & 0xff00001f != 0x54000000:
            raise ValueError('advertising address comparison differs')
        self.put(ADV_ADDRESS_CACHED, [0xd503201f], [word])
        # This build gates the per-set address on a flag (default 1) that it clears for controller types up to 3. Type zero keeps it.
        self.put(ADV_ADDRESS_FLAG_CLEAR, [0xd503201f], [branch(ADV_ADDRESS_FLAG_CLEAR, ADV_ADDRESS_FLAG_SETTER, True)])
        # Without a resolvable address the refresh must still clear its busy flag: leave through the native finish.
        self.put(ADDRESS_FALLBACK, [branch(ADDRESS_FALLBACK, ADDRESS_FINISH)], [branch(ADDRESS_FALLBACK, ADDRESS_RETURN)])
        # Registration: keep the native bound (1..6000) and add exactly type zero. The type is in w8 here.
        start = self.cursor
        entry = self.emit('registration', [lambda pc: cbnz(pc, 8, start + 8),
            lambda pc: branch(pc, REGISTER_ACCEPT), 0x51000509,
            lambda pc: branch(pc, REGISTER_SITE + 4)])
        self.put(REGISTER_SITE, [branch(REGISTER_SITE, entry)], [0x51000509])
        # Only type zero skips the vendor backend: its tables are cleared and the call returns 0 through the native epilogue.
        start = self.cursor
        words = [None] + self.address_words(0, VENDOR_OPS) + [mov_w(1, VENDOR_OPS_BYTES), lambda pc: branch(pc, BZERO, True)]
        for address in VENDOR_HOOKS:
            words.extend([lambda pc, a=address: adrp(pc, 8, a), memory(0xf9000000, 31, 8, address & 4095, 8)])
        words.extend([lambda pc: adrp(pc, 8, VENDOR_OPCODES), memory(0xb9000000, 31, 8, VENDOR_OPCODES & 4095, 4),
                      mov_w(0, 0), lambda pc: branch(pc, VENDOR_EPILOGUE)])
        original = start + 4 * len(words)
        words[0] = lambda pc: cbnz(pc, 0, original)
        words.extend([0x51000408, lambda pc: branch(pc, VENDOR_SITE + 4)])
        entry = self.emit('vendor_init', words)
        self.put(VENDOR_SITE, [branch(VENDOR_SITE, entry)], [0x51000408])
        # Both callback registrations keep their generic callback for type zero.
        for n, (site, epilogue) in enumerate(CALLBACKS):
            start = self.cursor
            entry = self.emit(f'callbacks_{n}', [lambda pc, t=start + 8: cbnz(pc, 8, t),
                lambda pc, t=epilogue: branch(pc, t), 0x51000509, lambda pc, t=site + 4: branch(pc, t)])
            self.put(site, [branch(site, entry)], [0x51000509])
        # Optional command 0x0c5b follows the native support bitmap; an unsupported one goes on with the chain. (This build also
        # skips it natively when its chipset value is 0; the bitmap test comes first and agrees with it.)
        index, bits = self.descriptor(0x0c5b)
        if (index, bits) != (0x7c, 0x1208):
            raise ValueError('optional command descriptor differs')
        address = SUPPORT_TABLE + (bits >> 8)
        start = self.cursor
        entry = self.emit('optional_0c5b', [lambda pc: adrp(pc, 16, address),
            memory(0x39400000, 16, 16, address & 4095, 1),
            lambda pc: 0x36180010 | relative(pc, start + 20, 14) << 5,
            0xd100c3ff, lambda pc: branch(pc, OPTIONAL_STEP + 8),
            lambda pc: adrp(pc, 8, CHAIN_CONTEXT),
            memory(0xf9400000, 0, 8, CHAIN_CONTEXT & 4095, 8),
            0xd50323ff,
            lambda pc: branch(pc, CHAIN_NEXT)])
        if self.word(OPTIONAL_STEP) != PACIBSP:
            raise ValueError('optional step entry differs')
        self.put(OPTIONAL_STEP + 4, [branch(OPTIONAL_STEP + 4, entry)], [0xd100c3ff])
        return self

    def volatile_root_keys(self):
        for site in ROOT_KEY_READS:
            word = self.word(site)
            if word & 0xff00001f != 0x54000001:
                raise ValueError('root key read branch differs')
            self.put(site, [branch(site, site + ((word >> 5 & 0x7ffff) << 2))], [word])
        for site in ROOT_KEY_WRITES:
            word = self.word(site)
            if word & 0xff00001f != 0x34000000:
                raise ValueError('root key write branch differs')
            offset = word >> 5 & 0x7ffff
            offset = offset - (1 << 19) if offset >> 18 else offset
            self.put(site, [branch(site, site + (offset << 2))], [word])
        word = self.word(ROOT_KEY_STORE_CHECK)
        if word & 0xff00001f != 0x35000000:
            raise ValueError('root key store check differs')
        self.put(ROOT_KEY_STORE_CHECK, [0xd503201f], [word])
        self.put(ROOT_KEY_FAILURE, [mov_w(24, 0)], [mov_w(24, 0x82)])
        self.put(ROOT_KEY_LOAD_RESULT, [0xaa1f03f8], [0xaa0003f8])
        return self

    def h4_shutdown(self):
        if self.word(H4_TABLE) != 1 or struct.unpack_from('<Q', self.data, self.offset(H4_TABLE) + 0x60)[0]:
            raise ValueError('native H4 table or empty pre-stop extension differs')
        expected = [adrp(H4_PRESTOP, 8, TRANSPORT), memory(0xf9400000, 8, 8, TRANSPORT & 4095, 8), memory(0xb9400000, 9, 8, 0, 4)]
        if self.here(H4_PRESTOP, 3) != expected:
            raise ValueError('native pre-stop transport lookup differs')
        start = self.cursor
        entry = self.emit('h4_prestop', [lambda pc: adrp(pc, 8, TRANSPORT), expected[1], expected[2],
            0x7100053f, lambda pc: 0x54000001 | relative(pc, start + 24, 19) << 5,
            RET, lambda pc: branch(pc, H4_PRESTOP + 12)])
        self.put(H4_PRESTOP, [branch(H4_PRESTOP, entry)], expected[:1])
        return self

    def capability_adapter(self):
        segment = next(s for s in self.segments if s[0] == '__DATA')
        if not segment[1] <= NATIVE_STATE or NATIVE_STATE + NATIVE_STATE_BYTES > segment[1] + segment[2]:
            raise ValueError('native capability storage is outside writable data')
        for section in self.sections:
            if max(NATIVE_STATE, section['address']) < min(NATIVE_STATE + NATIVE_STATE_BYTES, section['address'] + section['size']):
                raise ValueError('native capability storage overlaps a section')
        self.capability_slots()
        for name, slot in MODEL_CAPABILITIES.items():
            address = NATIVE_STATE + CAP_OFFSETS[name]
            op, scale = (0xb9400000, 4) if name == 'adv_sets' else (0x39400000, 1)
            getter = self.emit('get_' + name, [0xd503245f, lambda pc, a=address: adrp(pc, 8, a),
                                               memory(op, 0, 8, address & 4095, scale), RET])
            raw = self.fixups[MODEL_VTABLE + slot]
            new = (raw & 0xffffffff00000000) | (getter - self.base)
            self.put(MODEL_VTABLE + slot, [new & 0xffffffff, new >> 32], [raw & 0xffffffff, raw >> 32])
        publisher = self.emit('publish', [PACIBSP, 0xa9bf7bfd, 0x910003fd,
            lambda pc: adrp(pc, 16, NATIVE_STATE), memory(0xf9400000, 16, 16, NATIVE_STATE & 4095, 8),
            0xd63f0a1f, 0xa8c17bfd, 0xd65f0fff])
        for kind, entry, tail, epilogue, parameter in FEATURE_CALLBACKS:
            displaced = self.word(entry + 4)
            if self.word(entry) != PACIBSP or displaced not in (0xd10103ff, 0xd10183ff):
                raise ValueError('native feature callback prologue differs')
            capture = self.emit(f'features_{kind}', [lambda pc: adrp(pc, 8, NATIVE_STATE),
                memory(0xb9000000, 0, 8, (NATIVE_STATE & 4095) + 64, 4),
                mov_w(9, kind), memory(0xb9000000, 9, 8, (NATIVE_STATE & 4095) + 68, 4),
                memory(0xf9000000, parameter, 8, (NATIVE_STATE & 4095) + 72, 8),
                displaced, lambda pc, e=entry: branch(pc, e + 8)])
            self.put(entry + 4, [branch(entry + 4, capture)], [displaced])
            name = ALL_STEP if kind else LEGACY_STEP
            expected = [adrp(tail, 1, name), add(1, 1, name & 4095), mov_w(0, 0), branch(tail + 12, ADVANCE, True)]
            self.put(tail, [branch(tail, publisher, True), branch(tail + 4, epilogue), 0xd503201f, 0xd503201f], expected)
        prepare = self.emit('prepare_advertising', [0xa9bf7be0, lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, (NATIVE_STATE & 4095) + 8, 8), 0xd63f0a1f,
            0xa8c17be0, 0xd10143ff, lambda pc: branch(pc, ADV_START + 8)])
        if self.word(ADV_START) != PACIBSP:
            raise ValueError('native broadcaster entry differs')
        self.put(ADV_START + 4, [branch(ADV_START + 4, prepare)], [0xd10143ff])
        at = self.load_commands_end
        size = (24 + len(NATIVE_DYLIB) + 1 + 7) & ~7
        first_section = min(s['offset'] for s in self.sections if s['segment'] == '__TEXT')
        if at + size > first_section or any(self.data[at:at + size]):
            raise ValueError('no empty load-command room for native library')
        command = struct.pack('<6I', 0xc, size, 24, 2, 0x10000, 0x10000) + NATIVE_DYLIB
        words = struct.unpack('<' + 'I' * (size // 4), command.ljust(size, b'\0'))
        self.put(self.base + at, words, [0] * len(words))
        count, total = struct.unpack_from('<II', self.data, 16)
        self.put(self.base + 16, [count + 1, total + size], [count, total])
        self.entries['capability_state'] = hex(NATIVE_STATE)
        self.entries['library'] = NATIVE_DYLIB.decode()
        return self

    def report(self):
        return dict(input_sha256=ORIGINAL_SHA256, input_uuid=self.uuid,
                    output_sha256=hashlib.sha256(self.data).hexdigest(),
                    adapter_ready='library' in self.entries, hardware_verified=False,
                    cave_start=hex(CAVE), cave_end=hex(self.cursor),
                    entries=self.entries, patches=self.patches, capability_slots=self.capability_slots())


def layout_header():
    """The private ABI shared with the adapter library, as native_ios27.layout_header writes it for iPadOS."""
    lines = ['/* Generated by native_macos27.py; external Apple image stays local. */',
             '#ifndef QBT_NATIVE_LAYOUT_H', '#define QBT_NATIVE_LAYOUT_H', '#include <stdint.h>', '#include <stddef.h>']
    lines += [f'#define QBT_{name} 0x{address:x}ULL' for name, address in NATIVE_BINDINGS.items()]
    lines += ['#define QBT_NATIVE_UUID {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(ORIGINAL_UUID.replace('-', ''))) + '}']
    lines += ['struct qbt_native_state {', '    void (*publish)(void);', '    void (*prepare)(void *);']
    lines += [f'    uint8_t {name};' for name in MODEL_CAPABILITIES if name != 'adv_sets']
    lines += ['    uint8_t reserved[2];', '    uint32_t adv_sets;', '    uint8_t reserved_tail[32];', '    uint32_t status, kind;',
              '    uintptr_t parameters;', '};']
    for name, offset in CAP_OFFSETS.items():
        lines += [f'_Static_assert(offsetof(struct qbt_native_state, {name}) == {offset}, "{name} ABI");']
    lines += ['_Static_assert(offsetof(struct qbt_native_state, status) == 64, "status ABI");',
              '_Static_assert(offsetof(struct qbt_native_state, parameters) == 72, "parameters ABI");', '#endif', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('image', type=Path, help='arm64e slice of the original bluetoothd')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = NativeImage(args.image).vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.data)
    report = args.output.with_suffix(args.output.suffix + '.json')
    report.write_text(json.dumps(image.report(), indent=2) + '\n')
    print(f'Native capability adapter: {args.output}; report: {report}')
