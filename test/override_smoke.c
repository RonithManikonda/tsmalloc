/*
 * Uses only the standard allocator functions, and is run with tsmalloc
 * injected. It finds ts_is_ours at run time, so it fails if the override
 * didn't take effect instead of passing against the system malloc.
 */
#ifdef __linux__
#define _GNU_SOURCE /* RTLD_DEFAULT, reallocarray, memalign, ... */
#endif

#include <dlfcn.h>
#include <errno.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#define malloc_size malloc_usable_size
#endif
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int (*is_ours)(const void *);
static int failures;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #cond); \
            __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);          \
        }                                                                \
    } while (0)

#ifdef __APPLE__
#define REQUIRE_ZONE(z)                       \
    do {                                      \
        if ((z) == NULL) {                    \
            fprintf(stderr, "no zone\n");     \
            return 1;                         \
        }                                     \
    } while (0)

#endif

/* Called through these so the compiler can't remove pairs of calls. */
static void *(*volatile do_malloc)(size_t) = malloc;
#ifdef __linux__
static void (*volatile do_free)(void *) = free;
static void *(*volatile do_calloc)(size_t, size_t) = calloc;
static void *(*volatile do_realloc)(void *, size_t) = realloc;
static void *(*volatile do_reallocarray)(void *, size_t, size_t) = reallocarray;
static void *(*volatile do_memalign)(size_t, size_t) = memalign;
static void *(*volatile do_pvalloc)(size_t) = pvalloc;
static void *(*volatile do_aligned_alloc)(size_t, size_t) = aligned_alloc;
static int (*volatile do_posix_memalign)(void **, size_t, size_t) = posix_memalign;
#endif

static void *worker(void *arg) {
    void **handoff = arg;
    for (int i = 0; i < 2000; i++) {
        size_t size = 16 + (size_t)(i * 37) % 4096;
        char *p = do_malloc(size);
        EXPECT(p != NULL && is_ours(p));
        memset(p, 0x41, size);
        free(p);
    }
    /* Freed by the main thread: a remote free through the plain API. */
    *handoff = do_malloc(128);
    return NULL;
}

int main(void) {
    is_ours = __extension__(int (*)(const void *))dlsym(RTLD_DEFAULT, "ts_is_ours");
    if (is_ours == NULL) {
        fprintf(stderr, "tsmalloc is not loaded\n");
        return 1;
    }

    static const size_t sizes[] = {1, 64, 4096, 100000, 1 << 20, 8 << 20};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char *p = do_malloc(sizes[i]);
        EXPECT(p != NULL && is_ours(p));
        EXPECT(malloc_size(p) >= sizes[i]);
        memset(p, 0x5A, sizes[i]);
        p = realloc(p, sizes[i] * 2);
        EXPECT(p != NULL && is_ours(p));
        EXPECT((unsigned char)p[sizes[i] - 1] == 0x5A);
        free(p);
    }

    unsigned char *z = calloc(1000, 8);
    EXPECT(z != NULL && is_ours(z) && z[7999] == 0);
    free(z);

    void *aligned = NULL;
    EXPECT(posix_memalign(&aligned, 4096, 100) == 0 && is_ours(aligned));
    EXPECT(((size_t)aligned & 4095) == 0);
    free(aligned);

    void *paged = valloc(100);
    EXPECT(paged != NULL && is_ours(paged));
    free(paged);

    char *copy = strdup("through libc's own malloc");
    EXPECT(copy != NULL && is_ours(copy));
    free(copy);

#ifdef __APPLE__
    /* tsmalloc is the default zone, so the zone API lands on it as well. */
    malloc_zone_t *zone = malloc_default_zone();
    char *via_zone = malloc_zone_malloc(zone, 300);
    EXPECT(via_zone != NULL && is_ours(via_zone));
    EXPECT(malloc_zone_from_ptr(via_zone) != NULL);
    via_zone = malloc_zone_realloc(zone, via_zone, 70000);
    EXPECT(via_zone != NULL && is_ours(via_zone));
    malloc_zone_free(zone, via_zone);

    void *via_memalign = malloc_zone_memalign(zone, 256, 300);
    EXPECT(via_memalign != NULL && is_ours(via_memalign) && ((size_t)via_memalign & 255) == 0);
    free(via_memalign);

    void *batch[16];
    unsigned got = malloc_zone_batch_malloc(zone, 64, batch, 16);
    EXPECT(got == 16);
    for (unsigned i = 0; i < got; i++)
        EXPECT(is_ours(batch[i]));
    malloc_zone_batch_free(zone, batch, got);

    EXPECT(malloc_good_size(100) >= 100);

    /* From a private zone, so it isn't ours, and the interposed functions
     * have to hand it back rather than read a header from it. */
    malloc_zone_t *private_zone = malloc_create_zone(0, 0);
    REQUIRE_ZONE(private_zone);
    char *foreign = malloc_zone_malloc(private_zone, 200);
    EXPECT(foreign != NULL && !is_ours(foreign));
    EXPECT(malloc_zone_from_ptr(foreign) == private_zone);
    EXPECT(malloc_size(foreign) >= 200);
    strcpy(foreign, "still here");
    foreign = realloc(foreign, 100000);
    EXPECT(foreign != NULL && strcmp(foreign, "still here") == 0);
    free(foreign);
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    void *m = do_memalign(256, 300);
    EXPECT(m != NULL && is_ours(m) && ((size_t)m & 255) == 0);
    do_free(m);
    /* Not a power of two: rounded up to 128. */
    m = do_memalign(100, 300);
    EXPECT(m != NULL && is_ours(m) && ((size_t)m & 127) == 0);
    do_free(m);
    m = do_memalign(1, 50);
    EXPECT(m != NULL && is_ours(m));
    do_free(m);
    errno = 0;
    EXPECT(do_memalign(SIZE_MAX, 16) == NULL && errno == EINVAL);

    char *pv = do_pvalloc(page + 1);
    EXPECT(pv != NULL && is_ours(pv) && ((size_t)pv & (page - 1)) == 0);
    EXPECT(malloc_usable_size(pv) >= 2 * page);
    pv[2 * page - 1] = 1;
    do_free(pv);
    pv = do_pvalloc(0);
    EXPECT(pv != NULL && malloc_usable_size(pv) >= page);
    do_free(pv);
    errno = 0;
    EXPECT(do_pvalloc(SIZE_MAX - 1) == NULL && errno == ENOMEM);

    void *al = do_aligned_alloc(64, 100);
    EXPECT(al != NULL && is_ours(al) && ((size_t)al & 63) == 0);
    do_free(al);
    errno = 0;
    EXPECT(do_aligned_alloc(3, 100) == NULL && errno == EINVAL);
    void *untouched = &failures;
    EXPECT(do_posix_memalign(&untouched, 3, 100) == EINVAL && untouched == &failures);

    char *arr = do_reallocarray(NULL, 10, 100);
    EXPECT(arr != NULL && is_ours(arr));
    arr[999] = 7;
    arr = do_reallocarray(arr, 20, 100);
    EXPECT(arr != NULL && is_ours(arr) && arr[999] == 7);
    errno = 0;
    EXPECT(do_reallocarray(arr, SIZE_MAX, 2) == NULL && errno == ENOMEM);
    do_free(arr); /* the failed call left it alone */

    /* glibc's behaviour, which differs from ts_realloc's. */
    void *gone = do_malloc(40);
    EXPECT(do_realloc(gone, 0) == NULL);
    void *from_null = do_realloc(NULL, 0);
    EXPECT(from_null != NULL && is_ours(from_null));
    do_free(from_null);

    errno = 0;
    EXPECT(do_calloc(SIZE_MAX, 2) == NULL && errno == ENOMEM);
    EXPECT(malloc_usable_size(NULL) == 0);

    /* Not ours: has to be left alone rather than read as a block. */
    char *foreign = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT(foreign != MAP_FAILED && !is_ours(foreign));
    strcpy(foreign, "still here");
    do_free(foreign);
    EXPECT(malloc_usable_size(foreign) == 0);
    errno = 0;
    EXPECT(do_realloc(foreign, 100) == NULL && errno == ENOMEM);
    EXPECT(strcmp(foreign, "still here") == 0);
    munmap(foreign, page);
#endif
    free(NULL);

    pthread_t t[8];
    void *handoff[8];
    for (int i = 0; i < 8; i++)
        EXPECT(pthread_create(&t[i], NULL, worker, &handoff[i]) == 0);
    for (int i = 0; i < 8; i++) {
        pthread_join(t[i], NULL);
        EXPECT(is_ours(handoff[i]));
        free(handoff[i]);
    }

    /* A child of a multithreaded process, with the override still in place. */
    char *inherited = do_malloc(4096);
    memset(inherited, 7, 4096);
    pid_t pid = fork();
    EXPECT(pid >= 0);
    if (pid == 0) {
        alarm(30);
        int ok = inherited[4095] == 7 && is_ours(inherited);
        free(inherited);
        char *fresh = do_malloc(1 << 20);
        ok = ok && fresh != NULL && is_ours(fresh);
        free(fresh);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    EXPECT(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(inherited);

    /* stdio buffers, which libc allocates for itself. */
    printf("override_smoke: %d failures\n", failures);
    return failures != 0;
}
