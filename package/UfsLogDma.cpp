// SPDX-License-Identifier: MIT
//
// One contiguous DMA buffer for the UFS log transport, allocated the way the working ports allocate it.
//
// Why this file exists at all: package/UfsLogKernel.c is C and deliberately IOKit-object-free, so it used
// IOMallocContiguous(). On this board that call cannot succeed, and b265 printed why:
//
//   UkeLastLog: userdata log transport unavailable: dma buffer
//   vm_page_find_contiguous(num=34, low=1048575): found 0 pages at 0xfffffffff000…, scanned 1 pages…
//   kern_allocsite: failure 3: size 0x22000 flags 0x10000 result 3
//
// 0x22000 is exactly UFSLOG_DMA_BYTES (8192 + 131072 = 34 pages), and low=0xFFFFF pages is four gibibytes:
// the allocator was looking for a contiguous run BELOW 4 GiB. This port's device tree gives XNU
// 0x8C0000000..0x9FFDFFFFF and nothing else, so there is no page below 4 GiB to find and the attempt can
// never succeed - xnu's memory map starts at 37 GiB.
//
// Both references solve this by not using IOMallocContiguous at all:
//   UniXNU-sheng/package/AdspProbe.cpp:318 and Allenwdk/UniXNU-uke/drivers/NativeUfs/NativeUfs.cpp:1129
//   IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, … | kIOMemoryPhysicallyContiguous |
//                                               kIOMemoryMapperNone, bytes, 4096)
// which carries no address-mask argument. This file is that call, wrapped so the C transport can use it.
//
// It lives in its own translation unit because the C unit's compile record has no IOKit include path and
// this one does (it is built from the kernel's C++ record, like UfsLog.cpp).
//
// Lifetime: the descriptor is deliberately never released. There is exactly one log transport per boot,
// the buffer must outlive every write, and leaking one object after a successful open is what keeps the
// kernel's own allocator out of the picture. A failed open releases it and reports 0.

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>

extern "C" int uke_log_dma_alloc(uint32_t bytes, void **virt, uint64_t *phys);

extern "C" int
uke_log_dma_alloc(uint32_t bytes, void **virt, uint64_t *phys)
{
	IOBufferMemoryDescriptor *buffer;
	IOPhysicalAddress address;
	IOByteCount contiguous = 0;
	void *bytesPointer;

	if ((virt == nullptr) || (phys == nullptr) || (bytes == 0)) {
		return 0;
	}
	buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    static_cast<IOOptionBits>(kIODirectionInOut) |
	    static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
	    static_cast<IOOptionBits>(kIOMemoryMapperNone),
	    bytes, 4096);
	if (buffer == nullptr) {
		return 0;
	}
	bytesPointer = buffer->getBytesNoCopy();
	address = buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
	// The controller is handed one physical address for the request list and the data area, so the whole
	// range has to be one segment and page aligned. Anything else is unusable and is given straight back.
	if ((bytesPointer == nullptr) || (address == 0) || ((address & 4095) != 0) || (contiguous < bytes)) {
		buffer->release();
		return 0;
	}
	*virt = bytesPointer;
	*phys = static_cast<uint64_t>(address);
	return 1;      // the descriptor is intentionally retained for the life of the kernel
}
