#!/usr/bin/env bash
# Apply the XNU patch series the kernel build applies, to the XNU revision the build applies it to, in
# the build's order. Two things had to be reproduced rather than assumed, and both were wrong before:
#
#   * WHICH TREE. The series targets xnu-12377.121.6, the revision the OS profile pins
#     ("xnu_revision" in profiles/os/<profile>/profile.toml, and what `unixnu fetch` checks out). This
#     script used to take d8b80295118e out of unixnu.lock and clone the tag that pairs with it
#     (xnu-10063.141.1) -- the 26.5.2-era feed pin, which the macOS profile overrides. Against that
#     tree the very first patch cannot apply, because 12377 already ships config/MASTER.arm64.bcm2837
#     and bcm2837-machine-config.patch creates it.
#
#   * WHICH PATCHES. The series is resolved, not globbed. board.toml [kernel] patch_series names the
#     layers (arch:patches/xnu/series, os:patches/xnu/series, device:patches/xnu/series) and every
#     entry resolves relative to its own series file unless it carries a "<profile>:" prefix -- the
#     same resolution the driver performs (unixnu/patches.py plan()). The catalog directories hold more
#     patch files than any one profile selects, so applying every *.patch, which this script also used
#     to do, tests a configuration no build produces.
#
# The core checkout must be the one the kernel build uses (build-kernel-min.yml's UKE_CORE_REPO and
# UKE_CORE_REF defaults, TongHuaQWQ/UniXNU@macos): the profiles that name the XNU revision and the
# patch order live there. Point UNIXNU_CORE at it. XNU_COMMIT overrides the revision, XNU_SOURCE
# clones from a local mirror instead of github.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CORE=${UNIXNU_CORE:-$ROOT/../UniXNU}
XNU_GIT=${XNU_GIT:-https://github.com/apple-oss-distributions/xnu.git}

if [ ! -f "$CORE/components.toml" ] || [ ! -d "$CORE/profiles/os" ]; then
  echo "no core checkout at $CORE (set UNIXNU_CORE)" >&2
  exit 1
fi

# The revision the profile pins, falling back to the feed pin recorded in unixnu.lock.
XNU_COMMIT=${XNU_COMMIT:-$(python3 - "$CORE" "$ROOT" <<'PY'
import pathlib
import sys
import tomllib

core = pathlib.Path(sys.argv[1]).resolve()
device = pathlib.Path(sys.argv[2]).resolve()
board = tomllib.loads((device / 'board.toml').read_text())
profile = core / 'profiles' / 'os' / board['board']['os_profile'] / 'profile.toml'


def find(node, key):
    """The profiles keep these at different depths; take the first string under that key."""
    if isinstance(node, dict):
        if isinstance(node.get(key), str):
            return node[key]
        for value in node.values():
            found = find(value, key)
            if found:
                return found
    return None


revision = find(tomllib.loads(profile.read_text()), 'xnu_revision') if profile.is_file() else None
if revision:
    print(revision)
else:
    print(tomllib.loads((device / 'unixnu.lock').read_text())['source']['xnu']['commit'])
PY
)}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ -n "${XNU_SOURCE:-}" ]; then
  git clone -q --shared "$XNU_SOURCE" "$WORK/xnu"
else
  # A commit, not a tag: --branch cannot name one, and the revision the profile pins is a commit.
  git init -q "$WORK/xnu"
  git -C "$WORK/xnu" remote add origin "$XNU_GIT"
  git -C "$WORK/xnu" fetch -q --depth 1 origin "$XNU_COMMIT"
fi
git -c advice.detachedHead=false -C "$WORK/xnu" checkout -q --detach "$XNU_COMMIT"
ACTUAL=$(git -C "$WORK/xnu" rev-parse HEAD)
if [ "$ACTUAL" != "$XNU_COMMIT" ]; then
  echo "unexpected XNU revision: $ACTUAL (expected $XNU_COMMIT)" >&2
  exit 1
fi
echo "XNU $ACTUAL"

# Resolve the series exactly as the driver does. One absolute patch path per line, in build order.
mapfile -t PATCHES < <(python3 - "$CORE" "$ROOT" <<'PY'
import pathlib
import sys
import tomllib

core = pathlib.Path(sys.argv[1]).resolve()
device = pathlib.Path(sys.argv[2]).resolve()
board = tomllib.loads((device / 'board.toml').read_text())
kernel = board['kernel']

os_root = core / 'profiles' / 'os' / board['board']['os_profile']
if not (os_root / 'profile.toml').is_file():
    sys.exit(f'OS profile not found in the core checkout: {os_root}/profile.toml')


def find_arch(node):
    """profiles.py takes the arch profile name from the OS profile.toml, wherever it puts it."""
    if isinstance(node, dict):
        if isinstance(node.get('arch'), str):
            return node['arch']
        for value in node.values():
            found = find_arch(value)
            if found:
                return found
    return None


arch_name = find_arch(tomllib.loads((os_root / 'profile.toml').read_text()))
if not arch_name:
    sys.exit(f'{os_root}/profile.toml names no arch profile')
roots = {'arch': core / 'profiles' / 'arch' / arch_name, 'os': os_root, 'device': device}

series = []
for reference in kernel.get('patch_series', []):
    prefix, separator, relative = reference.partition(':')
    path = roots[prefix] / relative if separator and prefix in roots else device / reference
    if not path.is_file():
        sys.exit(f'patch series missing: {path}')
    series.append(path)

selected = []
for path in series:
    for number, line in enumerate(path.read_text().splitlines(), 1):
        entry = line.strip()
        if not entry or entry.startswith('#'):
            continue
        if entry.startswith('component:'):
            sys.exit(f'{path}:{number}: component: entries are solved against the component catalog, '
                     f'which this check does not read: {entry}')
        prefix, separator, relative = entry.partition(':')
        target = roots[prefix] / relative if separator and prefix in roots else path.parent / entry
        target = target.resolve()
        if not target.is_file():
            sys.exit(f'{path}:{number}: patch missing: {target}')
        if target in selected:
            sys.exit(f'{path}:{number}: the same patch is selected twice: {target}')
        selected.append(target)

print(f'{len(series)} layer(s), {len(selected)} patch(es)', file=sys.stderr)
for target in selected:
    print(target)
PY
)

if [ "${#PATCHES[@]}" -eq 0 ]; then
  echo "the resolved series is empty; refusing to report success" >&2
  exit 1
fi

apply() {
  local patch=$1
  local name
  local error
  name=$(basename "$patch")
  if ! error=$(git -C "$WORK/xnu" apply --check "$patch" 2>&1); then
    echo "FAIL: $name" >&2
    printf '%s\n' "$error" | sed 's/^/  /' >&2
    return 1
  fi
  git -C "$WORK/xnu" apply "$patch"
  echo "PASS: $name"
}

for patch in "${PATCHES[@]}"; do
  apply "$patch"
done
echo "PASS: the series applies cleanly to $XNU_COMMIT"
