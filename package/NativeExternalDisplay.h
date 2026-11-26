// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTypes.h>

class IOService;
struct NativeExternalScanoutRect {
    uint32_t width,height,x,y;
};
struct NativeExternalDisplayLink {
    uint32_t width, height;
    uint32_t pixelKHz, hfront, hsync, hback, vfront, vsync, vback;
    bool hlow, vlow;
    const uint8_t *edid;
    unsigned edidBytes;
    void *owner;
    IOReturn (*acquire)(void *owner, void **pixels, uint32_t *pitch, uint32_t *length);
    IOReturn (*submit)(void *owner);
    // Submit queues one flip. Poll reports Busy until its CTL update retires.
    IOReturn (*poll)(void *owner);
    IOReturn (*submitSurface)(void *owner, IOMemoryDescriptor *source,
                              uint32_t offset, uint32_t pitch, const NativeExternalScanoutRect &);
    IOReturn (*submitXr10)(void *owner, IOMemoryDescriptor *source, uint32_t offset,
                          uint32_t pitch, uint32_t sourceWidth, uint32_t sourceHeight,
                          const NativeExternalScanoutRect &target);
};

// The display SMMU uses 4 KiB pages. On success, leave the descriptor wired
// until its owner has retired scanout and invalidated the mapping.
static inline unsigned native_dpu_wire_pages(IOMemoryDescriptor *memory, uint64_t *pages,
                                              unsigned capacity, IOByteCount prefix=0) {
    constexpr unsigned pageSize=4096;
    const IOByteCount allocation=memory->getLength();
    const IOByteCount bytes=prefix?prefix:allocation;
    if(!bytes || bytes>allocation || bytes>uint64_t(capacity)*pageSize ||
       memory->prepare()!=kIOReturnSuccess)return 0;
    unsigned count=0;
    for(IOByteCount offset=0;offset<bytes;) {
        IOByteCount length=0;
        const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!pa || (pa&(pageSize-1)) || !length || (length<bytes-offset && (length&(pageSize-1)))) {
            memory->complete();
            return 0;
        }
        if(length>bytes-offset)length=bytes-offset;
        for(IOByteCount at=0;at<length;at+=pageSize)pages[count++]=pa+at;
        offset+=length;
    }
    return count;
}

extern "C" IOReturn native_legacy_external_publish(IOService *);
extern "C" IOReturn native_legacy_external_connect(const NativeExternalDisplayLink *);
extern "C" bool native_legacy_panel_dma_left_ready();
extern "C" IOReturn native_legacy_panel_route_dma(bool);
extern "C" bool native_legacy_dp_debug();
