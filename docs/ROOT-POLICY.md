# Root policies for 26C5035r: what was signed, and what still needs a Mac

`scripts/policies/26C5035r/{OS,SystemOS,App}/` holds the signed root policies the arm64e storage runtime
authenticates every read against. They were produced on 2026-11-15 on an x86 Mac with
`UniXNU/scripts/make-root-policies.py`, from the same disk images this board installs. This page records
what that required, what it produced, and which steps still need macOS — so the work is reproducible
without guessing.

## What the policies bind

Each `verification.json` is a record of a real signing run, not a hand-written pin. Every digest below was
re-checked against the images on disk after decryption.

| window | image | blocks | image_sha256 | volume_uuid |
| --- | --- | --- | --- | --- |
| OS | `141-22233-021.dmg`, 14,493,417,472 B | 3,538,432 | `40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70` | `84581E99-85BE-4957-AC53-2947E887B3E4` |
| SystemOS | `141-22145-021.dmg`, 2,917,138,432 B | 712,192 | `14330ed990fe7022114b10718f4defe3049d49b6e5d52fd3ac71a487541cd1f3` | `6678E466-2F88-4C56-9408-DC7361BAA0BA` |
| App | `141-22106-021.dmg`, 54,525,952 B | 13,312 | `797bb1e07e354e80e75d3be2658d8f586b343dbdc05cb93cb880e772ab0d23d1` | `F2AEA010-C8EB-4AF8-92A9-4B90781CD668` |

All three share one anchor, `sha256(trusted-key.bin) = 4deaef985a8d17f3e0c8cd94ad879cd8…` (RSA-3072,
395-byte anchor), and all three record `real_c_signature_and_block_verification: true` and
`original_image_modified: false`. The three digests are also exactly upstream's
`profiles/os/macos-27.3-26C5035r/profile.toml [root]` values, which is an independent confirmation that
this board installs the images those policies describe.

The three block tables are 113,229,824 + 22,790,144 + 425,984 B (130 MiB). Against
`boot_partition_bytes = 0x6000000` (96 MiB) the OS table alone does not fit, so `board.toml [root]`
sets `tables_in_userdata`, the kernel bundle carries only the 544-byte headers, and the tables travel as
the `[userdata] objects` `OS.vroot` / `SystemOS.vroot` / `App.vroot`. `hashes.bin` sizes equal
blocks × 32 exactly for all three windows.

## Command

```
make-root-policies.py --tree TREE --output TREE/scripts/policies/26C5035r \
  --window OS=<OS image>:84581E99-85BE-4957-AC53-2947E887B3E4 \
  --window SystemOS=<SystemOS image>:6678E466-2F88-4C56-9408-DC7361BAA0BA \
  --window App=<App image>:F2AEA010-C8EB-4AF8-92A9-4B90781CD668
```

On the Mac used here, two prerequisites were missing and had to be supplied first: the trimmed Xcode
install has no `xcodebuild`, so `xcrun --sdk iphoneos --show-sdk-path` fails even though
`Platforms/iPhoneOS.platform/.../iPhoneOS.sdk` exists (a five-line `xcrun` shim answers that query and
passes everything else through); and there is no Homebrew, so no OpenSSL 3 under the three prefixes
`make-root-policies.py` probes (OpenSSL 3.0.15 was configured in `$HOME` and the temporary tree's copy of
the script learned a `UNIXNU_OPENSSL_PREFIX` override). Both adaptations are recorded in
`work/keys/uke-owner/mac-round/` on the workstation, not in this repository.

## What still needs macOS

| step | why | consequence here |
| --- | --- | --- |
| `kits/VerifiedRoot/tests/check.py`, `tools/verify_image.py` | builds and loads `verified-root.dylib` with `xcrun clang -dynamiclib` | verification is macOS-only; each window's `verification.json` already records its result |
| `scripts/make-macos-data-seed.py`, `scripts/seed-data-grow.py` | `hdiutil` | a *new* Data seed needs a Mac; the existing one is committed to the workstation at `runtime/data.dbr` |
| `scripts/build-verified-root-probe.py` | `xcrun clang -target arm64-apple-ios17.0` plus a pinned sheng image | blocked independently of macOS: it refuses this board's images, so `VERIFIED_ROOT_PROBE` stays off |
| `unixnu build kernel` | needs `xcodebuild` | CI's macOS runner, not a workstation Mac |

Signing and table generation do **not** need macOS: `kits/VerifiedRoot/tools/manifest.py` needs only
python3 and the `openssl` CLI, and re-signing a test image on Linux reproduced the same anchor. The owner
private key is therefore the one artifact that cannot be regenerated; it is kept outside every repository
(`work/keys/uke-owner/` on the workstation, `0600`, with its own README) and must never be committed.
