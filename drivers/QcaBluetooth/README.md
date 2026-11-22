# QcaBluetooth

Qualcomm WCN7850 Bluetooth driver for XNU. It runs the Linux `btqca.c` startup
sequence over a GENI UART and connects the controller to Apple's `bluetoothd`.

## Status

Device: Xiaomi Pad 6S Pro (sheng), iPadOS 27.0.1 (24A446).

| State | Features |
| --- | --- |
| Working | Pairing, reconnection after reboot, power toggle, A2DP playback, AirPods noise control and battery, use alongside Wi-Fi |
| Not tested | Calls and microphone (HFP/SCO), sleep and wake |
| Unavailable | Features that need Apple vendor commands |

## Layout

| Path | Contents |
| --- | --- |
| `include/qbt.h`, `src/qbt_core.c` | Linux initialization, H4 framing and IBS on one executor |
| `include/qbt_geni.h`, `src/qbt_geni.c` | GENI SE DMA: four 2048-byte RX buffers, one 4096-byte TX buffer |
| `include/qbt_transport.h`, `src/qbt_transport.c` | UART.SK kernel pipe, backpressure, channel lifetime and worker power requests |
| `src/qbt_linux.c`, `shim/` | Boundary to the locked Linux sources |
| `userland/BlueTool.c` | `com.apple.BlueTool` XPC bootstrap service |
| `userland/Type0.c` | Capabilities from HCI reports for the native stack |
| `tools/` | Native daemon patch, signing, source lock and checks |

The board supplies WCN power, clocks, pins, IRQs, mapped DMA addresses and
firmware. Pairing, key storage, BLE and audio profiles run in `bluetoothd`.
[Native routing](native-ios27.md) describes the iPadOS 27 adapter.

## Kernel interface

| Sysctl | Access | Meaning |
| --- | --- | --- |
| `kern.qbt_boot` | Read | Controller state |
| `kern.qbt_power_on` | Read | Reading starts the controller and returns the result |
| `kern.qbt_firmware` | Read | Loaded firmware name |

`kern.qbt_power_on` is a read because BlueTool's sandbox denies sysctl writes.

The optional `qbt_platform.trace_hci` callback records discovery commands after
the UART accepts their complete frame, plus controller status, Inquiry Complete,
Inquiry Result and Remote Name events. Frames retain their original bytes.
ACL, audio, pairing keys and unrelated vendor payloads are excluded.
The callback runs on the protocol executor and must not block it.

On sheng, `log-bluetooth=3` enables a bounded trace in the persistent boot log.
`[bluetooth hci]` rows contain sequence numbers, monotonic nanoseconds, direction,
H4 type and hexadecimal bytes. `TX_UART` means UART acceptance; controller
acknowledgement is a separate `RX_CONTROLLER` event. `missed` and `dropped`
report trace losses. Read the userdata tail from Recovery after the test.

## Checks

```sh
python3 tools/test-runtime.py
python3 tools/test-geni.py
python3 tools/build-bluetool.py
python3 tools/build-type0.py
python3 tools/check-kernel.py /path/to/tree --record <kernel source compile record> \
    --extra-source <board source> --board-export <symbol>
```

`tools/prepare-linux.py` verifies the source hash and stages byte-identical Linux
code in `out/linux`. `QBT_LINUX_TREE` selects an existing Linux checkout.

## Sources and licenses

Linux initialization and GENI/H4/IBS code use GPL-2.0-only. `src/qbt_transport.c`
uses APSL-2.0, and `userland/Type0.c` uses BSD-3-Clause. Public interfaces and
BlueTool use BSD-2-Clause-Patent. Each file identifies its license;
[NOTICE](NOTICE.md) and `tools/source-lock.json` record attribution.

Vendor firmware and Apple binaries are external inputs and stay out of this
repository.
