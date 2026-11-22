#!/usr/bin/env python3
"""Check production WCN6750 power ownership state machine against Wi-Fi/Bluetooth coexistence sequences."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'package/WcnPower.cpp').read_text()
start_marker = '/* ---- WCN ownership state machine (host-tested verbatim by scripts/check-wcn-power.py) ---- */'
end_marker = '/* ---- end WCN ownership state machine ---- */'
body = source[source.index(start_marker):source.index(end_marker)]
out = root / 'drivers/QcaBluetooth/out/power-test'
out.mkdir(parents=True, exist_ok=True)

prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include "WcnPower.h"
using IOLock = std::mutex;
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *l) { l->lock(); }
static void IOLockUnlock(IOLock *l) { l->unlock(); }
static unsigned prepare, enable, disable;
static bool rail_failure, disable_failure, pmic_failure;
static int pmicPrepare() {
    prepare++;
    return pmic_failure ? -EIO : 0;
}
static int powerRails(bool shared, bool on) {
    if (!shared) return 0;
    if (on) enable++;
    else disable++;
    return ((on && rail_failure) || (!on && disable_failure)) ? -EIO : 0;
}
'''

tests = r'''
int main() {
    assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));
    assert(enable == 1 && prepare == 1 && sheng_wcn_power_owners() == 2);
    assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));
    assert(enable == 1 && prepare == 1);
    assert(!sheng_wcn_power_get(SHENG_WCN_WLAN));
    assert(enable == 1 && prepare == 1 && sheng_wcn_power_owners() == 3);
    assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));
    assert(!disable && sheng_wcn_power_owners() == 1);
    assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));
    assert(enable == 1 && prepare == 1 && sheng_wcn_power_owners() == 3);
    assert(!sheng_wcn_power_put(SHENG_WCN_WLAN));
    assert(!disable && sheng_wcn_power_owners() == 2);
    disable_failure = true;
    assert(sheng_wcn_power_put(SHENG_WCN_BLUETOOTH) == EIO);
    assert(sheng_wcn_power_owners() == 2);
    disable_failure = false;
    assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));
    assert(sheng_wcn_power_owners() == 0);
    assert(!sheng_wcn_power_get(SHENG_WCN_WLAN));
    assert(enable == 2 && prepare == 2);
    assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));
    assert(enable == 2 && prepare == 2);
    unsigned before_disable = disable;
    assert(!sheng_wcn_power_put(SHENG_WCN_WLAN));
    assert(disable == before_disable);
    assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));
    assert(disable == before_disable + 1);
    /* Rail failure rolls back power rails and leaves zero owners. */
    before_disable = disable;
    rail_failure = true;
    assert(sheng_wcn_power_get(SHENG_WCN_BLUETOOTH) == EIO);
    assert(disable == before_disable + 1 && !sheng_wcn_power_owners());
    /* PMIC failure does not touch rails and leaves zero owners. */
    rail_failure = false;
    pmic_failure = true;
    unsigned before_enable = enable;
    before_disable = disable;
    assert(sheng_wcn_power_get(SHENG_WCN_BLUETOOTH) == EIO);
    assert(enable == before_enable && disable == before_disable && !sheng_wcn_power_owners());
    assert(sheng_wcn_power_get(2) == EINVAL && sheng_wcn_power_put(2) == EINVAL);
    delete gWcnLock;
    return 0;
}
'''

(out / 'power.cpp').write_text(prefix + body + tests)
subprocess.run(
    ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
     '-I' + str(root / 'package'), str(out / 'power.cpp'), '-o', str(out / 'power')],
    check=True,
)
subprocess.run([out / 'power'], check=True)
(out / 'report.json').write_text(json.dumps(dict(
    passed=True,
    hardware_verified=False,
    source_sha256=hashlib.sha256(body.encode()).hexdigest(),
), indent=2) + '\n')
print('PASS: WCN6750 independent radio votes, shared supply lifetime, rollback and power failure handling')
