// SPDX-License-Identifier: BSD-2-Clause-Patent
// uke board entry of the QcomDwc3 driver: the controller's classes register here, after the hardware answered, the way a module
// load would register them (see HidInputProbe.cpp for the same deferred metaclass registration).
//
// This is the only place the official route reports itself. The sheng board prints native_usb_stats through package/BootLog.c's
// `log-sys=1` group, a component this port does not carry, so the table is logged here instead: two lines after every step that
// matters, which is what the panel (with -uketextconsole) and the pram msgbuf record keep. The numbers are the driver's own:
// QcomDwc3.cpp documents each index above native_usb_stats, and stage is negative when a step failed.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up(void);
extern "C" void qcom_dwc3_publish(void);
extern "C" void qcom_dwc3_diag(bool);
extern "C" void qcom_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];
extern "C" void qcom_dwc3_host_load(int);
// Two lines on purpose. The first is the bring-up verdict and the stage table a failed round is read from; the second is the
// register state behind it (SMMU context, repeater, PHY PCS, role) so one photograph of the tail answers both questions.
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
static void worker(void *, wait_result_t) {
    IOSleep(3000);
    // Logged before anything is decided: a failed stage is the reading a no-USB round needs, and a worker that only spoke on
    // success could not be told from one that never ran.
    const bool broughtUp = qcom_dwc3_bring_up();
    trace(broughtUp ? "bring-up" : "bring-up-failed");
    if (broughtUp) {
        // AppleUSBXDCI has no personality of its own, so nothing has loaded it: its constructors and metaclasses, and those of
        // IOUSBDeviceFamily below it, run on this load.
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
        native_usb_stats[21] = (uint32_t)loaded;
        if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {native_usb_stats[0] = (uint32_t)-9; trace("xdci-load"); thread_terminate(current_thread());}
        // The same for host mode: Apple's xHCI driver and the USB host family below it.
        qcom_dwc3_host_load((int)OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI"));
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) {native_usb_stats[0] = (uint32_t)-10; trace("premod");}
        else {
            qcom_dwc3_static_init();
            if (OSMetaClass::postModLoad(handle)) {native_usb_stats[0] = (uint32_t)-11; trace("postmod");}
            else {
                qcom_dwc3_publish();
                trace("published");
                unsigned mode = 0;
                PE_parse_boot_argn("usb", &mode, sizeof mode);
                if (mode == 2 || mode == 3) qcom_dwc3_diag(mode == 3);
            }
        }
    }
    thread_terminate(current_thread());
}
extern "C" void uke_usb_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("usb", &enabled, sizeof enabled) || !enabled) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
