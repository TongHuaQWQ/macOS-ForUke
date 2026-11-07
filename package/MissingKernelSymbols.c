// SPDX-License-Identifier: MIT
//
// Provide kernel entry points the prelinked fileset calls but this configuration
// does not build.
//
// Apple's arm64 Macs enable `config_csr` and `config_csr_from_dt` in
// `config/MASTER.arm64.MacOSX`, so `bsd/kern/kern_csr.c` is part of their kernel
// and AMFI, apfs and sandbox call `csr_check()` 87 times. This port builds with
// `config/MASTER.arm64`, whose SECURITY_BASE is only
// { config_macf kernel_integrity config_secure_bsd_root }: `kern_csr.c` is
// `optional config_csr`, so the file is not compiled at all, nothing defines
// `_csr_check`, and the prelinker has no function to bind those 87 call sites to
// (it leaves them on the panic stub, and no anchor can help — the symbol does not
// exist to anchor to).
//
// The definitions are weak on purpose: turn config_csr on and XNU's own
// implementation wins without a duplicate-symbol error at the link.
//
// The answer is "everything is allowed", which is what this board already assumes
// elsewhere: boot_args carries no CSR active config, and the payload boots with
// amfi_get_out_of_my_way=1.

#include <stdint.h>

#define UKE_CSR_ALLOW_ALL 0xffffffffu
#define UKE_EPERM         1 /* sys/errno.h: EPERM, spelled out to keep this file header-free */

__attribute__((weak)) int
csr_get_active_config(uint32_t *config)
{
	if (config != 0) {
		*config = UKE_CSR_ALLOW_ALL;
	}

	return 0;
}

__attribute__((weak)) int
csr_check(uint32_t mask)
{
	uint32_t config = 0;

	/* This weak stub exists because the prelinked fileset calls csr_check by name, so a line here
	 * proves the archive's code runs at all - independent of STARTUP, of the console and of msgbuf.
	 * Same pram slot the USB breadcrumbs use (uke_usb writes UKEUSB-* there from 0xFF800000+0x1000);
	 * this one uses +0x1200 and says so once. */
	{
		static int written;
		if (!written) {
			extern void uke_usb_hook_entry(const char *);
			extern void uke_last_log_hook_entry(const char *);
			uke_usb_hook_entry("UKEUSB-HOOK-CSRCHECK");
			uke_last_log_hook_entry("UKELASTLOG-HOOK-CSRCHECK");
			typedef unsigned long uke_vm_offset_t;
			extern uke_vm_offset_t ml_io_map_wcomb(uke_vm_offset_t, unsigned long);
			uke_vm_offset_t va = ml_io_map_wcomb(0xFF800000ull, 0x2000);
			if (va != 0) {
				char *slot = (char *)(va + 0x1200);
				const char *text = "UKEUSB-VIA-CSRCHECK";
				unsigned i = 0;
				while (i < 63 && text[i] != '\0') {
					slot[i] = text[i];
					i++;
				}
				while (i < 64) {
					slot[i++] = 0;
				}
				__asm__ volatile ("dsb sy" ::: "memory");
			}
			written = 1;
		}
	}

	if (csr_get_active_config(&config) != 0) {
		return UKE_EPERM;
	}

	return ((config & mask) == mask) ? 0 : UKE_EPERM;
}
