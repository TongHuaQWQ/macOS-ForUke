# XnuPrelink (board copy)

XNU kernelcache prelink and ABI inspection. This is the **board copy** of the kit
`TongHuaQWQ/XnuPrelink` (fork of `https://github.com/UniXNU/XnuPrelink.git`), kept inside the
`macOS-ForUke` device repository because it is the copy the uke build actually runs.

## 1. What this is

A Python kit that pairs a **stripped Apple kernelcache** with **our own symbol-bearing rebuilt
kernel** and emits the kernel collection that becomes the boot image's `kernel.macho`. For an arm64e
`MH_FILESET` kernelcache, `emit_prelink.py` selects the fileset emitter: `fileset_match.py` derives a
function/data pairing, `fileset_layout.py` and `fileset_segmented.py` place the native segments in the
space the replaced kernel and the omitted entries free, and the result is a kernel collection whose
root is the native kernel and whose remaining entries are the selected Apple kexts plus their
dependency closure. The ordinary (non-fileset) ARM64 prelink interface remains available.

It is a **prelink-stage, build-time** tool: it sits between the XNU link and the payload. It produces
no runtime component of its own — its output is the kernel image the loader hands to XNU.

Why the fork exists: macOS 26 ships no KDK, so there is no symbol-bearing build of the original kernel
to compare against. Pairing therefore needs hand-reviewed identities per kernelcache build
(`fixtures/fileset-<build>-symbols.json`) and a record of which call site wanted which unnamed kernel
function. Upstream has neither.

## 2. Where it sits

A copy, not a checkout: this directory is tracked by the device repository itself (no `.git` of its
own), so it can be edited by whoever is reviewing anchors for this board.

| Consumer | Pin | Where |
| --- | --- | --- |
| device `TongHuaQWQ/macOS-ForUke` | `[[source]] name = "XnuPrelink" kind = "kit" local = "kits/XnuPrelink"` (no git pin — `local` resolves before the core's submodule, because `feeds.py` is first-wins) | `feeds.toml` |
| device `board.toml` prelink step | `[kernel.prelink] tool = "kits/XnuPrelink/emit_prelink.py"`, `kernel`, `kernelcache_sha256`, `roots`, `slide` | `board.toml` |
| core `TongHuaQWQ/UniXNU` (upstream of this copy) | submodule `kits/XnuPrelink` → `https://github.com/TongHuaQWQ/XnuPrelink.git`, catalog `revision = "a7714579cb9f1243a74abe386dc18a5174da881e"`, `path = "kits/XnuPrelink"` | `UniXNU/.gitmodules`, `UniXNU/components.toml` |

Why a copy and not a pin: `feeds.py` resolves sources first-wins, and a `git` entry cannot override a
component the core already carries as a submodule, so a `git` pin for `XnuPrelink` silently never takes
effect. The anchors have to live in a writable tree anyway — the matcher reads them from
`Path(__file__).parent/'fixtures'`. `kits/NativePrelink` is a compatibility symlink to this directory
for the audit scripts that hardcode the legacy name; `board.toml` names the canonical path.

What it links into: the kernel collection (the payload's `kernel.macho`), not a static archive.

## 3. Dependencies

| Input | Comes from | Without it |
| --- | --- | --- |
| Python 3.11+ (standard library only — `struct`, `json`, `gzip`, `hashlib`, `hmac`, `plistlib`, `xml.etree`, `array`, `bisect`) | Host (the macOS runner of `build-kernel-min.yml`) | Nothing runs. No third-party dependency, no compiler step. |
| A decompressed `MH_FILESET` kernelcache | The 25F84 universal IPSW (`[ipsw]` in `board.toml`), resolved through `unixnu ipsw fetch`; pinned by `[kernel.prelink] kernelcache_sha256 = "697e01d6…"` (`kernelcache.release.mac17g`) | No original collection to pair against; a wrong pin only fails at fetch time. |
| The linked native kernel **with symbols** | `unixnu build kernel` (the path in `[kernel.prelink] kernel`); CI ships it as `native/native.macho` in the `uke-kernel-min` artifact | Nothing to emit. |
| A symbol-bearing comparison kernel | `build-fileset-release-reference.py --tree . --lto`, the last entry of `board.toml` `[kernel] archives` | Fewer names transfer; the matcher's `pac-checked-shape` anchors collapse (113 → 1 in round b2 without it). |
| The shipped naming table `fixtures/reference-names-25F84-kernel.release.t8142.json.gz` | Generated once outside the repository from a symbol-bearing kernel, then committed here | Same-build naming evidence is lost: a public tree cannot hold Apple's per-build symbol evidence any other way. The map records the table hash and is refused if a different table is supplied. |
| Reviewed anchors `fixtures/fileset-25F84-symbols.json` (**197** functions; the core copy of this kit carries 60) | **Hand-made** from identification evidence (function hashes, `__PRELINK_INFO`, sequence alignment, `vdesc_name`, vtable slots, `declared_names()`). Apple publishes no KDK for macOS 26, so this is not obtainable from any public source | Ambiguous functions stay unbound and are reported as missing. |
| The kernelcache's own `com.apple.kernel` symbol table | Inside the kernelcache (`declared_names()`) | `--root`'s closure cannot resolve *data* names, and `unresolved closure references` stays non-zero. |

`fixtures/fileset-23F84-symbols.json` and `fileset-24A446-symbols.json` cover the earlier kernelcache
builds the same board was measured on. Successful emission and the structural checks do **not**
establish ABI or boot compatibility: pairings are inferred from stripped code.

## 4. Build and use

Nothing to compile. With this directory as the working directory:

```sh
python3 -m unittest discover -p 'test_*.py'      # 70 tests, incl. test_reference_names.py
```

As the driver runs it (from `board.toml` `[kernel.prelink]`, inside the resolved build tree where this
directory appears as `kits/XnuPrelink`):

```sh
python3 kits/XnuPrelink/emit_prelink.py \
    --kernel <build/xnu/…/mach.development.bcm2837.unstripped.noctf> \
    --prelinked <decompressed kernelcache.release.mac17g> \
    --root <com.apple.bundle.id> [--root ...] \
    --slide -0x1000000 --output <kernel.macho> \
    [--fileset-map FILE] [--fileset-reference FILE] [--allow-missing]
```

Maps carry a matcher version as well as the input hashes; both must match before a map is reused, and
both emission entry points reject maps produced by outdated matching code. Missing mappings stop output
and are listed in the JSON report beside the requested image; `--allow-missing` explicitly permits an
experimental image with unresolved references.

The standalone matcher and the lower-level emitter are the same interface as the core copy:

```sh
python3 fileset_match.py <decompressed MH_FILESET kernelcache> <native arm64e kernel with symbols> \
    [--entry com.apple.kernel] [--out pairing.json] [--reference-kernel FILE] [--max-rounds 40]
python3 fileset_emit.py --map FILE --output FILE [--report FILE] [--root BUNDLE_ID ...]
```

Shipping `native/native.macho` in the CI artifact is what makes this usable locally: the matcher runs
on the host with that file plus the kernelcache, so writing an anchor no longer costs a 35-minute round.

## 5. See also

- [Port a device](https://unixnu.com/zh/porting/steps) — step 2 (shared support and prelink ABI work)
  and step 7 (build and boot);
- [Prelink concept](https://unixnu.com/zh/concepts/prelink), [payload concept](https://unixnu.com/zh/concepts/payload);
- [XnuPrelink component page](https://unixnu.com/zh/drivers/xnu-prelink) and the
  [component catalog](https://unixnu.com/zh/reference/components);
- device records: `../porting.toml`, `../docs/STATE.md`, `../board.toml` `[kernel.prelink]`,
  `../KERNEL-BUILD.md`; the kit's own fork: `TongHuaQWQ/XnuPrelink`.

## Behaviour notes

The fileset emitter places the collection root at the native kernel's link address, marks the copied
kernel as a fileset member, rebases metadata and keeps only the selected roots and their dependencies
in the fileset index. The legacy `--slide` value does not apply to filesets. Rebuilt chains use virtual
offsets, including across native BSS gaps.

`fileset_layout.py` places native segments in space freed by the replaced kernel and omitted entries.
Code follows retained kext code; constants and data precede their kext ranges as required by XNU's
`arm_vm_init`. `fileset_segmented.py` applies native split-segment v2 records, rebuilds root fixups and
function starts, and recomputes corecrypto's HMAC over relocated code. Segment collisions and
unsupported relocation kinds stop emission. The native input executable is unchanged.

Maps carry a matcher version as well as input hashes. The integrated CLI regenerates outdated caches.
Reviewed symbol anchors in `fixtures/` include function hashes and identification evidence. Other
matches use code, data operands and identified vtables. A `.cold` error path is not substituted for its
parent function. Link order alone does not bind ambiguous functions. The comparison recognizes complete
compiler-inserted PAC failure checks and preserves branch geometry when comparing bodies. Native code
keeps those checks. Both emission entry points reject maps produced by outdated matching code.
`emit_prelink.py --fileset-reference FILE` accepts a symbol-bearing comparison kernel, such as a
RELEASE build with LTO. Unique code matches may supply an unambiguous symbol name in the actual native
kernel. Reference addresses are never emitted; compiler-generated names and graph-only guesses do not
transfer. The map records the comparison image hash. The standalone matcher calls this option
`--reference-kernel FILE`.

The same two options also accept the comparison kernel written down as data:
`reference-names.json[.gz]`, one `(shape digest, length, symbols)` row per function start of the
comparison in address order. Function *i* of a same-build entry is function *i* of that kernel, so the
row position replaces the address and the file holds no code, no offset and no address. It is generated
once from a symbol-bearing kernel (`tools/build_reference_names.py` in the UniXNU measurement
directory) and shipped as a fixture, which is how a public repository can carry Apple's same-build
naming evidence at all. Names are bound by digest and length and then by position, both followed by the
native kernel's own unique symbol; the map records the table hash and is refused if a different table is
supplied.

`--root`'s closure also needs *data* names, which a function table cannot carry. The kernelcache names
those itself: the `com.apple.kernel` entry's own symbol table is read as `declared_names()` and binds
every function and data object whose declared name the native kernel defines exactly once. That needs
no external file, and it is what takes `unresolved closure references` to 0.

In the UniXNU build driver, `unixnu kernel --skip-prelink` builds and stages the native executable as
`kernel.macho`. `unixnu prelink --skip-prelink` stages an existing build. This path copies the native
bytes unchanged and does not fetch a kernelcache or add its kexts. Its adjacent JSON record marks the
output as `native-only`; payload arguments and loader support are separate.

Build and interface details: [documentation](https://unixnu.com/drivers/xnu-prelink). Component
sources and dependencies: [catalog](https://unixnu.com/reference/components).
