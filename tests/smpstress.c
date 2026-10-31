/*
 * smpstress: a guest program for tests/linux-stress.sh. Original code.
 *
 *   smpstress SECONDS [MEGABYTES]
 *
 * Runs two threads per online hart, each pinned to one hart, for SECONDS.
 * Every operation makes Linux ask SquirrelBIOS for cross-hart work:
 *
 *   - mmap, touch and munmap in the shared address space: remote TLB
 *     flushes (SBI RFENCE remote_sfence_vma),
 *   - rewriting a small function that every thread calls, then
 *     riscv_flush_icache: remote instruction fences (remote_fence_i),
 *   - pipe ping-pong between threads on different harts: wake-ups by
 *     inter-processor interrupt (SBI IPI),
 *   - nanosleep: timer interrupts.
 *
 * With MEGABYTES, it first writes a pattern into that much memory, one
 * word per 4 KB page, and checks it back.
 *
 * Prints "smpstress: PASS ..." or "smpstress: FAIL ..." and exits 0 or 1.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_riscv_flush_icache
#define __NR_riscv_flush_icache (244 + 15)
#endif

static volatile int stop;
static volatile int failed;
static int nharts;
static uint32_t *code;                  /* li a0, N ; ret */
static volatile uint32_t code_gen;      /* the N in code, once flushed */
static pthread_mutex_t code_lock = PTHREAD_MUTEX_INITIALIZER;
static int pipes[512][2];
static unsigned long ops[512];

static void fail(const char *what, long a, long b)
{
    if (!__sync_lock_test_and_set(&failed, 1))
        printf("smpstress: FAIL %s (%ld, %ld)\n", what, a, b);
    stop = 1;
}

static void write_code(uint32_t n)
{
    /* addi a0, zero, n (12-bit) ; jalr zero, 0(ra) */
    code[0] = ((n & 0x7ff) << 20) | (10 << 7) | 0x13;
    code[1] = 0x00008067;
    __sync_synchronize();
    syscall(__NR_riscv_flush_icache, code, code + 2, 0);
}

static void *worker(void *p)
{
    long id = (long)p;
    int hart = id % nharts;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(hart, &set);
    /* A hart may be offline for a moment (CPU hotplug tests): run
     * anywhere then, and try to pin again later */
    int pinned = sched_setaffinity(0, sizeof set, &set) == 0;
    int peer = id ^ 1;                  /* threads come in pairs */
    unsigned long n = 0;
    while (!stop) {
        /* remote TLB flushes */
        size_t len = 16 * 4096;
        unsigned char *m = mmap(0, len, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            fail("mmap", id, n);
            break;
        }
        for (size_t i = 0; i < len; i += 4096) {
            if (m[i] != 0)
                fail("fresh page not zero", id, i);
            m[i] = (unsigned char)id;
        }
        if (mprotect(m, len, PROT_READ))
            fail("mprotect", id, n);
        for (size_t i = 0; i < len; i += 4096)
            if (m[i] != (unsigned char)id)
                fail("page lost its data", id, i);
        munmap(m, len);

        /* remote instruction fences: the code must never run stale */
        if (n % 8 == 0) {
            pthread_mutex_lock(&code_lock);
            uint32_t g = (code_gen + 1) & 0x7ff;
            write_code(g);
            code_gen = g;
            pthread_mutex_unlock(&code_lock);
        }
        pthread_mutex_lock(&code_lock);
        uint32_t want = code_gen;
        uint32_t got = ((uint32_t (*)(void))code)();
        pthread_mutex_unlock(&code_lock);
        if (got != want)
            fail("stale instructions after flush_icache", want, got);

        /* IPIs: ping-pong with the peer thread on another hart */
        char c = 0;
        if (id & 1) {
            if (read(pipes[id][0], &c, 1) != 1 || write(pipes[peer][1], &c, 1) != 1)
                fail("pipe", id, n);
        } else {
            if (write(pipes[peer][1], &c, 1) != 1 || read(pipes[id][0], &c, 1) != 1)
                fail("pipe", id, n);
        }

        /* timers */
        if (n % 16 == 0) {
            struct timespec ts = { 0, 100000 };
            nanosleep(&ts, 0);
        }
        if (!pinned && n % 64 == 0)
            pinned = sched_setaffinity(0, sizeof set, &set) == 0;
        n++;
    }
    ops[id] = n;
    /* let a peer blocked in read go */
    char c = 0;
    if (write(pipes[peer][1], &c, 1) < 0)
        ;
    return 0;
}

static int memcheck(long mb)
{
    size_t len = (size_t)mb << 20;
    uint64_t *m = mmap(0, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        printf("smpstress: FAIL mmap of %ld MB\n", mb);
        return 1;
    }
    size_t words = len / 8;
    for (size_t i = 0; i < words; i += 512)
        m[i] = i * 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < words; i += 512)
        if (m[i] != i * 0x9E3779B97F4A7C15ull) {
            printf("smpstress: FAIL memory at %zu MB\n", i * 8 >> 20);
            return 1;
        }
    munmap(m, len);
    printf("smpstress: %ld MB written and checked\n", mb);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    long mb = argc > 2 ? atol(argv[2]) : 0;
    setvbuf(stdout, 0, _IONBF, 0);
    if (mb && memcheck(mb))
        return 1;
    nharts = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = nharts * 2;
    if (nthreads > 512)
        nthreads = 512;
    code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        puts("smpstress: FAIL no executable page");
        return 1;
    }
    write_code(0);
    for (int i = 0; i < nthreads; i++)
        if (pipe(pipes[i])) {
            puts("smpstress: FAIL pipe");
            return 1;
        }
    pthread_t t[512];
    for (long i = 0; i < nthreads; i++)
        if (pthread_create(&t[i], 0, worker, (void *)i)) {
            printf("smpstress: FAIL pthread_create %ld\n", i);
            return 1;
        }
    sleep(secs);
    stop = 1;
    unsigned long total = 0, least = ~0ul;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(t[i], 0);
        total += ops[i];
        if (ops[i] < least)
            least = ops[i];
    }
    if (failed)
        return 1;
    if (least == 0) {
        puts("smpstress: FAIL a thread made no progress");
        return 1;
    }
    printf("smpstress: PASS %d harts, %d threads, %lu rounds (fewest %lu)\n",
           nharts, nthreads, total, least);
    return 0;
}
