/* linuxchurn.c : stress the fork page-copy and frame-reuse paths and verify
 * memory integrity, to reproduce and characterize the intermittent cc1 heap
 * corruption on the 4 KiB-granule arm64/KVM path far faster than a kernel
 * build.
 *
 * Several workers run concurrently, like make -j, so fork/exit of many address
 * spaces interleaves the way the real build churns the frame allocator. Each
 * worker repeats: mmap an anonymous chunk, check it reads as zero (a non-zero
 * read means a reused frame still held a prior owner's data), fill every
 * pointer-sized slot with its own address, verify the fill in-process, then
 * fork a child that re-verifies the inherited copy (a mismatch there means the
 * fork page-copy produced bad child data) before it exits. The parent reaps,
 * unmaps, and repeats, so frames cycle through alloc and free under load.
 *
 * A mismatch prints want/got/at so its shape is visible. got == (want |
 * 0x100000000) is the bit-32 / physmap signature; anything else is stale or
 * random data. The run does not stop at the first hit, so repeated samples
 * accumulate. Args: workers chunk_mib rounds do_fork (defaults 4 8 200 1;
 * do_fork 0 drops the child, leaving only the zero-on-reuse check). Run with
 * `make linux-spike SPIKE_PROG="linuxchurn 4 16 1000"`. An ordinary Linux
 * binary. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static void
say(const char *s)
{
    write(1, s, strlen(s));
}

static void
say_hex(uint64_t v)
{
    char b[19];
    int i = 18;
    b[i--] = 0;
    if (v == 0)
        b[i--] = '0';
    while (v) {
        int d = (int)(v & 0xf);
        b[i--] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    b[i--] = 'x';
    b[i] = '0';
    say(&b[i]);
}

static void
say_dec(uint64_t v)
{
    char t[24];
    int ti = 0;
    if (v == 0)
        t[ti++] = '0';
    while (v) {
        t[ti++] = (char)('0' + v % 10);
        v /= 10;
    }
    char b[24];
    int l = 0;
    while (ti > 0)
        b[l++] = t[--ti];
    write(1, b, (size_t)l);
}

static uint64_t
parse_u(const char *s, uint64_t dflt)
{
    if (!s || !*s)
        return dflt;
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (uint64_t)(*s - '0');
    return v;
}

/* Fill every slot with its own address. */
static void
fill(uintptr_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uintptr_t)&p[i];
}

/* Verify every slot still equals its own address. On the first mismatch print
 * want/got/at plus a tag for the bit-32 signature, and return 1. A clean pass
 * returns 0. The caller labels the context (self, child, zero). */
static int
verify(uintptr_t *p, size_t n, const char *ctx, int round)
{
    for (size_t i = 0; i < n; i++) {
        uintptr_t want = (uintptr_t)&p[i];
        uintptr_t got = p[i];
        if (got != want) {
            say("[CHURN] ");
            say(ctx);
            say(" round "); say_dec((uint64_t)round);
            say(" at "); say_hex((uint64_t)want);
            say(" want "); say_hex((uint64_t)want);
            say(" got "); say_hex((uint64_t)got);
            if (((uint64_t)got ^ (uint64_t)want) == 0x100000000ULL)
                say(" BIT32");
            say("\n");
            return 1;
        }
    }
    return 0;
}

/* A fresh anonymous mapping must read as all zero. A non-zero word means the
 * frame handed out still carried a previous owner's data. */
static int
check_zero(uintptr_t *p, size_t n, int round)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            say("[CHURN] stale round "); say_dec((uint64_t)round);
            say(" at "); say_hex((uint64_t)&p[i]);
            say(" got "); say_hex((uint64_t)p[i]);
            say("\n");
            return 1;
        }
    }
    return 0;
}

static int
worker(size_t bytes, int rounds, int id, int do_fork)
{
    size_t n = bytes / sizeof(uintptr_t);
    int hits = 0;

    for (int r = 0; r < rounds; r++) {
        uintptr_t *p = mmap(0, bytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            say("[CHURN] mmap failed worker "); say_dec((uint64_t)id);
            say(" round "); say_dec((uint64_t)r); say("\n");
            return hits + 1;
        }

        /* A fresh mapping must read as zero: the central check. Then fill with
         * self-addresses so this frame carries recognizable non-zero data into
         * whichever process reuses it next, which is what makes a later stale
         * read identifiable. */
        hits += check_zero(p, n, r);
        fill(p, n);
        hits += verify(p, n, "self", r);

        if (do_fork) {
            pid_t pid = fork();
            if (pid == 0) {
                /* Child: re-verify the page-copy it inherited, then dirty every
                 * slot with a recognizable non-zero pattern through its
                 * cacheable mapping, leaving dirty cache lines over these
                 * frames. The child's frames are freed when it exits and handed
                 * to the parent's next mmap; if zero-on-reuse does not evict
                 * these lines, the parent reads this pattern back instead of
                 * zero, which is the stale-frame bug. */
                int bad = verify(p, n, "child", r);
                for (size_t i = 0; i < n; i++)
                    p[i] = (uintptr_t)0xC1C1C1C1C1C1C1C1ULL;
                _exit(bad ? 2 : 0);
            } else if (pid > 0) {
                int st = 0;
                waitpid(pid, &st, 0);
                if (!WIFEXITED(st) || WEXITSTATUS(st) == 2)
                    hits++;
            }
            /* pid < 0: fork pressure, skip the child this round. */
        }

        munmap(p, bytes);
    }
    return hits;
}

int
main(int argc, char **argv)
{
    uint64_t workers = parse_u(argc > 1 ? argv[1] : 0, 4);
    uint64_t mib = parse_u(argc > 2 ? argv[2] : 0, 8);
    uint64_t rounds = parse_u(argc > 3 ? argv[3] : 0, 200);
    uint64_t do_fork = parse_u(argc > 4 ? argv[4] : 0, 1);
    size_t bytes = (size_t)mib * 1024 * 1024;

    if (workers < 1)
        workers = 1;

    say("churn: workers="); say_dec(workers);
    say(" chunk_mib="); say_dec(mib);
    say(" rounds="); say_dec(rounds);
    say(" fork="); say_dec(do_fork);
    say("\n");

    for (uint64_t w = 0; w < workers; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            int h = worker(bytes, (int)rounds, (int)w, (int)do_fork);
            _exit(h ? 1 : 0);
        }
        /* pid <= 0: on fork failure fall through; fewer workers still churn. */
    }

    int total_bad = 0;
    int st = 0;
    while (waitpid(-1, &st, 0) > 0) {
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            total_bad++;
    }

    if (total_bad == 0)
        say("churn: clean\n");
    else {
        say("churn: CORRUPTION in "); say_dec((uint64_t)total_bad);
        say(" workers\n");
    }
    return total_bad ? 1 : 0;
}
