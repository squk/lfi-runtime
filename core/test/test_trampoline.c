#include "lfi_arch.h"
#include "lfi_core.h"
#include "test.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Use -b to run the benchmark: ./core/test/test_trampoline.c.elf -b

#include "test_progs.h"

static inline long long unsigned
time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) {
        exit(1);
    }
    return ((long long unsigned) ts.tv_sec) * 1000000000LLU +
        (long long unsigned) ts.tv_nsec;
}

static int
callback(int a)
{
    printf("callback got %d\n", a);
    return a;
}

static int
callback_bench(int a)
{
    return a;
}

int
main(int argc, char **argv)
{
    bool bench = false;
    if (argc >= 2 && strcmp(argv[1], "-b") == 0)
        bench = true;
    size_t pagesize = getpagesize();
    // Initialize LFI.
    struct LFIEngine *engine = lfi_new(
        (struct LFIOptions) {
            .boxsize = gb(4),
            .pagesize = pagesize,
            .verbose = true,
            .no_verify = false,
        },
        1);
    assert(engine);

    // Create a new sandbox.
    struct LFIBox *box = lfi_box_new(engine);
    assert(box);

    lfiptr p = lfi_box_mapany(box, pagesize, LFI_PROT_READ | LFI_PROT_WRITE,
        LFI_MAP_ANONYMOUS | LFI_MAP_PRIVATE, -1, 0);
    assert(p != (lfiptr) -1);
    assert(lfi_box_ptrvalid(box, p));

#if defined(LFI_ARCH_X64)
    // Set all bytes to trap instructions, since 0 does not pass verification.
    memset((void *) lfi_box_l2p(box, p), 0xcc, pagesize);
#endif

    lfiptr p_prog = lfi_box_copyto(box, p, prog, sizeof(prog));
    lfiptr p_prog_cb = lfi_box_copyto(box, p + sizeof(prog), prog_cb,
        sizeof(prog_cb));

    bool ret_ok = lfi_box_init_ret(box);
    assert(ret_ok);

    int r = lfi_box_mprotect(box, p, pagesize, LFI_PROT_READ | LFI_PROT_EXEC);
    assert(r == 0);

    lfiptr stack = lfi_box_mapany(box, pagesize, LFI_PROT_READ | LFI_PROT_WRITE,
        LFI_MAP_ANONYMOUS | LFI_MAP_PRIVATE, -1, 0);

    struct LFIContext *ctx = lfi_ctx_new(box, NULL);
    assert(ctx);

#if defined(LFI_ARCH_X64)
    lfi_ctx_regs(ctx)->rsp = stack + pagesize;
#elif defined(LFI_ARCH_ARM64)
    lfi_ctx_regs(ctx)->sp = stack + pagesize;
#endif

    int x = LFI_INVOKE(box, &ctx, p_prog, int, (int, int), 10, 32);
    assert(x == 42);
    printf("add(%d, %d) = %d\n", 10, 32, x);

    void *box_callback = lfi_box_register_cb(box, (void *) callback);
    void *box_callback_bench = lfi_box_register_cb(box,
        (void *) callback_bench);

    x = LFI_INVOKE(box, &ctx, p_prog_cb, int, (int (*)(int), int), box_callback,
        42);
    assert(x == 42);

#if defined(LFI_ARCH_X64)
    static uint8_t prog_cb_unaligned[] = {
        // clang-format off
        0x48, 0x83, 0xec, 0x08, // sub $8, %rsp
        0x48, 0x89, 0xf8,       // mov %rdi, %rax
        0x89, 0xf7,             // mov %esi, %edi
        0x89, 0xc0,             // mov %eax, %eax
        0x4c, 0x09, 0xf0,       // or %r14, %rax
        0xff, 0xd0,             // call *%rax (return address at offset 16)
        0x48, 0x83, 0xc4, 0x08, // add $8, %rsp
        0x83, 0xc0, 0x01,       // add $1, %eax
        0x41, 0x5b,             // pop %r11
        0x41, 0x83, 0xe3, 0xe0, // and $0xffffffe0, %r11d
        0x4d, 0x09, 0xf3,       // or %r14, %r11
        0x41, 0xff, 0xe3,       // jmp *%r11
        // clang-format on
    };

    lfiptr p_unaligned = lfi_box_mapany_noverify(box, pagesize,
        LFI_PROT_READ | LFI_PROT_WRITE, LFI_MAP_ANONYMOUS | LFI_MAP_PRIVATE, -1,
        0);
    assert(p_unaligned != (lfiptr) -1);
    lfiptr p_prog_cb_unaligned = lfi_box_copyto(box, p_unaligned,
        prog_cb_unaligned, sizeof(prog_cb_unaligned));
    r = lfi_box_mprotect_noverify(box, p_unaligned, pagesize,
        LFI_PROT_READ | LFI_PROT_EXEC);
    assert(r == 0);

    x = LFI_INVOKE(box, &ctx, p_prog_cb_unaligned, int, (int (*)(int), int),
        box_callback, 41);
    assert(x == 42);
#endif

    if (bench) {
        size_t iters = 100000000;
        long long unsigned start = time_ns();
        for (size_t i = 0; i < iters; i++) {
            LFI_INVOKE(box, &ctx, p_prog, int, (int, int), 10, 32);
        }
        long long unsigned elapsed = time_ns() - start;
        printf("time per invocation: %.1f ns\n",
            (float) elapsed / (float) iters);

        start = time_ns();
        for (size_t i = 0; i < iters; i++) {
            LFI_INVOKE(box, &ctx, p_prog_cb, int, (int (*)(int), int),
                box_callback_bench, 42);
        }
        elapsed = time_ns() - start;
        printf("time per invocation with callback: %.1f ns\n",
            (float) elapsed / (float) iters);
    }

    lfi_ctx_free(ctx);

    lfi_box_free(box);

    lfi_free(engine);

    return 0;
}
