/** @file
  Firmware heartbeat, gunyah watchdog pet, and a measurement of whether timer
  events work at all in this UEFI.

  Why this driver exists: the vendor-DXE bring-up dies with no output, and two very
  different causes look identical in a photograph - a driver that hangs the CPU,
  and a boot that Gunyah's virtual watchdog resets because nobody petted it. Both
  end in "black screen, then reset".

  The first version counted with a one-second timer event and produced no counter at
  all on the panel (the b71 round photographed after Smp2pDxe), which is itself an
  answer: timer events are not being delivered here, so anything that waits on one -
  including the pet - never runs. This version therefore does not depend on
  interrupts:

    * it wraps gBS->Stall and counts the vendor drivers' own waits, which is where a
      stuck bring-up spends its time anyway,
    * it pets the watchdog on every counted wait, so a long vendor timeout keeps the
      board alive instead of tripping the watchdog,
    * it writes one line per ten seconds of accumulated stall time, plus a word in
      the reserved page at 0xA7000000, so a counter that stops means a hung CPU,
    * and at startup it prints how many timer callbacks a two-second busy wait
      produced - the direct measurement of the timer/IRQ path.

  Output level: every line here is DEBUG_WARN, not DEBUG_INFO. The platform sets
  PcdDebugPrintErrorLevel = 0x8007EE0F (SiliconPkg.dsc.inc), which enables INIT, WARN,
  LOAD, FS and ERROR but NOT INFO (0x40), so a DEBUG_PRINT(INFO) is compiled in and then
  filtered out at runtime - that is why the dispatcher's "Loading driver at" lines
  (DEBUG_LOAD|DEBUG_INFO) reach the panel while this driver's INFO lines did not.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include "UkePersist.h"

//
// "FD Reserved" in Library/MemoryMapLib: 0xA7000000, one MiB, mapped, owned by this
// firmware and unused by the FD itself (that starts at 0xA7100000). A word of state
// here still exists after a reset, when the panel does not.
//
#define UKE_BEAT_BASE       0xA7000000
#define UKE_BEAT_MAGIC      0x54414542U /* "BEAT" */
#define UKE_BEAT_LINE_US    10000000U   /* one panel line per 10 s of stall time */

STATIC EFI_STALL  mOriginalStall;
STATIC volatile UINT32  mInWrapper;
STATIC UINT32     mTimerTicks;
STATIC UINT32     mStallCount;
STATIC UINT64     mStallUs;
STATIC UINT64     mNextLineUs = UKE_BEAT_LINE_US;

/**
  Gunyah's virtual watchdog: the hypervisor call UkeBootMemoryDxe, UkeUsbLogDxe and
  Android's gh_virt_wdt all make.
**/
STATIC
VOID
UkeProgressPet (
  VOID
  )
{
  register UINT64  X0 __asm ("x0") = 0x86000007;
  register UINT64  X1 __asm ("x1") = 0;
  register UINT64  X2 __asm ("x2") = 0;
  register UINT64  X3 __asm ("x3") = 0;
  register UINT64  X4 __asm ("x4") = 0;
  register UINT64  X5 __asm ("x5") = 0;
  register UINT64  X6 __asm ("x6") = 0;
  register UINT64  X7 __asm ("x7") = 0;

  __asm__ volatile (
    "smc #0"
    : "+r" (X0), "+r" (X1)
    : "r" (X2), "r" (X3), "r" (X4), "r" (X5), "r" (X6), "r" (X7)
    : "memory"
    );
}

STATIC
VOID
UkeProgressMark (
  VOID
  )
{
  volatile UINT32  *Marker;

  if (PcdGet32 (PcdUkeHeartbeatMarker) == 0) {
    return;
  }

  Marker    = (volatile UINT32 *)(UINTN)UKE_BEAT_BASE;
  Marker[0] = UKE_BEAT_MAGIC;
  Marker[1] = mStallCount;
  Marker[2] = mTimerTicks;
  Marker[3] = (UINT32)(mStallUs / 1000);
}

/**
  Replaces gBS->Stall for the rest of the boot. Every wait a vendor driver takes - the
  PMIC, clock and IPC timeouts are all Stalls - pets the watchdog and is counted, which
  keeps working when the timer event path does not.
**/
STATIC
EFI_STATUS
EFIAPI
UkeProgressStall (
  IN UINTN  Microseconds
  )
{
  EFI_STATUS  Status;

  //
  // Re-entrancy guard: the console and debug paths are allowed to call Stall, and printing
  // from inside a Stall that a print called is an unbounded recursion with a smashed stack
  // at the end of it. A nested call bypasses the body entirely.
  //
  if (mInWrapper != 0) {
    return mOriginalStall (Microseconds);
  }

  mInWrapper  = 1;
  mStallCount += 1;
  mStallUs    += Microseconds;

  UkeProgressPet ();

  if (mStallUs >= mNextLineUs) {
    UkeProgressMark ();
    // DEBUG, not the UefiLib printer: this driver is dispatched from APRIORI before any
    // console exists, and UefiLibPrint asserts on a NULL gST->ConOut
    // ("ASSERT [...] UefiLibPrint.c(201): Console != ((void *) 0)"), which halted the b72
    // boot on this very line. The dispatcher's "Loading driver at ..." lines reach the panel
    // through DebugLib, so these do too.
    DEBUG ((
      DEBUG_WARN,
      "UKE-BEAT stalls=%u time=%ums timer=%u\n",
      mStallCount,
      (UINTN)(mStallUs / 1000),
      mTimerTicks
      ));
    mNextLineUs += UKE_BEAT_LINE_US;
  }

  Status      = mOriginalStall (Microseconds);
  mInWrapper  = 0;
  return Status;
}

STATIC
VOID
EFIAPI
UkeProgressTimerTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mTimerTicks += 1;
}


//
// The inherited USB state, read once before any vendor driver can touch the clock or
// power tree. Same registers and same order as QcomXnuUsbDebug's
// soc/QcomUsbQualify.c: nothing outside the GCC window is read until the power domain
// and the three required USB branches are proven live, because a read of an unclocked
// DWC3 window is an asynchronous external abort with no output at all.
// (Live DT: gcc: clock-controller@100000 reg <0x100000 0x1f4200>; msm-dwc3 a600000.)
//
#define UKE_GCC_BASE      0x00100000
#define UKE_DWC3_BASE     0x0A600000
#define UKE_GCC_BCR       0x39000
#define UKE_GCC_GDSCR     0x39004
#define UKE_GCC_CFG_GDSCR 0x39008

STATIC CONST UINT32  mGccCbcr[5] = { 0x39018, 0x39024, 0x39028, 0x39060, 0x39064 };

STATIC
VOID
UkeProgressUsbState (
  VOID
  )
{
  UINT32  Cbcr[5];
  UINT32  Bcr;
  UINT32  CfgGdscr;
  UINTN   Index;
  UINTN   Running;

  for (Index = 0; Index < 5; Index++) {
    Cbcr[Index] = MmioRead32 (UKE_GCC_BASE + mGccCbcr[Index]);
  }

  Bcr      = MmioRead32 (UKE_GCC_BASE + UKE_GCC_BCR);
  CfgGdscr = MmioRead32 (UKE_GCC_BASE + UKE_GCC_CFG_GDSCR);

  Running = 0;
  for (Index = 0; Index < 3; Index++) {
    if ((Cbcr[Index] & (1U << 31)) == 0) {
      Running++;
    }
  }

  DEBUG ((
    DEBUG_WARN,
    "UKE-GCC: bcr=%08x cfg_gdscr=%08x cbcr %08x %08x %08x %08x %08x\n",
    Bcr,
    CfgGdscr,
    Cbcr[0], Cbcr[1], Cbcr[2], Cbcr[3], Cbcr[4]
    ));
  DEBUG ((
    DEBUG_WARN,
    "UKE-GCC: gdsc_powered=%u reset_deasserted=%u usb_clocks_running=%u/3\n",
    (UINTN)((CfgGdscr & (1U << 16)) != 0),
    (UINTN)((Bcr & 1U) == 0),
    Running
    ));

  if (((CfgGdscr & (1U << 16)) == 0) || ((Bcr & 1U) != 0) || (Running < 3)) {
    DEBUG ((DEBUG_WARN, "UKE-GCC: DWC3 not readable yet, window left untouched\n"));
    return;
  }

  DEBUG ((
    DEBUG_WARN,
    "UKE-GCC: dwc3 gsnpsid=%08x gctl=%08x phycfg=%08x dcfg=%08x dctl=%08x dsts=%08x\n",
    MmioRead32 (UKE_DWC3_BASE + 0xC120),
    MmioRead32 (UKE_DWC3_BASE + 0xC110),
    MmioRead32 (UKE_DWC3_BASE + 0xC200),
    MmioRead32 (UKE_DWC3_BASE + 0xC700),
    MmioRead32 (UKE_DWC3_BASE + 0xC704),
    MmioRead32 (UKE_DWC3_BASE + 0xC70C)
    ));
}

EFI_STATUS
EFIAPI
UkeProgressDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   TimerEvent;
  UINT32      Before;

  mStallCount = 0;
  mStallUs    = 0;
  mTimerTicks = 0;
  mInWrapper  = 0;

  //
  // The previous boot's stage log, printed before any vendor driver runs: a round that
  // ends in a black screen is otherwise unattributable, because the display can be lost
  // mid-boot and there is no console left to ask.
  //
  {
    UKE_PERSIST_BLOCK  *Previous = UkePersistBlock ();

    if (Previous->Magic == UKE_PERSIST_MAGIC) {
      UINT32  Index;
      UINT32  First;
      UINT32  Shown;

      DEBUG ((
        DEBUG_WARN,
        "UKE-PREV: boot %u reached stage %u detail %08x after %u records\n",
        Previous->Boot,
        Previous->LastStage,
        Previous->LastDetail,
        Previous->Count
        ));

      Shown = Previous->Count < UKE_PERSIST_RECORDS ? Previous->Count : UKE_PERSIST_RECORDS;
      First = (Previous->Count >= UKE_PERSIST_RECORDS) ? (Previous->Count % UKE_PERSIST_RECORDS) : 0;
      for (Index = 0; Index < Shown; Index++) {
        UKE_PERSIST_RECORD  *Record;

        Record = &Previous->Records[(First + Index) % UKE_PERSIST_RECORDS];
        DEBUG ((
          DEBUG_WARN,
          "UKE-PREV: %u stage %u detail %08x tick %lx\n",
          Index,
          Record->Stage,
          Record->Detail,
          Record->Tick
          ));
      }
    } else {
      DEBUG ((DEBUG_WARN, "UKE-PREV: no stage log (first boot into this image)\n"));
    }

    Previous->Records[0].Stage = 0;   /* start a fresh ring for this boot */
    Previous->Count            = 0;
    Previous->Boot            += 1;
    Previous->Magic            = UKE_PERSIST_MAGIC;
    Previous->Version          = 1;
  }

  DEBUG ((
    DEBUG_WARN,
    "UKE-BEAT entry: marker=%u stallhook=%u\n",
    PcdGet32 (PcdUkeHeartbeatMarker),
    PcdGet32 (PcdUkeHeartbeatStallHook)
    ));
  UkePersistStage (UKE_STAGE_PROGRESS_ENTRY, 0);
  UkeProgressMark ();
  UkeProgressUsbState ();
  DEBUG ((DEBUG_WARN, "UKE-BEAT entry: pet + mark done\n"));
  UkePersistStage (UKE_STAGE_PROGRESS_READY, 0);

  //
  // Measure the timer path before replacing anything: arm a 100 ms periodic event, busy
  // wait two seconds through the original Stall, and report how many callbacks arrived.
  // Zero is what this board answers, and it is worth printing: every other diagnostic
  // that waits on time inherits that fact.
  //
  Before = mTimerTicks;
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, UkeProgressTimerTick, NULL, &TimerEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (TimerEvent, TimerPeriodic, 100U * 1000U * 10U);
  }

  gBS->Stall (2U * 1000U * 1000U);

  DEBUG ((
    DEBUG_WARN,
    "UKE-BEAT timer probe: %u callbacks in 2 s (%r)\n",
    mTimerTicks - Before,
    Status
    ));

  if (!EFI_ERROR (Status)) {
    gBS->CloseEvent (TimerEvent);
  }

  if (PcdGet32 (PcdUkeHeartbeatStallHook) != 0) {
    mOriginalStall = gBS->Stall;
    gBS->Stall     = UkeProgressStall;
    DEBUG ((DEBUG_WARN, "UKE-BEAT stall counter armed, watchdog petted\n"));
  } else {
    DEBUG ((DEBUG_WARN, "UKE-BEAT stall hook disabled by PCD\n"));
  }

  return EFI_SUCCESS;
}
