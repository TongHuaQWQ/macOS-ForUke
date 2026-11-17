// SPDX-License-Identifier: BSD-2-Clause-Patent
// uke board entry of the QcomDwc3 driver: the controller's bring-up, and -- from usb=4 on -- the load and
// registration of Apple's own USB classes (the way a module load would register them; see HidInputProbe.cpp).
//
// Started from the first statement of bsd_autoconf() by
// patches/xnu/zzzzzzzzzzzzzzzzzzz-uke-usb-start.patch. Not from native_legacy_display_late_probe(), which is
// where the reference board calls its usb probe: that caller is compiled only when
// XNU_NATIVE_LEGACY_DISPLAY is on, which this board leaves off, and b251-usbdwc3 therefore shipped with
// libQcomDwc3.a force-loaded and never called (the built kernel had _uke_usb_probe and no
// _native_legacy_display_late_probe at all). bsd_autoconf() is also the point the qcom-usb-debug worker
// uses, and for the reason that decides the placement: this kernel has never returned from IOKitBSDInit(),
// so a board service called after it would never start.
//
// Two things this file must not do
// --------------------------------
// Sleep: this kernel's architected timer never delivers an interrupt and IOSleep() waits for exactly that
// interrupt, so a sleep never returns. Every wait here is a spin on the timebase (uke_delay_ms in
// package/QcomDwc3Platform.c), and the first line is printed before the first of them -- a silent worker
// and a worker that never ran have to be distinguishable from the panel.
//
// Assume the kext system is ready: this runs before IOKitBSDInit() has done anything. The hardware
// bring-up needs nothing from IOKit, so `usb=1` is: bring the controller up, log it, stop. The Apple stack
// half is `usb>=4` and retries the two loads with a bounded spin, because at this point the collection may
// still be registering. Whether the stack half is allowed from this point at all is the question the next
// boot answers; this round does not bake that assumption in.
//
// Reporting: one line when the worker starts, one per bring-up stage from the driver's own stage() hook, and
// two lines after the attempt whatever its outcome. The reference board prints the same numbers through
// package/BootLog.c's `log-sys=1` group, a component this port does not carry. QcomDwc3.cpp documents each
// index above native_usb_stats, and stage is negative when a step failed.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/debug.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up(void);
extern "C" void qcom_dwc3_publish(void);
extern "C" void qcom_dwc3_diag(bool);
extern "C" void qcom_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];
extern "C" void qcom_dwc3_host_load(int);
extern "C" void uke_delay_ms(uint32_t);
static void trace(const char *where) {
    const int32_t stage = (int32_t)native_usb_stats[0];
    IOLog("UKE usb: %s stage=%d gsnpsid=%08x hwpar=%08x/%08x/%08x gdsc=%x/%x smmu=%08x repeater=%08x rpmh=%u irq=%u stats19=%d stats20=%d kext=%d\n",
          where, (int)stage, (unsigned)native_usb_stats[1], (unsigned)native_usb_stats[2], (unsigned)native_usb_stats[3],
          (unsigned)native_usb_stats[4], (unsigned)native_usb_stats[5], (unsigned)native_usb_stats[6],
          (unsigned)native_usb_stats[7], (unsigned)native_usb_stats[8], (unsigned)native_usb_stats[9],
          (unsigned)native_usb_stats[11], (int32_t)native_usb_stats[19], (int32_t)native_usb_stats[20],
          (int32_t)native_usb_stats[21]);
    IOLog("UKE usb: helper dsts=%08x dcfg=%08x gctl=%08x gusb2phycfg=%08x retimer=%08x qmp=%08x role=%08x vbus=%08x\n",
          (unsigned)native_usb_stats[15], (unsigned)native_usb_stats[16], (unsigned)native_usb_stats[17],
          (unsigned)native_usb_stats[18], (unsigned)native_usb_stats[32], (unsigned)native_usb_stats[33],
          (unsigned)native_usb_stats[44], (unsigned)native_usb_stats[10]);
}
// The video console keeps only its last screenful, and this board has no other channel until USB itself
// works. A thread that re-states the bring-up verdict every few seconds therefore does two things at once:
// it makes the numbers readable from a photograph at any moment, and its silence is itself a result (the
// kernel stopped, not the console).
static void report_forever(const char *where) {
    for (unsigned beat = 1;; ++beat) {
        IOLog("UKE usb: alive #%u (%s)\n", beat, where);
        trace(where);
        uke_delay_ms(5000);
    }
}
// A thread that does nothing but re-state which stage the bring-up is in. It exists because the bring-up
// runs on another thread and can block inside an allocation or a mapping -- and a blocked bring-up prints
// nothing at all, which is what b254's panel showed: no panic, no stage lines, only the console freezing
// later on AppleImage4's output. A line every second survives that, because the console's scrollback always
// holds the most recent second.
static void reporter(void *, wait_result_t) {
    for (unsigned beat = 1;; ++beat) {
        const int32_t stage = (int32_t)native_usb_stats[0];
        IOLog("UKE usb: beat #%u stage=%d gsnpsid=%08x gdsc=%x/%x smmu=%08x rep=%08x rpmh=%u\n",
              beat, (int)stage, (unsigned)native_usb_stats[1], (unsigned)native_usb_stats[5],
              (unsigned)native_usb_stats[6], (unsigned)native_usb_stats[7], (unsigned)native_usb_stats[8],
              (unsigned)native_usb_stats[9]);
        uke_delay_ms(1000);
    }
}
static void worker(void *, wait_result_t) {
    unsigned mode = 0;
    PE_parse_boot_argn("usb", &mode, sizeof mode);
    IOLog("UKE usb: worker entered usb=%u\n", mode);
    // One short spin so the controller is not touched in the same breath as the BSD init that started us.
    uke_delay_ms(250);
    const bool broughtUp = qcom_dwc3_bring_up();
    trace(broughtUp ? "bring-up" : "bring-up-failed");
    if (mode == 6) {
        // The freeze, and this port's own established way to read a number off the panel: the console keeps
        // only its last screenful, so any early line scrolls away before a photograph can catch it. The panic
        // text stays on screen (and one line of it survives in the firmware's 128-byte pram record), so the
        // bring-up verdict is readable no matter what the console does afterwards. ~108 characters, which is
        // the length this port's panic line is designed to be.
        panic("UKE usb: stage=%d gsnpsid=%08x gdsc=%x/%x smmu=%08x rep=%08x qmp=%08x rpmh=%u irq=%u",
              (int)native_usb_stats[0], (unsigned)native_usb_stats[1], (unsigned)native_usb_stats[5],
              (unsigned)native_usb_stats[6], (unsigned)native_usb_stats[7], (unsigned)native_usb_stats[8],
              (unsigned)native_usb_stats[33], (unsigned)native_usb_stats[9], (unsigned)native_usb_stats[11]);
    }
    if (!broughtUp || mode < 4 || mode == 6) { report_forever(broughtUp ? "up" : "up-failed"); }
    // AppleUSBXDCI has no personality of its own, so nothing else ever loads it: its constructors and
    // metaclasses, and those of IOUSBDeviceFamily below it, run on this load.
    OSReturn loaded = kOSReturnError;
    unsigned attempts = 0;
    while (attempts < 20) {
        ++attempts;
        loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
        if (loaded == kOSReturnSuccess || loaded == kOSKextReturnStartStopError) { break; }
        uke_delay_ms(250);
    }
    native_usb_stats[21] = (uint32_t)loaded;
    IOLog("UKE usb: xdci load -> %d after %u attempt(s)\n", (int)loaded, attempts);
    if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {
        native_usb_stats[0] = (uint32_t)-9;
        trace("xdci-load");
        thread_terminate(current_thread());
        return;
    }
    qcom_dwc3_host_load((int)OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI"));
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { native_usb_stats[0] = (uint32_t)-10; trace("premod"); thread_terminate(current_thread()); return; }
    qcom_dwc3_static_init();
    if (OSMetaClass::postModLoad(handle)) {
        native_usb_stats[0] = (uint32_t)-11;
        trace("postmod");
        thread_terminate(current_thread());
        return;
    }
    qcom_dwc3_publish();
    trace("published");
    if (mode >= 5) { qcom_dwc3_diag(true); }
    report_forever("published");
}
extern "C" void uke_usb_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("usb", &enabled, sizeof enabled) || !enabled) {
        // Never silent: "the call site did not run" and "the argument did not arrive" look identical from
        // the outside, and both have cost a round already.
        IOLog("UKE usb: disabled (usb= not parsed or 0)\n");
        return;
    }
    thread_t thread = nullptr;
    kern_return_t started = kernel_thread_start(reporter, nullptr, &thread);
    if (started != KERN_SUCCESS) { IOLog("UKE usb: reporter thread failed %d\n", (int)started); }
    else { thread_deallocate(thread); }
    started = kernel_thread_start(worker, nullptr, &thread);
    if (started != KERN_SUCCESS) { IOLog("UKE usb: worker thread failed %d\n", (int)started); return; }
    thread_deallocate(thread);
    IOLog("UKE usb: worker started usb=%u\n", enabled);
}
