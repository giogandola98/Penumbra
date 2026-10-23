/* Host tests for the power-save hold around ML's card I/O (src/fio-ml.c).
 *
 * src/fio-ml.c is compiled unchanged (a copy, so its #include "..." resolve to fio_shim/) against
 * fake Canon FIO stubs and a fake hold API. Every public FIO_* entry point and helper is driven
 * through success, failure and early-return paths, and the test checks that:
 *   - every Canon stub call happens while a "fio" hold is taken;
 *   - holds and releases pair up after every call (no leak, no extra release);
 *   - nesting works (FIO_CreateFile -> is_dir -> FIO_FindFirstEx);
 *   - when the hold cannot be taken (before init, interrupt, IRQs masked) nothing is released. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dryos.h"
#include "fio-ml.h"

static int failures;
static int checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int shim_cacheable;
int shim_fio_malloc_fail;

void shim_assert(const char * msg, int line)
{
    printf("FAIL fio-ml.c:%d: ASSERT(%s)\n", line, msg);
    failures++;
}

/* ------------------------------------------------------- fake hold API -- */

static int holds;           /* current "fio" nesting */
static int max_holds;
static int takes, releases;
static int hold_allowed = 1;    /* 0: what powersave_hold_try does in an interrupt / before init */

int powersave_hold_try(const char * who)
{
    CHECK(who && strcmp(who, "fio") == 0);
    if (!hold_allowed) return 0;
    takes++;
    if (++holds > max_holds) max_holds = holds;
    return 1;
}

int powersave_release(const char * who)
{
    CHECK(who && strcmp(who, "fio") == 0);
    releases++;
    CHECK(holds > 0);
    holds--;
    return 0;
}

/* -------------------------------------------------- fake Canon stubs -- */

static int stub_calls;
static int unheld_calls;

static void stub(const char * name)
{
    stub_calls++;
    if (hold_allowed && holds <= 0)
    {
        unheld_calls++;
        printf("FAIL %s called without a fio hold\n", name);
        failures++;
    }
}

#define MAX_NODES 32
static struct { char path[FIO_MAX_PATH_LENGTH]; int is_dir; uint32_t size; } nodes[MAX_NODES];
static struct { int used; int node; uint32_t pos; } handles[8];
static int write_fail;

static int find(const char * path)
{
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].path[0] && strcmp(nodes[i].path, path) == 0)
            return i;
    return -1;
}

static int add(const char * path, int is_dir, uint32_t size)
{
    int i = find(path);
    for (int j = 0; i < 0 && j < MAX_NODES; j++)
        if (!nodes[j].path[0]) i = j;
    if (i < 0) return -1;
    snprintf(nodes[i].path, sizeof(nodes[i].path), "%s", path);
    nodes[i].is_dir = is_dir;
    nodes[i].size = size;
    return i;
}

static int parent_exists(const char * path)
{
    const char * slash = strrchr(path, '/');
    if (!slash || slash - path <= 2) return 1;      /* "B:/x" */
    char dir[FIO_MAX_PATH_LENGTH];
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    int i = find(dir);
    return i >= 0 && nodes[i].is_dir;
}

static FILE * new_handle(int node)
{
    for (int h = 0; h < 8; h++)
        if (!handles[h].used)
        {
            handles[h].used = 1; handles[h].node = node; handles[h].pos = 0;
            return (FILE *)(uintptr_t)(0x100 + h * 0x10);
        }
    return PTR_INVALID;
}

static int handle_of(FILE * f)
{
    int h = (int)(((uintptr_t) f - 0x100) / 0x10);
    return (h >= 0 && h < 8 && handles[h].used) ? h : -1;
}

static int open_handles(void)
{
    int n = 0;
    for (int h = 0; h < 8; h++) n += handles[h].used;
    return n;
}

FILE * _FIO_OpenFile(const char * name, unsigned mode)
{
    (void) mode;
    stub(__func__);
    int i = find(name);
    return (i >= 0 && !nodes[i].is_dir) ? new_handle(i) : PTR_INVALID;
}

FILE * _FIO_CreateFile(const char * name)
{
    stub(__func__);
    if (!parent_exists(name)) return PTR_INVALID;
    int i = add(name, 0, 0);
    return i >= 0 ? new_handle(i) : PTR_INVALID;
}

int _FIO_RemoveFile(const char * name)
{
    stub(__func__);
    int i = find(name);
    if (i < 0) return -1;
    nodes[i].path[0] = 0;
    return 0;
}

int _FIO_GetFileSize(const char * name, uint32_t * size)
{
    stub(__func__);
    int i = find(name);
    if (i < 0 || nodes[i].is_dir) return -1;
    *size = nodes[i].size;
    return 0;
}

int _FIO_CreateDirectory(const char * name)
{
    stub(__func__);
    return add(name, 1, 0) >= 0 ? 0 : -1;
}

int _FIO_RenameFile(const char * src, const char * dst)
{
    stub(__func__);
    int i = find(src);
    if (i < 0) return -1;
    snprintf(nodes[i].path, sizeof(nodes[i].path), "%s", dst);
    return 0;
}

struct fio_dirent * _FIO_FindFirstEx(const char * dir, struct fio_file * file)
{
    (void) file;
    stub(__func__);
    if (strcmp(dir, "B:/") == 0) return (struct fio_dirent *) 0x1000;
    int i = find(dir);
    return (i >= 0 && nodes[i].is_dir) ? (struct fio_dirent *) 0x1000 : (struct fio_dirent *) 1;
}

int _FIO_FindNextEx(struct fio_dirent * dirent, struct fio_file * file)
{
    (void) dirent; (void) file;
    stub(__func__);
    return 1;
}

void _FIO_FindClose(struct fio_dirent * dirent)
{
    (void) dirent;
    stub(__func__);
}

void _FIO_CloseFile(FILE * f)
{
    stub(__func__);
    int h = handle_of(f);
    CHECK(h >= 0);
    if (h >= 0) handles[h].used = 0;
}

int64_t _FIO_SeekSkipFile(FILE * f, int64_t pos, int whence)
{
    stub(__func__);
    int h = handle_of(f);
    if (h < 0) return -1;
    handles[h].pos = whence == SEEK_END ? nodes[handles[h].node].size : (uint32_t) pos;
    return handles[h].pos;
}

int _FIO_ReadFile(FILE * f, void * buf, size_t count)
{
    stub(__func__);
    int h = handle_of(f);
    if (h < 0) return -1;
    uint32_t left = nodes[handles[h].node].size - handles[h].pos;
    uint32_t n = count < left ? count : left;
    memset(buf, 'x', n);
    handles[h].pos += n;
    return n;
}

int _FIO_WriteFile(FILE * f, const void * buf, size_t count)
{
    (void) buf;     /* never read: dump_big_seg passes raw addresses */
    stub(__func__);
    int h = handle_of(f);
    if (h < 0 || write_fail) return -1;
    handles[h].pos += count;
    if (handles[h].pos > nodes[handles[h].node].size)
        nodes[handles[h].node].size = handles[h].pos;
    return count;
}

/* --------------------------------------------------------------- tests -- */

/* after every public call: everything taken was released */
#define BALANCED() do { CHECK(holds == 0); CHECK(takes == releases); } while (0)

static void reset(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(handles, 0, sizeof(handles));
    holds = max_holds = takes = releases = 0;
    stub_calls = unheld_calls = 0;
    hold_allowed = 1;
    write_fail = shim_fio_malloc_fail = shim_cacheable = 0;
}

static void test_create_write_close(void)
{
    reset();
    add("B:/ML", 1, 0);

    /* missing B:/ML/LOGS: remove, create (fails), is_dir walk, mkdir, create */
    FILE * f = FIO_CreateFile("ML/LOGS/a.txt");
    BALANCED();
    CHECK(f != NULL);
    CHECK(find("B:/ML/LOGS") >= 0);
    CHECK(max_holds >= 2);                  /* is_dir -> FIO_FindFirstEx nested in the create */

    CHECK(FIO_WriteFile(f, "hello", 5) == 5);
    BALANCED();
    shim_cacheable = 1;                     /* cached source buffer: sync_caches path */
    CHECK(FIO_WriteFile(f, "world", 5) == 5);
    BALANCED();
    CHECK(FIO_SeekSkipFile(f, 0, SEEK_SET) == 0);
    BALANCED();
    my_fprintf(f, "%d %s", 42, "x");
    BALANCED();
    FIO_CloseFile(f);
    BALANCED();
    CHECK(open_handles() == 0);
    CHECK(nodes[find("B:/ML/LOGS/a.txt")].size == 10);
}

static void test_open_read(void)
{
    reset();
    add("B:/a.bin", 0, 100);

    CHECK(FIO_OpenFile("B:/missing", O_RDONLY) == NULL);
    BALANCED();

    FILE * f = FIO_OpenFile("B:/a.bin", O_RDONLY);
    BALANCED();
    CHECK(f != NULL);
    char buf[64];
    shim_cacheable = 0;                     /* uncacheable (DMA) buffer: direct read */
    CHECK(FIO_ReadFile(f, buf, 40) == 40);
    BALANCED();
    shim_cacheable = 1;                     /* cacheable buffer: bounce through fio_malloc */
    CHECK(FIO_ReadFile(f, buf, 64) == 60);
    BALANCED();
    FIO_CloseFile(f);
    BALANCED();

    char rb[128];
    CHECK(read_file("B:/a.bin", rb, sizeof(rb)) == 100);
    BALANCED();
    CHECK(read_file("B:/missing", rb, sizeof(rb)) == (size_t) -1);
    BALANCED();

    shim_cacheable = 0;
    int size = 0;
    uint8_t * all = read_entire_file("B:/a.bin", &size);
    BALANCED();
    CHECK(all && size == 100);
    free(all);
    CHECK(read_entire_file("B:/missing", &size) == NULL);
    BALANCED();
    CHECK(open_handles() == 0);
}

static void test_metadata(void)
{
    reset();
    add("B:/DCIM", 1, 0);
    add("B:/f.txt", 0, 7);

    uint32_t size = 0;
    CHECK(FIO_GetFileSize("B:/f.txt", &size) == 0 && size == 7);
    BALANCED();
    CHECK(FIO_GetFileSize("B:/nope", &size) != 0);
    BALANCED();
    CHECK(FIO_GetFileSize_direct("B:/f.txt") == 7);
    BALANCED();
    CHECK(FIO_GetFileSize_direct("B:/nope") == 0xFFFFFFFF);
    BALANCED();
    CHECK(is_file("B:/f.txt") && !is_file("B:/nope"));
    BALANCED();
    CHECK(is_dir("B:/DCIM") && !is_dir("B:/nope"));
    BALANCED();

    CHECK(FIO_CreateDirectory("B:/DCIM") == 0);     /* exists: is_dir only */
    BALANCED();
    CHECK(FIO_CreateDirectory("B:/NEW") == 0);
    BALANCED();
    CHECK(is_dir("B:/NEW"));

    CHECK(FIO_RenameFile("B:/f.txt", "B:/g.txt") == 0);
    BALANCED();
    CHECK(FIO_RemoveFile("B:/g.txt") == 0);
    BALANCED();
    CHECK(FIO_RemoveFile("B:/g.txt") != 0);
    BALANCED();

    struct fio_file file;
    struct fio_dirent * d = FIO_FindFirstEx("B:/DCIM", &file);
    BALANCED();
    CHECK(!IS_ERROR(d));
    CHECK(FIO_FindNextEx(d, &file) != 0);
    BALANCED();
    FIO_FindClose(d);
    BALANCED();

    char name[32];
    add("B:/N00.TXT", 0, 3);
    CHECK(get_numbered_file_name("B:/N%02d.TXT", 5, name, sizeof(name)) == 1);
    BALANCED();
}

static void test_copy_move_append(void)
{
    reset();
    add("B:/src.bin", 0, 300 * 1024);

    CHECK(FIO_CopyFile("B:/src.bin", "B:/dst.bin") == 0);
    BALANCED();
    CHECK(nodes[find("B:/dst.bin")].size == 300 * 1024);
    CHECK(max_holds >= 2);                  /* the whole copy is one outer bracket */

    CHECK(FIO_MoveFile("B:/dst.bin", "B:/moved.bin") == 0);
    BALANCED();
    CHECK(find("B:/dst.bin") < 0 && find("B:/moved.bin") >= 0);

    write_fail = 1;                         /* copy fails half way: dst removed */
    CHECK(FIO_CopyFile("B:/src.bin", "B:/bad.bin") != 0);
    BALANCED();
    CHECK(find("B:/bad.bin") < 0);
    write_fail = 0;

    shim_fio_malloc_fail = 1;               /* no buffer: both files closed, dst removed */
    CHECK(FIO_CopyFile("B:/src.bin", "B:/bad2.bin") != 0);
    BALANCED();
    CHECK(find("B:/bad2.bin") < 0);
    shim_fio_malloc_fail = 0;

    CHECK(FIO_CopyFile("B:/missing", "B:/x.bin") != 0);
    BALANCED();
    CHECK(open_handles() == 0);

    FILE * f = FIO_CreateFileOrAppend("B:/src.bin");    /* exists: open + seek to end */
    BALANCED();
    CHECK(f != NULL);
    FIO_CloseFile(f);
    f = FIO_CreateFileOrAppend("B:/new.log");           /* missing: create */
    BALANCED();
    CHECK(f != NULL);
    FIO_CloseFile(f);
    BALANCED();

    save_mem_to_file((void *) "abc", 3, "B:/mem.bin");
    BALANCED();
    CHECK(nodes[find("B:/mem.bin")].size == 3);
    dump_big_seg(1, "B:/seg.bin");
    BALANCED();
    CHECK(open_handles() == 0);
}

static void test_null_args(void)
{
    reset();
    uint32_t size;
    CHECK(FIO_OpenFile(NULL, 0) == NULL);
    CHECK(FIO_GetFileSize(NULL, &size) == -1);
    CHECK(FIO_RemoveFile(NULL) == -1);
    CHECK(FIO_FindFirstEx(NULL, NULL) == NULL);
    CHECK(FIO_CreateDirectory(NULL) == -1);
    CHECK(FIO_RenameFile(NULL, "B:/x") == -1);
    CHECK(FIO_CreateFile(NULL) == NULL);
    CHECK(FIO_CopyFile(NULL, "B:/x") == -1);
    CHECK(FIO_CreateFileOrAppend(NULL) == NULL);
    BALANCED();
    CHECK(stub_calls == 0);
}

static void test_hold_not_allowed(void)
{
    /* interrupt / IRQs masked / before init: the I/O still runs, nothing is released */
    reset();
    hold_allowed = 0;
    add("B:/a.bin", 0, 10);
    FILE * f = FIO_CreateFile("B:/b.bin");
    CHECK(f != NULL);
    CHECK(FIO_WriteFile(f, "x", 1) == 1);
    FIO_CloseFile(f);
    CHECK(FIO_CopyFile("B:/a.bin", "B:/c.bin") == 0);
    CHECK(stub_calls > 0);
    CHECK(takes == 0 && releases == 0 && holds == 0);
}

int main(void)
{
    test_create_write_close();
    test_open_read();
    test_metadata();
    test_copy_move_append();
    test_null_args();
    test_hold_not_allowed();

    CHECK(unheld_calls == 0);
    printf("test_fio_hold: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
