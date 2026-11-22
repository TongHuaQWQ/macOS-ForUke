# iPadOS 27 native adapter

The adapter targets the original arm64e `bluetoothd` from 24A446:

- SHA-256: `cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1`
- UUID: `8E49197B-E6BC-3F3B-A8D3-DF4BEDD37F01`

`tools/patch-ios27.py` creates the patched executable and a JSON report.
`tools/build-type0.py` builds and signs `/usr/lib/QcaBTType0.dylib`.
`tools/build-userland.py` signs the native daemon with its original identifier
and entitlements, then stages BlueTool, bluetoothd and the capability library.
The board runtime builder installs their files, firmware and signed launchd
cache in the Data addon. Apple binaries remain external inputs.

## Routing

Every product reaches the original constructor with controller type zero, and
a product no factory branch lists falls through to the last constructor. The
daemon's virtual machine check returns false, so chip initialization runs.
Registration accepts zero and preserves the native 1 through 6000 range. Type
zero clears the vendor operation table, four vendor hooks and both vendor
opcodes. Both callback registration paths retain their generic callback.

Optional command 0x0c5b follows byte 18, bit 3 of the native support bitmap;
unsupported controllers advance through the native initialization chain. Host
flow control and erroneous data reporting may be rejected. H4 pre-stop skips its
empty extension and retains normal transport close.

If the keychain does not answer, root keys stay in memory for the daemon's
lifetime instead of restarting it.

## Scanning and addresses

Extended scanning uses 0x2041 and 0x2042, because the controller rejects legacy
scan commands once extended advertising commands have been used. Apple's scan
extension is off.

A random address (0x2005) is queued before a scan can be enabled. Starting an
advertising set always sends its address (0x2035). If no resolvable address can
be generated, the refresh uses a non-resolvable address and completes normally.

## Capabilities

The adapter reads Supported Commands and LE Features. Extended scanning,
connection, advertising, periodic advertising, sync transfer, coded PHY and
2M PHY follow the controller's reports. Both LE feature callback versions
preserve their original status and data pointer for capability publication.

An asynchronous LE Read Number of Supported Advertising Sets query supplies the
broadcast capacity. The count is capped at the native array's six records.
Query failures reach the native error path. Model getters, configuration caches
and lower stack flags receive the same capabilities. The broadcaster refreshes
its mode before native GAP initialization, including after reopen.

The library runs during initialization. Normal packet traffic stays in the
native stack and UART.SK. The eleven replacement getters use four instructions
each. Dyld signs their USERLAND24 rebases with the original keys and diversity;
object vtable pointers remain intact. Callback and broadcaster entry patches
retain their original PACIBSP landing pads. Native function calls use signed
IA/0 pointers. The layout follows Apple's
[dyld header](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/fixup-chains.h).

## Runtime interfaces

The board exports /arm-io/bluetooth and a matching IOService named bluetooth.
Transport encoding 1 and speed 3200000 are four-byte little-endian properties;
the stable private address appears as local-mac-address. Public addresses use
network order; the Linux adapter converts to bdaddr_t before calling btqca.
This follows the address representation in
[BlueZ](https://github.com/bluez/bluez/blob/5.79/lib/bluetooth.c).

Read-only unions expose the signed capability library under /usr/lib and both
executables under /usr/sbin. The library mounts first. BlueTool retains its
original Mach service and program path, runs as root and keeps transactions
active. Its launchd job disables pressured exit. It starts the controller by
reading `kern.qbt_power_on`; connecting to UART.SK also starts it. A controller
already powered off acknowledges the sleep request successfully. Other native
jobs are preserved; Wi-Fi and Bluetooth share one signed cache and one appended
trust module.

## Build and verification

From the component checkout, with the original binary at `$BLUETOOTHD`:

```sh
python3 -m venv out/native-venv
out/native-venv/bin/pip install -r tools/requirements-native.txt
out/native-venv/bin/python tools/test-native.py "$BLUETOOTHD"
python3 tools/test-type0.py "$BLUETOOTHD"
python3 tools/build-type0.py
python3 tools/build-userland.py out/runtime --bluetoothd "$BLUETOOTHD"
python3 tools/patch-ios27.py "$BLUETOOTHD" --output out/ios27/bluetoothd.native
```

The 165 instruction cases execute native routing, feature capture, authenticated
call boundaries, getters, lower setters and H4 pre-stop at two ASLR slides.
PAC operations are modeled without device keys. The production C publisher
passes 58 HCI and policy cases under ASan/UBSan, including command and feature
absence, capacity limits, failed replies, submit errors and reopen mode changes.
The arm64e library compiles and passes code-signature verification. Reports
reside in `out/native-test`, `out/type0-test` and `out/userland`.
