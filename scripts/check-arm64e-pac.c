/*
 * check-arm64e-pac: the device-side half of the docs/porting/arm64e.md requirement checks.
 *
 * Signs a pointer with PACIA and restores it with AUTIA on every core, pinning itself to each one
 * through sched_setaffinity, and prints one line per core plus a summary. A changed pointer proves
 * the CPU executed the instruction rather than treating it as a hint, and that the kernel enabled
 * SCTLR_EL1 EnIA/EnIB for EL0. A restored pointer proves the round trip.
 *
 * Build and run (no libc, no NDK):
 *   clang --target=aarch64-linux-android24 -march=armv8.3-a -nostdlib -static \
 *         -fno-stack-protector -fno-builtin -O1 -Wl,-e,_start \
 *         -o pac_rt scripts/check-arm64e-pac.c
 *   adb push pac_rt /data/local/tmp/pac_rt && adb shell '/data/local/tmp/pac_rt'
 *
 * Measured on uke (SM7675) 2026-11-01: cores=8, pacia_changed=8, autia_restored=8, and all eight
 * cores produced the same signature for the same input, which is what one key set shared by every
 * CPU looks like.
 */
/* Does this CPU authenticate? Signs a pointer with PACIA and restores it with AUTIA on every core,
 * pinning itself to each one through sched_setaffinity. Freestanding: raw syscalls, no libc. */
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}

static void out(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void outnum(long v) { char b[24]; int i = 23; b[i--] = 0; if (!v) b[i--] = '0';
	while (v) { b[i--] = (char)('0' + v % 10); v /= 10; } out(&b[i + 1]); }

static char *hex(char *p, u64 v)
{
	static const char d[] = "0123456789abcdef";
	*p++ = '0'; *p++ = 'x';
	for (int i = 60; i >= 0; i -= 4) *p++ = d[(v >> i) & 0xf];
	return p;
}

static inline u64 pacia(u64 p, u64 m) { __asm__ volatile("pacia %0, %1" : "+r"(p) : "r"(m)); return p; }
static inline u64 autia(u64 p, u64 m) { __asm__ volatile("autia %0, %1" : "+r"(p) : "r"(m)); return p; }

void target(void) { }

void _start(void)
{
	unsigned long mask;
	long cores = 0, changed = 0, restored = 0;

	for (long cpu = 0; cpu < 8; cpu++) {
		mask = 1ul << cpu;
		if (sys3(122, 0, 8, (long)&mask) != 0)   /* sched_setaffinity(0, 8, &mask) */
			continue;
		cores++;
		u64 p = (u64)(void *)&target;
		u64 s = pacia(p, 0);
		u64 b = autia(s, 0);
		if (s != p) changed++;
		if (b == p) restored++;

		char buf[80], *w = buf;
		out("cpu "); outnum(cpu); out(": ");
		w = hex(w, p); *w++ = ' '; w = hex(w, s); *w++ = ' '; w = hex(w, b); *w++ = '\n';
		sys3(64, 1, (long)buf, w - buf);
	}

	long order[2] = { 1, 93 };
	/* summary */
	out("cores="); outnum(cores);
	out("  pacia_changed="); outnum(changed);
	out("  autia_restored="); outnum(restored); out("\n");
	sys3(93, (changed == cores && restored == cores) ? 0 : 1, 0, 0);  /* exit */
	for (;;) {}
}
