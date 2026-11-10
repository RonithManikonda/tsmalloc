#include "test.h"

#include "page.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* TS_DEBUG checks, linked against a copy of the library built with them.
 *
 * Each misuse runs in a forked child, which has to die of SIGABRT with the
 * expected message on stderr. The parent is single-threaded whenever it
 * forks (threads from earlier tests have been joined). */

#ifndef TS_DEBUG
#error "test_debug needs the TS_DEBUG build of the library"
#endif

/* Allocation calls through volatile pointers, so the compiler can't reason
 * about the misuse below (it may warn about, or drop, a free of a freed
 * pointer or a write past what it knows malloc returned). */
static void *(*volatile do_malloc)(size_t) = ts_malloc;
static void (*volatile do_free)(void *) = ts_free;
static void *(*volatile do_realloc)(void *, size_t) = ts_realloc;
static size_t (*volatile do_usable)(const void *) = ts_usable_size;

static int dies_with(void (*fn)(void), const char *expected) {
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        alarm(20);
        fn();
        _exit(0); /* survived: the check didn't fire */
    }
    close(fds[1]);
    char out[512];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof out - 1 && (r = read(fds[0], out + n, sizeof out - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = '\0';
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return 0;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || strstr(out, expected) == NULL) {
        fprintf(stderr, "  wanted SIGABRT and \"%s\"; got status 0x%x, stderr:\n%s\n", expected,
                status, out);
        return 0;
    }
    return 1;
}

static const size_t sizes[] = {1, 16, 24, 100, 1000, 1024, 5000, 16384, 20000, 300000, 600000,
                               (size_t)4 << 20};

/* ---- correct programs run as usual ---- */

TEST(full_usable_size_can_be_written) {
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned char *p = ts_malloc(sizes[i]);
        REQUIRE(p != NULL);
        size_t usable = ts_usable_size(p);
        CHECK(usable >= sizes[i]);
        memset(p, 0x5A, usable);
        ts_free(p);
    }
}

TEST(realloc_keeps_contents_through_every_size) {
    unsigned char *p = NULL;
    size_t have = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        p = ts_realloc(p, sizes[i]);
        REQUIRE(p != NULL);
        for (size_t j = 0; j < have; j++)
            CHECK(p[j] == (unsigned char)j);
        size_t usable = ts_usable_size(p);
        for (size_t j = 0; j < usable; j++)
            p[j] = (unsigned char)j;
        have = sizes[i];
    }
    /* and back down, through in-place shrinks of the huge block */
    for (size_t i = sizeof sizes / sizeof sizes[0]; i-- > 0;) {
        p = ts_realloc(p, sizes[i]);
        REQUIRE(p != NULL);
        for (size_t j = 0; j < sizes[i]; j++)
            CHECK(p[j] == (unsigned char)j);
        size_t usable = ts_usable_size(p);
        for (size_t j = sizes[i]; j < usable; j++)
            p[j] = (unsigned char)j;
    }
    ts_free(p);
}

TEST(calloc_and_aligned_blocks_carry_canaries) {
    /* Up to 8 MiB, where a huge block's header sits in the chunk before it. */
    for (size_t align = 16; align <= ((size_t)8 << 20); align <<= 1) {
        unsigned char *p = ts_aligned_alloc(align, 100);
        REQUIRE(p != NULL);
        CHECK_EQ((uintptr_t)p % align, 0);
        memset(p, 1, ts_usable_size(p));
        ts_free(p);
    }
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned char *p = ts_calloc(1, sizes[i]);
        REQUIRE(p != NULL);
        for (size_t j = 0; j < sizes[i]; j++)
            if (p[j] != 0)
                CHECK(p[j] == 0);
        memset(p, 2, ts_usable_size(p));
        ts_free(p);
    }
}

TEST(freed_blocks_are_filled) {
    unsigned char *keep = ts_malloc(64); /* keeps the page from going back */
    unsigned char *p = ts_malloc(64);
    REQUIRE(keep != NULL && p != NULL);
    size_t usable = ts_usable_size(p);
    memset(p, 0, usable);
    ts_free(p);
    /* The first word is the free-list link now; the rest is fill. */
    size_t filled = 0;
    for (size_t i = sizeof(void *); i < usable; i++)
        filled += p[i] == 0xDF;
    CHECK_EQ(filled, usable - sizeof(void *));
    ts_free(keep);
}

enum { CROSS_BLOCKS = 2000 };
static void *cross[CROSS_BLOCKS];

static void *free_all(void *arg) {
    (void)arg;
    for (size_t i = 0; i < CROSS_BLOCKS; i++)
        ts_free(cross[i]);
    return NULL;
}

TEST(remote_frees_are_checked_too) {
    for (size_t i = 0; i < CROSS_BLOCKS; i++) {
        cross[i] = ts_malloc(16 + i % 700);
        REQUIRE(cross[i] != NULL);
        memset(cross[i], 7, ts_usable_size(cross[i]));
    }
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, free_all, NULL) == 0);
    pthread_join(t, NULL);
}

/* ---- misuse aborts ---- */

static void double_free_small(void) {
    void *keep = do_malloc(48);
    void *p = do_malloc(48);
    do_free(p);
    do_free(p);
    do_free(keep);
}

static void double_free_medium(void) {
    void *keep = do_malloc(20000);
    void *p = do_malloc(20000);
    do_free(p);
    do_free(p);
    do_free(keep);
}

static void double_free_huge(void) {
    void *p = do_malloc((size_t)1 << 20);
    do_free(p);
    do_free(p);
}

static void *remote_free(void *p) {
    do_free(p);
    return NULL;
}

static void double_free_remote(void) {
    void *keep = do_malloc(48);
    void *p = do_malloc(48);
    do_free(p);
    pthread_t t;
    if (pthread_create(&t, NULL, remote_free, p) != 0)
        _exit(9);
    pthread_join(t, NULL);
    do_free(keep);
}

static void realloc_after_free(void) {
    void *keep = do_malloc(200);
    void *p = do_malloc(200);
    do_free(p);
    (void)do_realloc(p, 400);
    do_free(keep);
}

static void usable_size_after_free(void) {
    void *keep = do_malloc(200);
    void *p = do_malloc(200);
    do_free(p);
    (void)do_usable(p);
    do_free(keep);
}

static void overrun_by_one(void) {
    unsigned char *p = do_malloc(100);
    p[do_usable(p)] = 0;
    do_free(p);
}

static void overrun_huge(void) {
    unsigned char *p = do_malloc((size_t)2 << 20);
    p[do_usable(p) + 3] = 0;
    do_free(p);
}

static void interior_pointer(void) {
    char *p = do_malloc(64);
    do_free(p + 16);
}

static void interior_pointer_huge(void) {
    char *p = do_malloc((size_t)1 << 20);
    do_free(p + 4096);
}

static _Alignas(64) char not_ours[64];

static void foreign_pointer(void) {
    do_free(not_ours);
}

static void misaligned_pointer(void) {
    char *p = do_malloc(64);
    do_free(p + 1);
}

/* A block past the page's bump cursor: inside our memory, never handed out. */
static void never_allocated(void) {
    char *p = do_malloc(2900);
    ts_page_t *page = ts_ptr_page(ts_ptr_segment(p), p);
    if (page->reserved >= page->capacity)
        _exit(8);
    do_free(ts_page_block_at(page, page->reserved));
}

TEST(double_free_aborts) {
    CHECK(dies_with(double_free_small, "double free"));
    CHECK(dies_with(double_free_medium, "double free"));
    CHECK(dies_with(double_free_remote, "double free"));
    CHECK(dies_with(realloc_after_free, "double free"));
    CHECK(dies_with(usable_size_after_free, "double free"));
    /* A freed huge block is unmapped or cached, and either way no longer
     * registered, so it's reported as not ours. */
    CHECK(dies_with(double_free_huge, "not allocated by tsmalloc"));
}

TEST(overrun_aborts) {
    CHECK(dies_with(overrun_by_one, "write past the end"));
    CHECK(dies_with(overrun_huge, "write past the end"));
}

TEST(bad_pointers_abort) {
    CHECK(dies_with(interior_pointer, "middle of a block"));
    /* The registry knows a huge block's one valid address. */
    CHECK(dies_with(interior_pointer_huge, "not allocated by tsmalloc"));
    CHECK(dies_with(foreign_pointer, "not allocated by tsmalloc"));
    CHECK(dies_with(misaligned_pointer, "not allocated by tsmalloc"));
    CHECK(dies_with(never_allocated, "never allocated"));
}
