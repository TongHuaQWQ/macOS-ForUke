# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Version-locked native iPadOS 27 Bluetooth routing and image metadata."""
import hashlib
import importlib.util
import struct
from pathlib import Path

_spec = importlib.util.spec_from_file_location('qbt_inspect_ios', Path(__file__).with_name('inspect-ios.py'))
_inspect = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_inspect)

ORIGINAL_SHA256 = 'cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1'
ORIGINAL_UUID = '8E49197B-E6BC-3F3B-A8D3-DF4BEDD37F01'
BASE = 0x100000000
CAVE = 0x100b9bba0
CAVE_END = 0x100b9c000
DESCRIPTORS = 0x100c1ce88
DESCRIPTOR_COUNT = 0x118
SUPPORT_TABLE = 0x100cadc50
SUPPORT_BYTES = 64
LE_FEATURES = 0x100cadb2e
FACTORIES = (0x10050a9dc, 0x10050bf00, 0x10050bf48)
CONTROLLER_THUNK = 0x1004a5290
CONTROLLER_INIT = 0x1004a4560
VIRTUAL_MACHINE_CHECK = 0x10056a12c
UNKNOWN_PRODUCT_CHECK = 0x10050c204
HOST_BUFFER_STATUS = 0x10032affc
HOST_FLOW_UNSUPPORTED = 0x10032b034
ERRONEOUS_STATUS = 0x10032b0f0
ADDRESS_REFRESH = 0x1003a77e8
ADV_ADDRESS_CACHED = 0x1000dd7bc
ADDRESS_FALLBACK = 0x1003a7bfc
ADDRESS_FINISH = 0x1003a7b64
ADDRESS_RETURN = 0x1003a7b78
COMMAND_SUBMIT = 0x1000648b8
SET_RANDOM_ADDRESS = 0xa5
REGISTER = 0x100377d28
REGISTERED_CHIP = 0x100c1e994
REGISTERED_CONTEXT = 0x100c2c238
VENDOR_INIT = 0x100377b60
VENDOR_CHIP = 0x100c2b610
VENDOR_OPS = 0x100c2b720
VENDOR_OPS_BYTES = 0xb08
VENDOR_HOOKS = (0x100c2ae40, 0x100c2ae60, 0x100c2aa60, 0x100c2c228)
VENDOR_OPCODES = 0x100c2c230
CALLBACKS = ((0x100377e9c, 0x100377ec0, 0x100377fa8, 0x100c2c278),
             (0x100377fc4, 0x100377fe8, 0x100378098, 0x100c2c428))
OPTIONAL_STEP = 0x100329ec8
CHAIN_NEXT = 0x1003d7848
CHAIN_CONTEXT = 0x100c2a9f0
MODEL_GETTER = 0x1000071f0
MODEL_VTABLE = 0x100bafcf8
# Model slots traced through the target's configuration getters and their logs.
MODEL_CAPABILITIES = dict(ext_adv=0xa30, periodic_adv=0xa38, periodic_sync=0xa40,
    past_sender=0xa48, past_recipient=0xa50, ext_scan=0xac0, enhanced_connection=0xac8,
    spec_adv=0xad8, coded_phy=0xba8, phy_2m=0xbb8, adv_sets=0x1138)
H4_PRESTOP = 0x1003083cc
ROOT_KEY_STORE_CHECK = 0x10085ebcc
ROOT_KEY_FAILURE = 0x10085ec98
ROOT_KEY_LOAD_RESULT = 0x10085ed1c
ROOT_KEY_READS = (0x10085e6bc, 0x10085e778, 0x10085e830, 0x10085e880, 0x10085e90c, 0x10085ec00)
ROOT_KEY_WRITES = (0x10085e724, 0x10085e7dc, 0x10085e94c, 0x10085ea28, 0x10085ea6c, 0x10085eb14,
                   0x10085eb9c, 0x10085ec70)
TRANSPORT = 0x100c1cdf8
H4_TABLE = 0x100c1cd50
NATIVE_STATE = 0x100caff80
NATIVE_STATE_BYTES = 128
NATIVE_DYLIB = b'/usr/lib/QcaBTType0.dylib'
ADV_START = 0x1007ce390
ADV_MODE = 0x90
ADV_CAPACITY = 6  # 0x1003892f8..300: seven-byte records, memset_s bound 42
CAP_OFFSETS = {name: 16 + index for index, name in enumerate(MODEL_CAPABILITIES) if name != 'adv_sets'}
CAP_OFFSETS['adv_sets'] = 28
NATIVE_BINDINGS = dict(STATE=NATIVE_STATE, STATE_BYTES=NATIVE_STATE_BYTES,
    DESCRIPTORS=DESCRIPTORS, DESCRIPTOR_COUNT=DESCRIPTOR_COUNT,
    SUPPORT_TABLE=SUPPORT_TABLE, SUPPORT_BYTES=SUPPORT_BYTES, LE_FEATURES=LE_FEATURES,
    SUBMIT=0x1000648b8, ADVANCE=0x10032702c, CHAIN_FAIL=0x1003d793c,
    CHAIN_CONTEXT=CHAIN_CONTEXT, INIT_SUBMIT_FLAG=0x100cadc34, MODEL=MODEL_GETTER,
    SET_EXT_ADV=0x10038987c, SET_SPEC_ADV=0x100389870, SET_ADV_HANDLES=0x100389898,
    SET_ENH_CONNECTION=0x100398f60, SET_EXT_SCAN=0x1003af20c, SET_EXT_SCAN_HCI=0x1003af198,
    SET_CODED_SCAN=0x1003af268,
    SET_PERIODIC_ADV=0x10038ac8c, SET_PERIODIC_SYNC=0x100296dbc,
    CACHE_EXT_ADV=0x100c3d120, CACHE_EXT_SCAN=0x100c3d110, CACHE_SPEC_ADV=0x100c3d288,
    CACHE_ENH_CONNECTION=0x100c3d298, CACHE_PERIODIC_ADV=0x100c3d130,
    CACHE_PERIODIC_SYNC=0x100c3d140, CACHE_PAST_SEND=0x100c3d150,
    CACHE_PAST_RECV=0x100c3d160, CACHE_CODED_PHY=0x100c3d240, CACHE_PHY_2M=0x100c1f140,
    LEGACY_STEP=0x100a0ef96, ALL_STEP=0x100a0ef26, MODEL_MULTIPLE_ADV=0xa0,
    ADV_MODE=ADV_MODE, ADV_CAPACITY=ADV_CAPACITY, ID_ADV_SETS=0xd1)
BZERO = 0x10090a160
PACIBSP = 0xd503237f
RET = 0xd65f03c0


def relative(pc, target, bits):
    delta = target - pc
    if delta % 4 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f'branch outside aligned range: {pc:#x} -> {target:#x}')
    return (delta // 4) & ((1 << bits) - 1)


def branch(pc, target, link=False):
    return (0x94000000 if link else 0x14000000) | relative(pc, target, 26)


def cbnz(pc, reg, target):
    return 0x35000000 | relative(pc, target, 19) << 5 | reg


def adrp(pc, reg, target):
    pages = ((target & ~4095) - (pc & ~4095)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError('ADRP outside range')
    pages &= (1 << 21) - 1
    return 0x90000000 | (pages & 3) << 29 | (pages >> 2) << 5 | reg


def add(reg, source, value):
    if not 0 <= value < 4096:
        raise ValueError('ADD immediate outside range')
    return 0x91000000 | value << 10 | source << 5 | reg


def memory(op, reg, base, offset, scale):
    if offset % scale or not 0 <= offset // scale < 4096:
        raise ValueError('load/store offset outside range')
    return op | (offset // scale) << 10 | base << 5 | reg


def mov_w(reg, value):
    if not 0 <= value <= 65535:
        raise ValueError('MOV immediate outside range')
    return 0x52800000 | value << 5 | reg


class NativeImage(_inspect.Image):
    def __init__(self, path):
        super().__init__(path)
        if hashlib.sha256(self.data).hexdigest() != ORIGINAL_SHA256:
            raise ValueError('bluetoothd must match the locked iPadOS 27 original SHA-256')
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

    def chained_fixups(self):
        """Enumerate the target's dyld USERLAND24 chains without changing them.

        Layout: Apple's mach-o/fixup-chains.h. Authenticated rebases retain
        diversity, address diversity, key and next when their target changes.
        """
        at = 32
        payload = None
        count = struct.unpack_from('<I', self.data, 16)[0]
        for _ in range(count):
            command, size = struct.unpack_from('<II', self.data, at)
            if command == 0x80000034:
                payload, length = struct.unpack_from('<II', self.data, at + 8)
                if payload + length > len(self.data):
                    raise ValueError('chained fixups extend beyond image')
            at += size
        if payload is None:
            raise ValueError('native image has no chained fixups')
        version, starts = struct.unpack_from('<II', self.data, payload)
        if version != 0:
            raise ValueError('unknown chained fixup version')
        starts += payload
        count = struct.unpack_from('<I', self.data, starts)[0]
        if count != len(self.segments):
            raise ValueError('chained fixup segment count differs')
        result = {}
        for index, segment in enumerate(self.segments):
            delta = struct.unpack_from('<I', self.data, starts + 4 + index * 4)[0]
            if not delta:
                continue
            header = starts + delta
            size, page_size, form, offset, _, pages = struct.unpack_from('<IHHQIH', self.data, header)
            if form != 12 or page_size != 16384 or size < 22 + pages * 2:
                raise ValueError('native USERLAND24 page format differs')
            if self.base + offset != segment[1]:
                raise ValueError('chained fixup segment address differs')
            for page in range(pages):
                first = struct.unpack_from('<H', self.data, header + 22 + page * 2)[0]
                if first == 0xffff:
                    continue
                if first & 0x8000 or first >= page_size:
                    raise ValueError('unsupported multi-start native fixup page')
                page_va = self.base + offset + page * page_size
                va = page_va + first
                while True:
                    if va in result or va + 8 > page_va + page_size:
                        raise ValueError('native fixup chain crosses its page')
                    raw = struct.unpack_from('<Q', self.data, self.offset(va))[0]
                    result[va] = raw
                    step = (raw >> 51) & 0x7ff
                    if not step:
                        break
                    va += step * 8
        return result

    def capability_slots(self):
        result = {}
        for name, slot in MODEL_CAPABILITIES.items():
            address = MODEL_VTABLE + slot
            raw = self.fixups.get(address)
            if raw is None or raw >> 62 != 2 or (raw >> 49) & 3 != 0:
                raise ValueError(f'{name} is not an authenticated IA rebase')
            result[name] = dict(slot=hex(slot), address=hex(address),
                                target=hex(self.base + (raw & 0xffffffff)),
                                diversity=hex((raw >> 32) & 65535),
                                address_diversity=bool((raw >> 48) & 1))
        return result

    def put(self, va, words, expected=None):
        if expected is not None:
            actual = [self.word(va + 4 * n) for n in range(len(expected))]
            if actual != expected:
                raise ValueError(f'instruction mismatch at {va:#x}: {actual!r}')
        for n, word in enumerate(words):
            struct.pack_into('<I', self.data, self.offset(va + 4 * n), word)
        if expected is not None:
            self.patches.append(dict(address=hex(va), original=[hex(w) for w in expected],
                                     patched=[hex(w) for w in words]))

    def emit(self, name, words):
        start = self.cursor
        if start + 4 * len(words) > CAVE_END:
            raise ValueError('native trampoline tail exhausted')
        self.put(start, [word(start + n * 4) if callable(word) else word for n, word in enumerate(words)])
        self.cursor += 4 * len(words)
        self.entries[name] = hex(start)
        return start

    def address_words(self, reg, address):
        return [lambda pc: adrp(pc, reg, address), add(reg, reg, address & 4095)]

    def descriptor(self, opcode):
        matches = []
        for index in range(DESCRIPTOR_COUNT):
            va = DESCRIPTORS + index * 24
            op, bits = struct.unpack_from('<HH', self.data, self.offset(va) + 2)
            if op == opcode:
                matches.append((index, bits))
        if len(matches) != 1:
            raise ValueError(f'opcode {opcode:#x} has no unique native descriptor')
        return matches[0]

    def vendor_routing(self):
        for site in FACTORIES:
            self.put(site, [mov_w(1, 0)], [mov_w(1, 15)])
        # Every product's factory branch reaches this thunk: the board has a standard
        # H4 controller whatever product identity the OS image carries.
        entry = self.emit('controller_type', [mov_w(1, 0),
            lambda pc: branch(pc, CONTROLLER_INIT)])
        self.put(CONTROLLER_THUNK, [branch(CONTROLLER_THUNK, entry)],
                 [branch(CONTROLLER_THUNK, CONTROLLER_INIT)])
        # No factory branch lists the board's product, which would leave the model null
        # and crash the daemon. Fall through into the last constructor call instead.
        self.put(UNKNOWN_PRODUCT_CHECK, [0xd503201f], [0x340002a0])
        # The board's ADT carries vmm-present for other components. This daemon reads it
        # as "skip chip initialization", so the controller would never be booted.
        self.put(VIRTUAL_MACHINE_CHECK, [mov_w(0, 0), RET], [0xf0003688, 0xf942b108])
        # Host flow control and erroneous data reporting are optional for the stack.
        # The callbacks still log a rejection; initialization continues without them.
        self.put(HOST_BUFFER_STATUS, [0xaa1f03e0], [0xaa1303e0])
        self.put(HOST_FLOW_UNSUPPORTED, [0xd503201f], [0x540001c1])
        self.put(ERRONEOUS_STATUS, [0xaa1f03e0], [0xaa1303e0])
        # A scan can be queued while the native refresh still sends its per-set addresses, and
        # its own address is random: enabling it without 0x2005 returns Invalid Parameters.
        # Queue the same address first. The native completion stays on the last 0x2005.
        target = 0x1003a69f8
        start = self.cursor
        skip = start + 8 * 4
        entry = self.emit('address_first', [lambda pc: branch(pc, target, True), 0x39400268,
            lambda pc: 0x34000008 | relative(pc, skip, 19) << 5, 0x91000668, 0xf90003e8,
            0xaa1f03e1, mov_w(0, SET_RANDOM_ADDRESS), lambda pc: branch(pc, COMMAND_SUBMIT, True),
            lambda pc: branch(pc, ADDRESS_REFRESH + 4)])
        self.put(ADDRESS_REFRESH, [branch(ADDRESS_REFRESH, entry)], [branch(ADDRESS_REFRESH, target, True)])
        # The refresh caches each set's address even when the controller rejects it because the
        # set does not exist yet (0x42). Starting that set then skips 0x2035 as already current,
        # and enabling it returns Invalid Parameters forever. Always send the address.
        self.put(ADV_ADDRESS_CACHED, [0xd503201f], [0x54000320])
        # When no resolvable address can be generated, the refresh falls back and returns without
        # clearing its busy flag, so the next advertising update restarts the daemon after 10 s
        # (reason 4074). Leave through the native finish that a refresh without 0x2005 uses.
        self.put(ADDRESS_FALLBACK, [branch(ADDRESS_FALLBACK, ADDRESS_FINISH)],
                 [branch(ADDRESS_FALLBACK, ADDRESS_RETURN)])
        # Preserve the native bound (1..6000), adding exactly type zero.
        start = self.cursor
        entry = self.emit('registration', [lambda pc: cbnz(pc, 0, start + 8),
            lambda pc: branch(pc, REGISTER + 16), 0x51000409,
            lambda pc: branch(pc, REGISTER + 4)])
        self.put(REGISTER, [branch(REGISTER, entry)], [0x51000409])

        # The original frame and PACIBSP are already in effect. Only type zero
        # skips vendor backend setup. Standard HCI initialization stays native.
        start = self.cursor
        words = [None] + self.address_words(0, VENDOR_OPS) + [mov_w(1, VENDOR_OPS_BYTES),
                  lambda pc: branch(pc, BZERO, True)]
        for address in VENDOR_HOOKS:
            words.extend([lambda pc, a=address: adrp(pc, 8, a),
                          memory(0xf9000000, 31, 8, address & 4095, 8)])
        words.extend([lambda pc: adrp(pc, 8, VENDOR_OPCODES),
                      memory(0xb9000000, 31, 8, VENDOR_OPCODES & 4095, 4),
                      mov_w(0, 0), lambda pc: branch(pc, 0x100377d00)])
        original = start + 4 * len(words)
        words[0] = lambda pc: cbnz(pc, 0, original)
        words.extend([0x51000408, lambda pc: branch(pc, 0x100377b84)])
        entry = self.emit('vendor_init', words)
        self.put(0x100377b80, [branch(0x100377b80, entry)], [0x51000408])

        for n, (_, site, epilogue, _) in enumerate(CALLBACKS):
            start = self.cursor
            entry = self.emit(f'callbacks_{n}', [lambda pc, t=start + 8: cbnz(pc, 8, t),
                lambda pc, t=epilogue: branch(pc, t), 0x51000509,
                lambda pc, t=site + 4: branch(pc, t)])
            self.put(site, [branch(site, entry)], [0x51000509])

        # This optional standard command is absent on some QCA revisions.
        # Use the native support bitmap and real chain-next operation.
        index, bits = self.descriptor(0x0c5b)
        if (index, bits) != (0x7c, 0x1208):
            raise ValueError('optional command descriptor differs')
        byte, mask = bits >> 8, bits & 255
        if byte >= SUPPORT_BYTES or mask != 8:
            raise ValueError('optional command support bit differs')
        address = SUPPORT_TABLE + byte
        start = self.cursor
        entry = self.emit('optional_0c5b', [lambda pc: adrp(pc, 16, address),
            memory(0x39400000, 16, 16, address & 4095, 1),
            lambda pc: 0x36180010 | relative(pc, start + 20, 14) << 5,
            0xd100c3ff, lambda pc: branch(pc, OPTIONAL_STEP + 8),
            lambda pc: adrp(pc, 8, CHAIN_CONTEXT),
            memory(0xf9400000, 0, 8, CHAIN_CONTEXT & 4095, 8),
            0xd50323ff,  # authenticate the original PACIBSP before tail call
            lambda pc: branch(pc, CHAIN_NEXT)])
        site = OPTIONAL_STEP + 4  # retain the original indirect-call landing pad
        self.put(site, [branch(site, entry)], [0xd100c3ff])
        return self

    def volatile_root_keys(self):
        # readRootKeys restarts the daemon when the keychain is unavailable. Continue with
        # generated keys instead; stored keys are used whenever the keychain answers.
        for site in ROOT_KEY_READS:
            word = self.word(site)
            if word & 0xff00001f != 0x54000001:
                raise ValueError('root key read branch differs')
            target = site + ((word >> 5 & 0x7ffff) << 2)
            self.put(site, [branch(site, target)], [word])
        for site in ROOT_KEY_WRITES:
            word = self.word(site)
            if word & 0xff00001f != 0x34000000:
                raise ValueError('root key write branch differs')
            offset = word >> 5 & 0x7ffff
            offset = offset - (1 << 19) if offset >> 18 else offset
            self.put(site, [branch(site, site + (offset << 2))], [word])
        # Remaining key stores and loads cannot fail startup: no restart, and success returned.
        self.put(ROOT_KEY_STORE_CHECK, [0xd503201f], [0x350016c0])
        self.put(ROOT_KEY_FAILURE, [mov_w(24, 0)], [mov_w(24, 0x82)])
        self.put(ROOT_KEY_LOAD_RESULT, [0xaa1f03f8], [0xaa0003f8])
        return self

    def h4_shutdown(self):
        # The stock pre-stop handles none and H4DS, but dereferences +0x60 for
        # H4. The actual H4 table has no such extension. Its regular close
        # callback remains installed and is called by native stack teardown.
        if self.word(H4_TABLE) != 1 or struct.unpack_from('<Q', self.data, self.offset(H4_TABLE) + 0x60)[0]:
            raise ValueError('native H4 table or empty pre-stop extension differs')
        expected = [adrp(H4_PRESTOP, 8, TRANSPORT),
                    memory(0xf9400000, 8, 8, TRANSPORT & 4095, 8),
                    memory(0xb9400000, 9, 8, 0, 4)]
        if [self.word(H4_PRESTOP + i * 4) for i in range(3)] != expected:
            raise ValueError('native pre-stop transport lookup differs')
        start = self.cursor
        entry = self.emit('h4_prestop', [lambda pc: adrp(pc, 8, TRANSPORT), expected[1], expected[2],
            0x7100053f,  # cmp w9, #1
            lambda pc: 0x54000001 | relative(pc, start + 24, 19) << 5,
            RET, lambda pc: branch(pc, H4_PRESTOP + 12)])
        self.put(H4_PRESTOP, [branch(H4_PRESTOP, entry)], expected[:1])
        return self

    def capability_adapter(self):
        # Reserve virtual zero-fill storage; no file section or fixup occupies it.
        segment = next(s for s in self.segments if s[0] == '__DATA')
        if not segment[1] <= NATIVE_STATE or NATIVE_STATE + NATIVE_STATE_BYTES > segment[1] + segment[2]:
            raise ValueError('native capability storage is outside writable data')
        for section in self.sections:
            if max(NATIVE_STATE, section['address']) < min(NATIVE_STATE + NATIVE_STATE_BYTES,
                                                          section['address'] + section['size']):
                raise ValueError('native capability storage overlaps a section')
        self.capability_slots()  # verify authenticated USERLAND24 rebases
        for name, slot in MODEL_CAPABILITIES.items():
            address = NATIVE_STATE + CAP_OFFSETS[name]
            op, scale = (0xb9400000, 4) if name == 'adv_sets' else (0x39400000, 1)
            getter = self.emit('get_' + name, [0xd503245f,
                lambda pc, a=address: adrp(pc, 8, a),
                memory(op, 0, 8, address & 4095, scale), RET])
            raw = self.fixups[MODEL_VTABLE + slot]
            # dyld signs the new rebase using its original key and diversity.
            # Copying a live vtable would invalidate its address-bound signatures.
            new = (raw & 0xffffffff00000000) | (getter - self.base)
            self.put(MODEL_VTABLE + slot, [new & 0xffffffff, new >> 32],
                     [raw & 0xffffffff, raw >> 32])

        publisher = self.emit('publish', [PACIBSP, 0xa9bf7bfd, 0x910003fd,
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, NATIVE_STATE & 4095, 8),
            0xd63f0a1f,  # blraaz x16; library function pointer is signed IA/0
            0xa8c17bfd, 0xd65f0fff])
        for kind, entry, tail, epilogue, parameter in (
                (0, 0x10032a91c, 0x10032aa08, 0x10032aa18, 1),
                (1, 0x10032a784, 0x10032a8ec, 0x10032a8fc, 2)):
            displaced = self.word(entry + 4)
            if self.word(entry) != PACIBSP or displaced not in (0xd10103ff, 0xd10183ff):
                raise ValueError('native feature callback prologue differs')
            capture = self.emit(f'features_{kind}', [lambda pc: adrp(pc, 8, NATIVE_STATE),
                memory(0xb9000000, 0, 8, (NATIVE_STATE & 4095) + 64, 4),
                mov_w(9, kind), memory(0xb9000000, 9, 8, (NATIVE_STATE & 4095) + 68, 4),
                memory(0xf9000000, parameter, 8, (NATIVE_STATE & 4095) + 72, 8),
                displaced, lambda pc, e=entry: branch(pc, e + 8)])
            self.put(entry + 4, [branch(entry + 4, capture)], [displaced])
            name = NATIVE_BINDINGS['ALL_STEP' if kind else 'LEGACY_STEP']
            expected = [adrp(tail, 1, name), add(1, 1, name & 4095), mov_w(0, 0),
                        branch(tail + 12, NATIVE_BINDINGS['ADVANCE'], True)]
            self.put(tail, [branch(tail, publisher, True), branch(tail + 4, epilogue),
                            0xd503201f, 0xd503201f], expected)

        # Refresh the constructor's pre-probe choice before native GAP init.
        prepare = self.emit('prepare_advertising', [0xa9bf7be0,
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, (NATIVE_STATE & 4095) + 8, 8), 0xd63f0a1f,
            0xa8c17be0, 0xd10143ff, lambda pc: branch(pc, ADV_START + 8)])
        if self.word(ADV_START) != PACIBSP:
            raise ValueError('native broadcaster entry differs')
        self.put(ADV_START + 4, [branch(ADV_START + 4, prepare)], [0xd10143ff])

        # Appending preserves every existing import ordinal. __TEXT already has
        # executable tail room, so no segment or __LINKEDIT relocation is needed.
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
                    stage='native-capability-adapter' if 'library' in self.entries else 'vendor-routing-and-h4-shutdown',
                    adapter_ready='library' in self.entries, hardware_verified=False,
                    cave_start=hex(CAVE), cave_end=hex(self.cursor),
                    entries=self.entries, patches=self.patches,
                    capability_slots=self.capability_slots(),
                    pending=([] if 'library' in self.entries else ['native capability publication']) +
                            ['payload installation and device verification'])


def layout_header():
    """Generate the shared private ABI for the version-locked adapter library."""
    lines = ['/* Generated by native_ios27.py; external Apple image stays local. */',
             '#ifndef QBT_IOS27_LAYOUT_H', '#define QBT_IOS27_LAYOUT_H',
             '#include <stdint.h>', '#include <stddef.h>']
    lines += [f'#define QBT_{name} 0x{address:x}ULL' for name, address in NATIVE_BINDINGS.items()]
    lines += ['struct qbt_native_state {',
              '    void (*publish)(void);', '    void (*prepare)(void *);']
    lines += [f'    uint8_t {name};' for name in MODEL_CAPABILITIES if name != 'adv_sets']
    lines += ['    uint8_t reserved[2];', '    uint32_t adv_sets;',
              '    uint8_t reserved_tail[32];', '    uint32_t status, kind;',
              '    uintptr_t parameters;', '};']
    for name, offset in CAP_OFFSETS.items():
        lines += [f'_Static_assert(offsetof(struct qbt_native_state, {name}) == {offset}, "{name} ABI");']
    lines += ['_Static_assert(offsetof(struct qbt_native_state, status) == 64, "status ABI");',
              '_Static_assert(offsetof(struct qbt_native_state, parameters) == 72, "parameters ABI");',
              '#endif', '']
    return '\n'.join(lines)
