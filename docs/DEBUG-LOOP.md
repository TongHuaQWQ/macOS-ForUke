# The debug loop we actually have (uke, SM7675)

How to flash, what the panel is telling you, and which read-out channels are proven. Written from the
2026-11-11 session; every claim carries the file, log line or commit behind it.

The short version: **the panel is the console**. The kernel has no serial port, a USB console that does
not enumerate, and a persistent log that is half-built. A photograph of the panel, plus the firmware's
own QR export, is what actually reads a failure today.

---

## 1. The loop, in one screen

```sh
# device in fastboot (power + volume-down, or `adb reboot bootloader` from Android)
fastboot flash vendor_boot_a work/flash/vendor_boot_round-b<NNN>-<name>.img   # the payload: kernel.macho + ADT
fastboot boot            work/flash/boot-round-b<NNN>-<name>.img              # the firmware: booted into RAM, never written

# read the panel. If the kernel panicked and you need the address:
python3 work/analysis/symbolize-panic.py --artifact work/ci-artifacts/b<NNN> --disasm 0xfffffff00c0321e4

# back to stock, always available:
fastboot flash vendor_boot_a device/uke-backup/os4/on-device-os4-partitions/vendor_boot_a.img
```

Getting a round in the first place is one command: `python3 work/round.py <RUN_ID> --name b<NNN>-<name>`
fetches the CI artifact, verifies it, builds the payload, copies both images into `work/flash/`, inserts
the row into `work/flash/INDEX.md`, and prints the two `fastboot` lines above (`work/round.py:1-17`).
`--kernel <path> --name …` re-builds the images only, with no download.

### The two facts that make this loop cheap

1. **The stock `vendor_boot_a` is on this machine, dumped from this unit.** The tablet was upgraded to
   HyperOS OS4 on 2026-11-11 and Xiaomi's public download for `uke` still sits at OS3.0.306, so the
   backup at `device/uke-backup/os4/on-device-os4-partitions/` is the only copy of the release this
   hardware runs: 47 `.img` files plus `SHA256SUMS` and `device-state.txt`
   (`device/uke-backup/os4/on-device-os4-partitions/MANIFEST-os4.md`). `vendor_boot_a.img` there is
   100,663,296 B, sha256 `666ef5d9cc3b77a8…` (verified with `sha256sum` against the directory's
   `SHA256SUMS`). A payload that will not boot is therefore one `fastboot flash`
   away from stock — not a reinstall, and not a risk.
   `work/round.py:143-150` prints both restore targets and prefers the OS4 one ("matches this unit");
   the tail of `work/flash/INDEX.md` still names the older OS3 dump, so trust `round.py`.
2. **The firmware is booted, not flashed.** `boot.img` is the UEFI FV + BootShim + DTB and
   `fastboot boot` puts it in RAM only (`work/flash/INDEX.md:3-4`). A broken firmware costs a power
   cycle, not a recovery. Only `vendor_boot_a` is written to flash — that is the single persistent
   change per round.

What that buys, measured: the payload is rebuilt locally in about 6 minutes with no CI at all
(`docs/STATE.md:109-110` — it is `tools/build-payload-linux.sh`, kernel plus local UEFI plus DTB), a
firmware-only change is about a minute (`docs/STATE.md:117-118`), and a kernel change is the expensive
one at 25–35 minutes of CI (`docs/STATE.md:99`). **Change the ADT or the boot arguments alone and you
pay 6 minutes, not 35.**

---

## 2. Reading the panel

Order matters — these appear in this order on a boot.

| what you see | what it means | proven? |
|---|---|---|
| UEFI menu, countdown, `Android`/`UniXNU` entries | the firmware is up; `fastboot boot` delivered it | proven, every round |
| `gic-probe: GICD_PIDR2 … / wrote 0x… / MPIDR_EL1 … / gicr +0x00000 … waker …` | the loader's own GIC probe: distributor writable, this core's affinity, all eight redistributor frames and their `GICR_WAKER` values. **Keep this line**: if the kernel dies before it can print, these are the only GIC facts on the machine | proven (`work/analysis/2026-11-11-b125-photo-triage.md` §1, photographed) |
| `UKE-GCC:` / `UKE-QUD:` lines | the firmware's USB inheritance evidence: `gdsc_powered`, `usb_clocks_running`, DWC3 `GSNPSID/GCTL/PHYCFG/DCFG`, halt state | proven (`docs/STATE.md` §62, §79) |
| `SecurityLock::LockType: SOFTWARE_LOCK …` and `ExitBootServicesCallback: Failed to Locate SCM Protocol!` | normal audit output of `CapsuleRuntimeDxe`, and a real but parked defect (`PROBLEMS.md` row 17). Neither is why a boot failed | proven (b125 triage §1 items 4-5, §3) |
| `DARWINBOOT: hand-off to the kernel` | the loader handed off; anything after this is kernel-side | proven |
| a **green bar** at the top, then a **blue bar** | green = the loader's hand-off marker; blue = XNU's video console running. Blue means the kernel is executing and can format a panic | proven (b125 triage §2) |
| `Unhandled platform error: esr 0x… far 0x… pc 0x… ec 0x… cpu …` — the text was `Unhandled implementation specific error: esr … far … pc …` before `bc05f07` shortened it | an XNU platform error. `ec 0x25/0x24` = data abort (read `esr & 0x1ffffff` for the fault status: `0x10` = a device refused the access, `0x18` = parity/ECC, `0x21-0x23` = table walk); `ec 0x2f` = SError, asynchronous, so `far` is a hint and `pc` is what places it. `cpu != 0` means a secondary core, not the boot CPU | proven (b127; the line is `patches/xnu/uke-serror-detail.patch`, ~108 characters to fit one panel row and the firmware's 128-byte `PanicLine`) |
| `uke-gic: …` / `uke-gic: GICD_CTLR.ARE stays 0 (0x…)` | the GIC block's own account of what it refused to do. A refusal here costs a timer that never ticks, not a panic | designed by `f8491f2`/`bc05f07`; **no boot has printed either branch yet** |
| `--- pram head: … ---`, `pram slot 0x…`, `pram find "…"`, `pram first text: …` | the firmware reading the **previous** boot's window, before this one can overwrite it. `<none: DRAM did not survive, or the kernel never wrote>` is the firmware's own summary line | channel proven to run; a **record read back from `0x9FFE00000` has not been observed yet** (`PROBLEMS.md` rows 3 and 5) |
| `UKEMEM: …` / `last panic: …` | the firmware's memory map and the menu's one-line panic summary. The line exists (`Library/UniXNULib/Main.c:135-136`, `Ui.Info = L"last panic: %a"`); only design-note examples of it carrying an XNU panic exist, none photographed | **not proven** |
| `UKE-PREV:` stage list, `UKE-WDOG:` handoff decision | the firmware's own cross-reset stage log at `0xA7605000` and its hand-off watchdog. Both are in `workspace/uefi-uke` only as uncommitted work (`PROBLEMS.md` row 22), and neither is proven across a real reset | **not proven** |
| QR card (`PH / E / CR / G / EV / TA / TB`) plus a text page | the kernel's OneSend frames: 60 bytes per frame, paged with the volume keys | proven (b96 was decoded and gave `PHASE=18 ERR=13`, `docs/STATE.md` §85) |
| `Failed to find an Operating System`, or nothing | after the menu's countdown with no payload, the bootloader has nothing to start — that is `fastboot boot`'s one-shot nature, not a failure | proven (`docs/10-M1验收与M2计划.md:24`) |

---

## 3. Channels, and which of them you may trust

| channel | how to read it | state |
|---|---|---|
| **Panel photograph** | phone camera; crop the region of interest | **proven.** The channel that has never lied: it read the GIC share of b125, the hand-off in b125, and the ESR/FAR/PC in b127 that solved `PROBLEMS.md` row 1 |
| **Panel QR export** | `python3 work/analysis/log-capture/panel_qr_decode.py`, frames paged by the volume keys | **proven** (b96). Bandwidth is 60 bytes per frame; it is for structured fields, not for a log |
| **Firmware pram read-back** | menu `last panic:` line, or the `pram …` block the firmware prints on the next boot | **half.** The reader now looks at both windows (`0x9FFE00000` first, `PcdUkePramBase 0xFF800000` second, `UkeBootMemoryDxe.c:615-630`), but no record has been read back since. Retention is the open half: pram is reliable only across a warm reset, and the routine power cycle here is a 10-second power-key hold (`docs/RECOVERY-LOG.md:90-91`) |
| **userdata ring** (`UKELLOG1`, 16 KiB records, last 32 MiB of userdata) | boot stock Android, `fastboot reboot recovery`, `adb exec-out dd` from the window, `bash work/analysis/log-capture/read-ukelog-twrp.sh`, then `python3 tools/decode-ukelog.py dump.bin -o boot.log` | **unproven on this branch.** The protocol and the decoder are proven on another branch's dumps (211/43 records, all magic+CRC valid, `docs/PROGRESS.md:531`), the writer is now compiled in, and both boot-arg geometries (`uke-log-first=2961408`, `uke-log-last=61949946`) match the live partition — but no read-back from this branch is recorded (`PROBLEMS.md` row 18) |
| **pstore / ramoops** | `adb shell su -c 'cat /sys/fs/pstore/*'` after booting Android | **disproven.** b79 published the record and Android never exposed it (`work/flash/INDEX.md`, row "已证伪"). Do not spend a round on it |
| **USB CDC-ACM terminal** | `scripts/debug_terminal.py --port /dev/ttyACM0` on the host | **does not enumerate** (`-71`/`-110`, `docs/STATE.md` §92). This is the port's own blocker, `PROBLEMS.md` row 4 |
| **UART** | — | **absent.** `serial=3` is set in `board.toml:408` and there is no accessible UART on this tablet (`UkeUsbLogDxe.c:3`), which is why the documented kernel-console path is a camera here (`work/analysis/log-capture/PLAN.md` §1.1) |
| **adb** | `adb shell …` | Only in Android or TWRP, never under XNU. `./tools/probe-uke.sh` from the workspace root (`/home/tonghua/Code/mcOS/tools/probe-uke.sh`, `docs/STATE.md:127`) uses it for hardware facts (GIC, timer, `iomem`, retention pages) before a round |

Two consequences worth stating plainly. First, **anything that has to survive the reset must be read
from Android or TWRP**, because the device has no adb under XNU. Second, **the cheapest decisive
experiment on this board is usually a payload rebuild, not a log channel**: the ADT and boot arguments
are local (6 minutes), so "does the fault follow this address" is cheaper to answer by moving the
address than by improving the read-out.

---

## 4. Reading a pc: the collection has no slide

The prelinked kernel collection keeps the addresses it was linked at, so a printed `pc` symbolises
directly — no KASLR slide, no offset to guess (`f8491f2` commit message; `work/analysis/2026-11-11-gic-are-root-cause.md` §3).

```sh
llvm-nm --defined-only -n kernel.macho              # 118,444 symbols on the b127 collection
python3 work/analysis/symbolize-panic.py --artifact work/ci-artifacts/b<NNN> --disasm 0xfffffff00c0321e4
```

The method behind the script, when it has to be done by hand: `kernel.macho`'s top-level segment table
gives VA → file offset (`__TEXT_EXEC` @ `0xfffffff008954000` = fileoff `0x1950000`), so any function body
can be sliced out and fed to `llvm-mc --disassemble` as `0x…` words. `llvm-objdump` cannot handle an
`MH_FILESET` as a whole; `symbolize-panic.py` writes a one-section ELF for the slice it needs
(`symbolize-panic.py:12, 120-141`). A `lr` that looks like a wild pointer is usually PAC-signed — drop
the high bits and it resolves (`b127`: `lr 0xbced7ef00c031fe4` → `_uke_gic_arm_cpu+0xc0`).

---

## 5. Round hygiene that has already cost rounds

| trap | symptom | what to do |
|---|---|---|
| `tools/build-payload-linux.sh` packs the **existing** FV | `boot.img` byte-identical to the last round although firmware sources changed | rebuild the FV: `workspace/uefi-uke/.venv-edk2/bin/python3 build_uefi.py -d uke -r DEBUG`; confirm with `boot.img`'s header `kernel_size`, not with `strings` (FVMAIN_COMPACT is LZMA) — `PROBLEMS.md` row 14 |
| EDK2 skips a module whose mtime did not move | the edit is in the tree, not in the binary | `touch` the source, rebuild, then `grep -c '<new string>' …/OUTPUT/<Driver>.efi` |
| EDK2 `DEBUG((…, "…"))` strings are **ASCII** | searching for UTF-16 finds nothing and looks like the patch did not apply | only `Print(L"…")` is UTF-16 |
| a kernel in a directory that also holds `kernel.macho.sha256` | payload build refuses with `sha256 mismatch` | give the kernel its own directory, or write the matching `.sha256` |
| boot arguments over 1000 bytes | the payload build truncates or the command line is rejected | currently 987 of 1000 (`docs/STATE.md:141`); delete before adding |
| two device patches editing the same file | `patch does not apply` and a build that fails in step 0 | enable one at a time, or rebase first (`docs/STATE.md:138`) |
| one macOS runner at a time | the next round queues behind the previous one | push one commit at a time |
| a CI failure nobody reports | a round is built on a stale assumption | `python3 work/ci-heartbeat.py --minutes 90` (exit 0/1 on the last run's conclusion) — `PROBLEMS.md` row 13 |

---

## 6. What "the round worked" looks like

A good round is not "no panic screen". In order, the checkpoints this port has actually reached:

1. `DARWINBOOT: hand-off to the kernel` on the panel — the loader is not the problem.
2. A blue bar — XNU is executing, not hung in the loader.
3. No `Unhandled …` line, and either a QR card or the kernel console continuing.
4. `uke-stall-probe: samples` non-zero on a later boot — the timer is finally ticking
   (`PROBLEMS.md` rows 1, 2, 7). Not observed yet in any round since `uke-gic-timer.patch` came back
   into the series; the only evidence that this kernel can get further at all is older and from a
   different configuration — `security policy loaded: AppleMobileFileIntegrity` on screen
   (`docs/STATE.md:56`).
