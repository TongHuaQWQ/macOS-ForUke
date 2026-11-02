# arm64e on the Xiaomi Pad 7 (uke)

The macOS track builds XNU as **arm64e**, and this page is the record of why that is possible on
non-Apple silicon: the hardware check on the unit, the four requirements of
`docs/porting/arm64e.md`, and the point at which the macOS image itself forces the ABI.

Measured on the retail unit `2410CRP4CC` (Xiaomi Pad 7, `uke`, SM7675), Android 16 build
`OS3.0.302.0.WOZCNXM`, SukiSU root, under `Running under Gunyah hypervisor 51/v1`.

## 1. The hardware implements pointer authentication

| Check | Command | Result |
| --- | --- | --- |
| feature bits | `adb shell 'grep -m1 Features /proc/cpuinfo'` | `paca` and `pacg` on **8 of 8** CPUs |
| cores | `adb shell 'grep -c paca /proc/cpuinfo'` vs `grep -c ^processor` | 8 vs 8 — 3× `0xd80` Cortex-X4, 4× `0xd81` Cortex-A720, 1× `0xd82` Cortex-A520 |
| algorithm | `adb shell 'su -c "dmesg \| grep -i authentication"'` | `Address authentication (architected QARMA3 algorithm)` and the same line for `Generic authentication` |
| EL1 enablement | `adb shell 'su -c "zcat /proc/config.gz \| grep ARM64_PTR_AUTH"'` | `CONFIG_ARM64_PTR_AUTH=y`, `CONFIG_ARM64_PTR_AUTH_KERNEL=y` |

The last row is the one that matters under the hypervisor XNU will run under: Android's kernel
already writes the SCTLR_EL1 enable bits (EnIA/EnIB/EnDA/EnDB) and the five key registers, and boots
normally, so those writes do not trap.

## 2. The instructions are not hints here

`scripts/check-arm64e-pac.c` is the EL0 half of the check, built freestanding for `aarch64+pauth`
and run on the device. It signs `0x20023c` with `PACIA` and reads it back with `AUTIA`:

```
cores=8 pacia_changed=8 autia_restored=8
0x20023c -> 0x68fe000020023c -> 0x20023c
```

A CPU without the feature executes `PACIA` as a hint and leaves the value alone, so *a changed
pointer* is the observation that proves the feature is live. All eight cores produce the same
signature for the same input and modifier — one key set shared by every CPU, which is what the
`profiles/arch/arm64e` layer assumes (it loads a single random key set per boot and keeps the
EL0/EL1 keys shared, so process isolation through PAC is deliberately absent).

The algorithm is architecturally QARMA3 rather than Apple's own. That does not matter: no signature
is computed off the device. `dyld` signs user pointers when it loads an image and XNU signs the kernel
collection during early boot, so the CPU only has to be self-consistent.

## 3. The macOS image forces the ABI

`kernelcache.release.mac17g` — the KernelCache entry of the `j704ap` identity in `board.toml`
(`Mac17,2`, build 25F84) — is an `MH_FILESET` with 386 load commands, `cputype 0x0100000c` and
`subtype 0xc0000002`: low byte 2 is `CPU_SUBTYPE_ARM64E` and bit 31 is the PTRAUTH ABI flag. An arm64
kernel cannot host it, and upstream states the rule plainly: iPadOS 26 / macOS 26 and later are
arm64e only.

So `board.toml` selects `os_profile = "macos-26.5.2-25F84"`, whose `arch = arm64e` resolves to
`profiles/arch/arm64e` instead of `profiles/arch/arm64`, and `scripts/build-xnu-4k-baseline.sh`
passes that value to both the XNU build (`ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG`) and
`build-firehose.sh` (`UNIXNU_XNU_ARCH`), so the kernel and the firehose stub agree.

## 4. What still has to be repeated on the first XNU boot

XNU programs the keys itself in `PAC_INIT_KEY_STATE`, with a key derivation (splitmix64 over
`CNTPCT_EL0` on the first CPU) that is not Android's, so requirements 2 and 3 of
`docs/porting/arm64e.md` have to be observed again there: the boot has to reach panic or console
output without a PAC trap. `configs/uke-macos_defconfig` is the configuration for that run, and the
three-argument `PACIA` forms plus the JOP/GA keys stay unexercised until a userland image boots.
