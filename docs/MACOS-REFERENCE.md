# The official macOS reference for this port

Everything on this page is copied or summarised from sources the project maintains, so nobody has to
re-derive it. The canonical pages are fetchable without a checkout; the pinned commit is what this
repository was checked against on 2026-11-17.

| Source | What it gives us |
| --- | --- |
| <https://unixnu.com/zh/devices/sheng-macos> (`docs/zh/devices/sheng-macos.md`, `docs/devices/sheng-macos.md` in a UniXNU checkout) | the official macOS port for the sheng SoC class, and which repository is authoritative |
| <https://github.com/Jelly10086/macOS-ForSheng> @ `4ec9620f127fe0daa7e9e9498ee7b0d374fd032f`, `docs/status.md` | the real implementation state: every stage that has passed, every failure that was located |
| <https://unixnu.com/zh/porting/log-capture> | how logs are meant to be collected on this class of device |

## What the official port has actually reached

Copied from the status table of the pinned commit (2026-10-25):

| Stage | State |
| --- | --- |
| Paired macOS 26.5 / 25F71 kernel + recovery userspace | original arm64e `launchd` running in QEMU **and on real sheng hardware** |
| Real hardware, filesystem | BaseSystem and the three ISC role volumes mount; temporary Data and template copy pass |
| Real hardware, services | recovery services keep starting; **WindowServer exits repeatedly** during HID and display init, so there is no graphical Recovery yet |
| Logs | written at runtime and read back from Recovery after a reboot (see `macoslog` below) |

Their older XNU-only milestones are not a substitute for this: the status document says so explicitly
("旧 XNU 独立验证 EL1 硬件交接，不能作为 macOS 26/27 用户空间的替代内核").

## Two pitfalls the official documents name, and where we stand

### 1. Root selection must use the APFS volume UUID, not the default HFS match

> 完整日志明确等待 IOMedia，默认匹配 Content=Apple_HFS；本次没有提供块设备，函数未返回 ... 下一段需接入真实只读块设备，并按已验证 BaseSystem 的 APFS 卷 UUID 选择根卷，不能沿用默认 HFS 选择。

`IOFindBSDRoot` waits for an `IOMedia` matching `Content=Apple_HFS` unless the payload supplies a
root-matching dictionary. That dictionary is `[payload.adt] root_match_volume_uuid`, which
`scripts/unixnu/devicetree.py:324` turns into `/chosen/root-matching`.

This board carried `04769D26-2CA0-4825-B443-E2E5F88300F2` until 2026-11-17 — the OS volume of **iPadOS
17.7.11 21H461**, the image the port started from, not a volume that exists on this unit. It is now
`84581E99-85BE-4957-AC53-2947E887B3E4`, the same value as `[root] os_volume_uuid` and upstream's
`[payload.adt] root_match_volume_uuid`, and the same value the signed policy records in
`scripts/policies/26C5035r/OS/verification.json` bind. A wrong UUID here fails silently and looks
exactly like "the block device never appeared".

### 2. A dedicated log partition is the officially maintained log path

The reference project added a **64 MiB FAT16 partition labelled `MACOSLOG`** (LUN 4, sde, LBA
448512–464895 at 4096-byte sectors) in free space at the tail, kept both GPTs and every existing entry,
verified it by writing and reading back a file from Recovery, and then had DarwinBoot write a bounded RAM
rescue into it before the next kernel load.

It is honest to record that this path is **not currently working for them either**: the later entries of
the same document report `macoslog` holding only `OBSERVED.JSON`, with no `NEXTLOG.BIN` and no `CAP`
directory, and the UEFI volume discovery not yet succeeding.

This port does not have that partition and does not propose to repartition a working unit for it. The
equivalent here is the kernel's own `UKELLOG1` window, which `package/UfsLog.cpp` writes into the **last
32 MiB of the inactive `boot_b` slot** (arguments `uke-log-first=256566 uke-log-last=281141`, read back
from Recovery with `dd` and `tools/decode-ukelog.py`). A dedicated FAT partition is the better long-term
home precisely because it is a filesystem rather than a raw window; it needs an explicit decision and a
GPT backup round, not a quiet change.

## Where this port deliberately differs

| Reference project | This port | Why |
| --- | --- | --- |
| dedicated `macosroot` partition, UFS component matching only that partition | `[userdata] windows` (OS / SystemOS / App) inside the userdata partition, plus signed root policies and `[root] tables_in_userdata` | upstream UniXNU's window model, which is what the signed-policy and userdata tooling is built around; the reference project's partition model needs its own UFS matching path |
| macOS 26.5 / 25F71 | macOS 27.3 / 26C5035r | the newer image is what this board installs; upstream's 27.3 profile is the one with the `[root]` digests we sign |
| `macoslog` FAT partition | `boot_b` tail window | see above: no repartitioning of a working unit |

## What to expect once the root mounts

Stages the reference reached on real hardware, in order, which is the checklist to read our own logs
against after the storage round:

1. APFS container identified, OS (BaseSystem) volume mounted;
2. the three ISC role volumes mounted;
3. temporary Data created;
4. template copy;
5. `launchd` starts and keeps starting recovery services;
6. from there, WindowServer is the next wall (HID + display), and it is a userspace service failure, not
   a kernel one.

## What the official device page's development record says about macOS

`https://unixnu.com/zh/devices/sheng` carries one 开发经验 block per area (`{#configuration}`,
`{#firmware}`, `{#cpu}`, `{#storage}`, `{#image}`, `{#diagnostics}`, `{#arm64e-os}` …). The items below are
the ones that bear on this port; the right column is where uke stands, so the next person can tell a
settled question from an open one.

### Configuration and identity

| Official lesson | uke |
| --- | --- |
| OS-profile source pins select the public macOS 26.5 families **without claiming binary ABI compatibility**. | Same stance: `profiles/os/macos-27.3-uke` pins `xnu-12377.121.6` as the closest public source to the 26C5035r kernelcache; the pin is a source identity, not an ABI proof. |
| Source storage/input archives are built separately and are not selected for boot in the minimal configuration. | `uke-macos_defconfig` follows exactly this: storage archives were absent until the policy directory existed. |
| Tables merge ARM64 → SoC → OS → device, and **arrays replace** inherited selections, so every required component and fragment has to be listed again. | This is why `[components] include` has to repeat everything, and why dropping `ShengUfs` from it really removed the driver. |

### Firmware and boot

| Official lesson | uke |
| --- | --- |
| The firmware DTB is an input to boot-image assembly; XNU consumes the separately generated ADT. | `[bootimg]`/`[device_tree]` vs `[payload.adt]` — and `grep _FVH boot.img` shows the FV while the ADT rides in the payload, exactly as described. |
| A configured menu entry needs a payload FAT containing `EFI/UniXNU/unixnu.toml`; when entries are missing, check the ramdisk block device. | Verified every round: `mdir -i vendor_boot.img@@4096 ::/EFI/UniXNU/` is how the flashed arguments are read back. |
| No extracted binaries live in any repository. | `board.toml [ipsw.sha256]` + `[firmware.image]` pins, nothing extracted committed. |

### CPU and interrupts

| Official lesson | uke |
| --- | --- |
| GIC register ranges must use the **translated physical addresses** from the hardware tree. | This is the same ground `work/re/docs/2026-11-14-gic-geometry-cliffs.md` covers (GICD `0x17100000`, GICR `0x17180000`, stride `0x40000` derived from `GICR_TYPER.VLPIS`). |
| Enabled CPU count and MPIDR affinities must be checked together. | The `cpu_start (cpu 0..7)` sequence is the check we have been reading off the panel. |

### Storage and root volume

| Official lesson | uke |
| --- | --- |
| An idle AHB branch may report `CLK_OFF` with hardware gating enabled — **check enable and gating bits together**. | Directly relevant: the firmware's USB dump prints `UKE-PIPE: cbcr=80000001 enable=1 halted=1`, which is this situation, not a fault. |
| SMMU bypass still has address-width constraints; validate every DMA range and context-bank assignment. | Open for our USB/DMA path. |
| The signed root-policy **headers** ship in `scripts/policies/<profile>` and the kernel archives pass `--policy-dir`; the **full block tables stay local inputs of the userdata image**. | Implemented here: `scripts/policies/26C5035r` is committed, the 130 MiB of tables travel as `[userdata] objects` (`OS.vroot` …) and are deliberately not in git. |

### Image assembly (the most macOS-specific block)

| Official lesson | uke |
| --- | --- |
| A run mounted the OS root and SystemCryptex, then **stopped because the payload omitted its Data seed**; the payload now refuses `CONFIG_XNU_OWNER_DATA_MOUNT` without a seed. | `scripts/unixnu/payload.py:53` carries that refusal, and it is exactly why `[payload.runtime]` is parked at `enabled = false` until a kernel is built with the matching `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`. |
| Fileset adapters carry `N_ABS` branches without split-segment records; XnuPrelink retargets them after placement and rejects destinations outside retained code. | `kits/XnuPrelink` is in this board's archive list for the same reason. |
| A packaged image panicked at `corecrypto_kext_start` because the recipe **omitted `--lto`** — and "packaging success with `--allow-missing` does not establish boot compatibility". | `CONFIG_XNU_BUILD_LTO=y` stays on in `uke-macos_defconfig`; a packaging success is not treated as boot evidence anywhere in this port. |
| The payload carries `kernel.macho.adt`; the boot image carries the firmware FDT. Keep the two inputs named explicitly. | `[payload.adt]` vs `[bootimg]`/`[device_tree]`, and the round's verification reads both separately. |

### Diagnostics

| Official lesson | uke |
| --- | --- |
| **A source implementation, a host check and a device observation establish different completion levels.** | The discipline this port needed and did not always follow: b256 was flashed on source review + a host format check, and a host *compile-and-run* check (now `tools/check-adv-log.py` self-test 7) is what would have caught it. |
| Keep an unresolved cause explicit and record the next measurement that can distinguish it. | Each round's notes say what the next boot must show, rather than asserting a cause. |
| Their logs carry tagged rows (`[adsp]`, `[audio]`, `[battery]`, `[dpu vig0]`, `[gpu wptr]` …) so a log read can be filtered by subsystem. | Adopted for the kernel log channel: `UKE-ADVLOG`, `UKE usb: beat #N stage=`, `lck-gate:`, `ucle-stall-probe:` are all greppable prefixes. |

### arm64e

| Official lesson | uke |
| --- | --- |
| SM8550 implements FEAT_PAuth and signatures are created on the device, so **the PAC algorithm need not match Apple's**. | Same SoC class, same conclusion. |
| Public XNU programs PAC keys only for VMAPPLE through HVC; the arm64e profile provides native key programming. | `profiles/arch/arm64e` is what this board builds on. |
| The macOS dyld cache is arm64e and contains SME code, which SM8550 does not have. | A userspace-stage ceiling to expect, not a kernel one. |
