# darwin24 profile: xnu-12377.121.6

The component ships `darwin22` (xnu-8796) and `darwin23` (xnu-10063) only, while this board
builds **xnu-12377.121.6** (`ac9718f`, the pin in `UniXNU/profiles/os/macos-26.5.2-25F84/profile.toml`,
verified by cloning the tag). Run `42263483616` proved the darwin23 pair does not apply there:
`pe_kprintf.c:13`, `kern/debug.c:1133` and `kern/printf.c:911` all fail, and no other patch in the
170-patch series touches those files, so it is the revision, not a conflict.

These two files are the darwin23 set rebased onto 12377:

    cd <xnu-12377 checkout>            # git clone --depth 1 --branch xnu-12377.121.6
    patch -p1 --fuzz=5 < darwin23/0001-qcom-usb-debug-console.patch
    patch -p1 --fuzz=5 < darwin23/0002-qcom-usb-debug-worker.patch
    git diff -- pexpert/arm/pe_kprintf.c osfmk/kern/debug.c osfmk/kern/printf.c \
                 osfmk/console/serial_console.c osfmk/console/serial_general.c \
        > darwin24/0001-qcom-usb-debug-console.patch
    git diff -- bsd/kern/bsd_init.c > darwin24/0002-qcom-usb-debug-worker.patch

Both regenerated patches pass `git apply --check` on a clean 12377 tree, and the inserted code lands
where the component expects: the console tee in `pe_kprintf.c`/`debug.c`/`printf.c`/`serial_*.c`, and
the worker functions at file scope in `bsd_init.c` started from `bsd_autoconf()` right after
`kminit()` — a call site that always runs, unlike the `STARTUP()`-registered entries this board's own
driver relies on (which have never produced observable output here).
