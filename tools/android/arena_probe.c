// Does a 64-bit Android process get 128 MB below the 4 GB line?
//
// This is the first question of the Android port and the one everything else
// waits on, so it is asked by a twenty-line program on a real device rather
// than by reasoning about ASLR.
//
// **Why it matters.** gMemInfo.DRAM.addr is a U32 and xMemInitHeap does
// pointer arithmetic on it as an integer, so every address the game allocator
// hands out has to survive the round trip back to a pointer. iMemMgr.cpp
// therefore reserves the arena through iHostReserveLow and REFUSES TO START
// if what comes back is above 4 GB. See src/SB/Core/pc/iMemMgr.cpp.
//
// On x86-64 Linux that is MAP_32BIT and the question does not arise. On arm64
// there is no such flag, so iHostPosix.cpp walks hints from 0x04000000 up and
// checks what it gets -- and whether any of those hints is honoured depends on
// the kernel's mmap_min_addr, on where the loader has already put things, and
// on how the device's ASLR is configured. None of that is knowable from here.
//
// This program runs exactly the loop iHostReserveLow runs, with exactly the
// arena size iMemMgr asks for, and says what happened. A pass means the port
// can be arm64. A failure means armeabi-v7a, where pointers are four bytes
// wide natively and the question disappears -- at the price of Google Play
// requiring a 64-bit binary and of 32-bit ARM being absent from new silicon.
//
// Build and run:
//
//   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
//       -O2 -o arena_probe tools/android/arena_probe.c
//   adb push arena_probe /data/local/tmp/
//   adb shell chmod 755 /data/local/tmp/arena_probe
//   adb shell /data/local/tmp/arena_probe
//
// Exits 0 if the arena can be had and 1 if it cannot, so it is usable from a
// script.
//
// **One caveat, and it is worth reading before trusting a pass.** This is a
// bare executable run from a shell, and the game is a library inside a zygote
// fork with a JVM, ART's heaps, SDL and the graphics driver already mapped.
// The low addresses are likelier to be occupied there, not less. So a failure
// here settles the question and a pass only makes it worth trying: the real
// confirmation is the port itself reaching iMemInit and not printing
// "outside the low 4 GB". That is phase 2, not phase 1.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

// iMemMgr.cpp: IMEM_DRAM_SIZE * 2. The second half is for gxHeap[1] and
// gxHeap[2], which xMemInit places past the block retail allocated for DRAM.
#define ARENA_SIZE (0x4000000u * 2u)

#define LOW_LIMIT 0x100000000ULL

static void report_mmap_min_addr(void)
{
    FILE* f = fopen("/proc/sys/vm/mmap_min_addr", "r");
    if (f == NULL)
    {
        printf("mmap_min_addr: unreadable (not fatal; the loop below is the test)\n");
        return;
    }

    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) == 1)
    {
        printf("mmap_min_addr: %llu (0x%llx)\n", v, v);
    }

    fclose(f);
}

int main(void)
{
    const int prot = PROT_READ | PROT_WRITE;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    printf("arena probe: %u bytes (%u MB), must end at or below 0x%llx\n",
           ARENA_SIZE, ARENA_SIZE / (1024 * 1024), LOW_LIMIT);
    printf("pointer size: %zu bytes\n", sizeof(void*));

    report_mmap_min_addr();

    // What the OS offers unasked, for contrast. This is what the game would
    // get if the loop below found nothing, and is printed first because the
    // distance between it and 4 GB is the whole problem in one number.
    void* any = mmap(NULL, ARENA_SIZE, prot, flags, -1, 0);
    if (any != MAP_FAILED)
    {
        printf("unhinted mmap: %p%s\n", any,
               (uint64_t)(uintptr_t)any + ARENA_SIZE <= LOW_LIMIT ? "  (already low!)" : "");
        munmap(any, ARENA_SIZE);
    }
    else
    {
        printf("unhinted mmap: FAILED -- this device cannot spare %u MB at all\n",
               ARENA_SIZE / (1024 * 1024));
        return 1;
    }

    // The loop from iHostReserveLow, flag for flag. MAP_FIXED_NOREPLACE makes
    // a hint binding: the mapping lands exactly there or fails with EEXIST,
    // rather than being answered with any address the kernel likes. A kernel
    // that does not know the flag ignores it, which leaves a plain hint, so
    // the result is checked either way.
    int lowflags = flags;
#ifdef MAP_FIXED_NOREPLACE
    lowflags |= MAP_FIXED_NOREPLACE;
#else
    printf("note: no MAP_FIXED_NOREPLACE in these headers; hints are advisory\n");
#endif

    int tried = 0;

    for (uint64_t base = 0x04000000ULL; base < 0x80000000ULL; base += 0x04000000ULL)
    {
        tried++;

        void* p = mmap((void*)(uintptr_t)base, ARENA_SIZE, prot, lowflags, -1, 0);
        if (p == MAP_FAILED)
        {
            continue;
        }

        if ((uint64_t)(uintptr_t)p + ARENA_SIZE <= LOW_LIMIT)
        {
            printf("PASS: %u MB at %p, on hint %d of 31 (0x%llx)\n",
                   ARENA_SIZE / (1024 * 1024), p, tried,
                   (unsigned long long)base);

            // Touch both ends. A mapping that cannot be written is not an
            // arena, and on a device under memory pressure the difference
            // between reserving and committing is where that shows up.
            memset(p, 0xAB, 4096);
            memset((char*)p + ARENA_SIZE - 4096, 0xCD, 4096);
            printf("      written at both ends; usable\n");

            munmap(p, ARENA_SIZE);
            return 0;
        }

        munmap(p, ARENA_SIZE);
    }

    printf("FAIL: no %u MB block below 4 GB after %d hints.\n",
           ARENA_SIZE / (1024 * 1024), tried);
    printf("      This device cannot run an arm64 build of the port as it stands.\n");
    printf("      Build armeabi-v7a instead: abiFilters in android/app/build.gradle.\n");
    return 1;
}
