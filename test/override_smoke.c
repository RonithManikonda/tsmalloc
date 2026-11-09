/*
 * Uses only the standard allocator functions, and is run with tsmalloc
 * injected. It finds ts_is_ours at run time, so it fails if the override
 * didn't take effect instead of passing against the system malloc.
 */
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <pthread.h>
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

#define REQUIRE_ZONE(z)                       \
    do {                                      \
        if ((z) == NULL) {                    \
            fprintf(stderr, "no zone\n");     \
            return 1;                         \
        }                                     \
    } while (0)

/* Called through these so the compiler can't remove pairs of calls. */
static void *(*volatile do_malloc)(size_t) = malloc;

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
    is_ours = (int (*)(const void *))dlsym(RTLD_DEFAULT, "ts_is_ours");
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

    /* stdio buffers, which libc allocates for itself. */
    printf("override_smoke: %d failures\n", failures);
    return failures != 0;
}
