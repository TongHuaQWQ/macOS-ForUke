#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the native routing and capability patches against an external original bluetoothd."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from tempfile import TemporaryDirectory

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X8,
    UC_ARM64_REG_X9, UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X29,
    UC_ARM64_REG_X30, UC_ARM64_REG_PC, UC_ARM64_REG_SP)
import native_ios27 as n

STACK = 0x20000000
STACK_BYTES = 0x10000
STOP = STACK + STACK_BYTES
CONTEXT = 0x1234567812345678


class Machine:
    def __init__(self, image, slide):
        self.image = image
        self.slide = slide
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for name, va, size, offset, file_bytes in image.segments:
            if size and name != '__PAGEZERO':
                self.uc.mem_map(va + slide, (size + 4095) & ~4095)
                self.uc.mem_write(va + slide, bytes(image.data[offset:offset + file_bytes]))
        self.uc.mem_map(STACK, STACK_BYTES + 4096)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_BYTES - 0x100)
        self.uc.reg_write(UC_ARM64_REG_X30, STOP)
        self.initial_sp = self.uc.reg_read(UC_ARM64_REG_SP)
        self.pac = 0
        self.visited = []
        self.stop_at = set()
        self.bzero_calls = []
        self.next_calls = []
        self.indirect_calls = []
        self.publish_calls = []
        self.prepare_calls = []
        self.mock_publish = STACK + 0x8000
        self.mock_prepare = STACK + 0x8100
        self.write(n.NATIVE_STATE, struct.pack('<QQ', self.mock_publish, self.mock_prepare))
        self.uc.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, uc, address, size, _):
        if address == STOP or address in self.stop_at:
            uc.emu_stop()
            return
        self.visited.append(address - self.slide)
        if address == self.mock_publish:
            self.publish_calls.append(struct.unpack('<IIQ', self.read(n.NATIVE_STATE + 64, 16)))
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == self.mock_prepare:
            self.prepare_calls.append(uc.reg_read(UC_ARM64_REG_X0))
            uc.reg_write(UC_ARM64_REG_X0, 0xdeadbeef)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == 0x10000d938 + self.slide:
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == n.BZERO + self.slide:
            va = uc.reg_read(UC_ARM64_REG_X0)
            count = uc.reg_read(UC_ARM64_REG_X1)
            self.bzero_calls.append((va, count))
            uc.mem_write(va, bytes(count))
            # Exercise the real caller-saved ABI. No hook can rely on bzero's
            # argument values surviving the imported call.
            for reg in range(UC_ARM64_REG_X0, UC_ARM64_REG_X0 + 19):
                uc.reg_write(reg, 0xdeadbeef)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == n.CHAIN_NEXT + self.slide:
            self.next_calls.append(uc.reg_read(UC_ARM64_REG_X0))
            uc.emu_stop()
        else:
            word = struct.unpack('<I', uc.mem_read(address, 4))[0]
            # Unicorn has no target PAC keys. Model only the signing operation;
            # execute all stack, register, branch and memory instructions.
            if word == n.PACIBSP:
                self.pac += 1
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xd50323ff:  # autibsp
                assert self.pac > 0
                self.pac -= 1
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xd63f0a1f:  # blraaz x16, native-to-library ABI
                from unicorn.arm64_const import UC_ARM64_REG_X16
                target = uc.reg_read(UC_ARM64_REG_X16)
                uc.reg_write(UC_ARM64_REG_X30, address + 4)
                uc.reg_write(UC_ARM64_REG_PC, target)
            elif word == 0xd61f081f:  # braaz x0, authenticated native tail call
                target = uc.reg_read(UC_ARM64_REG_X0)
                self.indirect_calls.append(target)
                uc.reg_write(UC_ARM64_REG_PC, target)
            elif word == 0xd65f0fff:  # retab: native frame is restored already
                assert self.pac > 0
                self.pac -= 1
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))

    def run(self, entry, stops=()):
        self.stop_at = {a + self.slide for a in stops}
        self.uc.emu_start(entry + self.slide, STOP + 4, count=4096)
        pc = self.uc.reg_read(UC_ARM64_REG_PC)
        assert pc == STOP or pc in self.stop_at or self.next_calls, hex(pc)
        return self

    def write(self, address, value):
        self.uc.mem_write(address + self.slide, value)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address + self.slide, size))


def check(image):
    slots = image.capability_slots()
    assert len(slots) == 11
    assert all(v['address_diversity'] for v in slots.values())
    assert slots['periodic_sync']['target'] == '0x1004a5c2c'
    assert slots['adv_sets']['target'] == '0x1004c6234'
    cases = 0
    for slide in (0, 0x5a000000):
        # Check both newly accepted zero and every registration boundary. Native
        # nonzero IDs keep the original optimized range calculation.
        for chip in (0, 1, 15, 1999, 2000, 3999, 4000, 5999, 6000, 6001, 65535, 0xffffffff):
            m = Machine(image, slide)
            m.write(n.REGISTERED_CHIP, b'\xa5' * 4)
            m.write(n.REGISTERED_CONTEXT, b'\xa5' * 8)
            m.uc.reg_write(UC_ARM64_REG_X0, chip)
            m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
            m.run(n.REGISTER)
            valid = chip <= 6000
            assert m.uc.reg_read(UC_ARM64_REG_X0) == (0 if valid else 101)
            assert m.read(n.REGISTERED_CHIP, 4) == (struct.pack('<I', chip) if valid else b'\xa5' * 4)
            assert m.read(n.REGISTERED_CONTEXT, 8) == (struct.pack('<Q', CONTEXT) if valid else b'\xa5' * 8)
            cases += 1

        m = Machine(image, slide)
        for va, size in ((n.VENDOR_OPS, n.VENDOR_OPS_BYTES), (n.VENDOR_OPCODES, 4)):
            m.write(va, b'\xa5' * size)
        for va in n.VENDOR_HOOKS:
            m.write(va, b'\xa5' * 8)
        m.write(n.VENDOR_OPS - 8, b'\x66' * 8)
        m.write(n.VENDOR_OPCODES + 4, b'\x77' * 4)
        m.uc.reg_write(UC_ARM64_REG_X0, 0)
        m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
        m.uc.reg_write(UC_ARM64_REG_X19, 0x19191919)
        m.uc.reg_write(UC_ARM64_REG_X20, 0x20202020)
        m.run(n.VENDOR_INIT)
        assert m.bzero_calls == [(n.VENDOR_OPS + slide, n.VENDOR_OPS_BYTES)]
        assert m.read(n.VENDOR_CHIP, 4) == bytes(4)
        assert m.read(n.VENDOR_OPS, n.VENDOR_OPS_BYTES) == bytes(n.VENDOR_OPS_BYTES)
        assert all(m.read(va, 8) == bytes(8) for va in n.VENDOR_HOOKS)
        assert m.read(n.VENDOR_OPCODES, 4) == bytes(4)
        assert m.read(n.VENDOR_OPS - 8, 8) == b'\x66' * 8
        assert m.read(n.VENDOR_OPCODES + 4, 4) == b'\x77' * 4
        assert m.uc.reg_read(UC_ARM64_REG_X19) == 0x19191919
        assert m.uc.reg_read(UC_ARM64_REG_X20) == 0x20202020
        assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
        assert m.uc.reg_read(UC_ARM64_REG_X0) == 0
        cases += 1

        for chip in (1, 2000, 4000, 6000, 6001):
            m = Machine(image, slide)
            m.uc.reg_write(UC_ARM64_REG_X0, chip)
            m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
            m.run(n.VENDOR_INIT, (0x100377b84,))
            assert m.uc.reg_read(UC_ARM64_REG_X8) == chip - 1
            assert m.uc.reg_read(UC_ARM64_REG_X0) == chip
            assert not m.bzero_calls
            cases += 1

        for entry, site, _, generic in n.CALLBACKS:
            for chip in (0, 1, 2000, 4000, 6000):
                m = Machine(image, slide)
                m.write(n.VENDOR_CHIP, struct.pack('<I', chip))
                m.uc.reg_write(UC_ARM64_REG_X0, CONTEXT)
                m.run(entry, (site + 4,) if chip else ())
                assert m.read(generic, 8) == struct.pack('<Q', CONTEXT)
                if chip:
                    assert m.uc.reg_read(UC_ARM64_REG_X9) == chip - 1
                else:
                    assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
                cases += 1

        for byte in (0, 1, 7, 8, 0xff):
            m = Machine(image, slide)
            m.write(n.SUPPORT_TABLE + 18, bytes([byte]))
            m.write(n.CHAIN_CONTEXT, struct.pack('<Q', CONTEXT))
            m.run(n.OPTIONAL_STEP, (n.OPTIONAL_STEP + 8,))
            if byte & 8:
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp - 0x30
                assert not m.next_calls and m.pac == 1
            else:
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp
                assert m.next_calls == [CONTEXT] and m.pac == 0
            cases += 1

        for transport in (0, 1, 2, 3, 11):
            m = Machine(image, slide)
            table = STACK + 0x100
            m.write(n.TRANSPORT, struct.pack('<Q', table))
            m.uc.mem_write(table, struct.pack('<I', transport))
            callback = 0 if transport == 1 else STOP
            m.uc.mem_write(table + 0x60, struct.pack('<Q', callback))
            m.run(n.H4_PRESTOP)
            assert m.indirect_calls == ([] if transport in (0, 1, 11) else [STOP])
            assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
            cases += 1

        for binding, destination in (
                ('SET_EXT_ADV', 0x100c313f0), ('SET_SPEC_ADV', 0x100c313a9),
                ('SET_ADV_HANDLES', 0x100c2c4b0), ('SET_ENH_CONNECTION', 0x100c3225a),
                ('SET_EXT_SCAN', 0x100c37f85), ('SET_EXT_SCAN_HCI', 0x100c37f87), ('SET_CODED_SCAN', 0x100c37f81),
                ('SET_PERIODIC_ADV', 0x100c32120), ('SET_PERIODIC_SYNC', 0x100c263b0)):
            for value in (0, 6 if binding == 'SET_ADV_HANDLES' else 1):
                m = Machine(image, slide)
                m.write(destination, b'\xa5')
                m.uc.reg_write(UC_ARM64_REG_X0, value)
                m.run(n.NATIVE_BINDINGS[binding])
                assert m.read(destination, 1) == bytes([value])
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp
                cases += 1

        for name, offset in n.CAP_OFFSETS.items():
            m = Machine(image, slide)
            value = 6 if name == 'adv_sets' else 1
            m.write(n.NATIVE_STATE + offset, struct.pack('<I', value) if name == 'adv_sets' else bytes([value]))
            getter = int(image.entries['get_' + name], 16)
            m.run(getter)
            assert m.uc.reg_read(UC_ARM64_REG_X0) == value
            slot = n.MODEL_VTABLE + n.MODEL_CAPABILITIES[name]
            raw = struct.unpack_from('<Q', image.data, image.offset(slot))[0]
            assert raw >> 32 == image.fixups[slot] >> 32
            assert n.BASE + (raw & 0xffffffff) == getter
            cases += 1

        for kind, entry in enumerate((0x10032a91c, 0x10032a784)):
            for status, parameters in ((0, STACK + 0x100), (0x12, STACK + 0x100), (0, 0)):
                m = Machine(image, slide)
                features = (1 << 12) | (1 << 11) | (1 << 8)
                m.uc.mem_write(STACK + 0x100, struct.pack('<QQQ', features, 0, 0))
                m.uc.reg_write(UC_ARM64_REG_X0, status)
                if kind:
                    from unicorn.arm64_const import UC_ARM64_REG_X2
                    m.uc.reg_write(UC_ARM64_REG_X1, 1)
                    m.uc.reg_write(UC_ARM64_REG_X2, parameters)
                else:
                    m.uc.reg_write(UC_ARM64_REG_X1, parameters)
                m.run(entry)
                assert m.publish_calls == [(status, kind, parameters)]
                if status == 0 and parameters:
                    assert m.read(n.LE_FEATURES, 8) == struct.pack('<Q', features)
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
                assert image.word(entry) == n.PACIBSP
                cases += 1

        m = Machine(image, slide)
        m.uc.reg_write(UC_ARM64_REG_X0, STACK + 0x100)
        m.run(n.ADV_START, (n.ADV_START + 8,))
        assert m.prepare_calls == [STACK + 0x100]
        assert m.uc.reg_read(UC_ARM64_REG_X0) == STACK + 0x100
        assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp - 0x50 and m.pac == 1
        assert image.word(n.ADV_START) == n.PACIBSP
        cases += 1

    # Machine code edits cannot silently touch existing functions or fixups.
    permitted = set(range(image.offset(n.CAVE), image.offset(image.cursor)))
    for patch in image.patches:
        start = image.offset(int(patch['address'], 16))
        permitted.update(range(start, start + 4 * len(patch['patched'])))
    assert all(a == b or at in permitted for at, (a, b) in enumerate(zip(image.original, image.data)))
    for site in n.FACTORIES:
        assert image.word(site) == n.mov_w(1, 0)
    assert image.descriptor(0x203b) == (0xd1, 0x2480)
    assert image.word(n.OPTIONAL_STEP) == n.PACIBSP
    # Every product reaches the original constructor with controller type zero.
    entry = int(image.entries['controller_type'], 16)
    assert image.word(n.CONTROLLER_THUNK) == n.branch(n.CONTROLLER_THUNK, entry)
    assert image.word(entry) == n.mov_w(1, 0) and image.word(entry + 4) == n.branch(entry + 4, n.CONTROLLER_INIT)
    cases += 1
    assert image.word(n.UNKNOWN_PRODUCT_CHECK) == 0xd503201f
    assert image.word(n.HOST_BUFFER_STATUS) == 0xaa1f03e0 and image.word(n.ERRONEOUS_STATUS) == 0xaa1f03e0
    assert image.word(n.HOST_FLOW_UNSUPPORTED) == 0xd503201f
    assert n.struct.unpack_from('<HH', image.data, image.offset(n.DESCRIPTORS + n.SET_RANDOM_ADDRESS * 24) + 2)[0] == 0x2005
    assert image.word(n.ADDRESS_REFRESH) >> 26 == 5
    assert image.word(n.ADV_ADDRESS_CACHED) == 0xd503201f
    assert image.word(n.ADDRESS_FALLBACK) == n.branch(n.ADDRESS_FALLBACK, n.ADDRESS_FINISH)
    cases += 1
    assert image.word(n.VIRTUAL_MACHINE_CHECK) == n.mov_w(0, 0) and image.word(n.VIRTUAL_MACHINE_CHECK + 4) == n.RET
    cases += 1
    # Keychain results no longer reach the daemon's fatal return; targets are unchanged.
    for site in n.ROOT_KEY_READS + n.ROOT_KEY_WRITES:
        assert image.word(site) >> 26 == 0b000101
        cases += 1
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'out/native-test')
    args = parser.parse_args()
    image = n.NativeImage(args.image).vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    cases = check(image)
    with TemporaryDirectory() as directory:
        wrong = Path(directory) / 'bluetoothd'
        bad = bytearray(image.original)
        bad[0x50a9dc] ^= 1
        wrong.write_bytes(bad)
        try:
            n.NativeImage(wrong)
        except ValueError as error:
            assert 'SHA-256' in str(error)
        else:
            raise AssertionError('accepted a changed target')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=True, cases=cases, input_sha256=hashlib.sha256(image.original).hexdigest(),
                  simulated='actual original and patched AArch64 instructions at two ASLR slides',
                  pointer_authentication='PAC operations modeled without target keys', hardware_verified=False)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Native routing, capability ABI and H4 shutdown: {cases} instruction cases passed at two ASLR slides')


if __name__ == '__main__':
    main()
