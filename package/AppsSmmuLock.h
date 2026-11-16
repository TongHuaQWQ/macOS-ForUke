/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Serialises the owners that reprogram one apps-SMMU context bank: the UFS takeover programs stream 0x60 and the USB controller
 * stream 0x40 behind the same bypass bank, and an interleaved pair of updates faults the bank. The two symbols are what
 * drivers/QcomDwc3/QcomDwc3.cpp imports; they say SM8550 because that is the sheng board's provider this driver was written
 * against, and package/QcomDwc3Platform.c implements them for this board. Weak on purpose: a build without that provider links
 * and runs, with only the ordering between the two owners lost. */
#ifndef UKE_APPS_SMMU_LOCK_H
#define UKE_APPS_SMMU_LOCK_H
extern "C" bool sm8550_apps_smmu_lock(void) __attribute__((weak_import));
extern "C" void sm8550_apps_smmu_unlock(void) __attribute__((weak_import));
class AppsSmmuLock {
    bool held;
public:
    AppsSmmuLock():held(sm8550_apps_smmu_lock && sm8550_apps_smmu_unlock && sm8550_apps_smmu_lock()) {}
    ~AppsSmmuLock(){if(held)sm8550_apps_smmu_unlock();}
    bool ready() const {return held || !sm8550_apps_smmu_lock;}
    AppsSmmuLock(const AppsSmmuLock&)=delete;
    AppsSmmuLock& operator=(const AppsSmmuLock&)=delete;
};
#endif
