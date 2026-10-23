/* Host shim: just enough of ML's headers to compile src/fio-ml.c unchanged for test_fio_hold.c.
 * The Canon stubs (_FIO_*) and the hold API are provided by the test. */
#ifndef _fio_shim_dryos_h_
#define _fio_shim_dryos_h_

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_DIGIC_45
#define CONFIG_FIO_RENAMEFILE_WORKS

#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif

#define PTR_INVALID         ((void *)0xFFFFFFFF)
#define IS_ERROR(ptr)       (1 & (uintptr_t)(ptr))

/* the test decides which buffers count as cacheable */
extern int shim_cacheable;
#define CACHEABLE(x)        (shim_cacheable ? (void *)(x) : (void *)0)
#define UNCACHEABLE(x)      ((void *)(x))
static inline void sync_caches(void) {}

void shim_assert(const char * msg, int line);
#define ASSERT(x)           { if (!(x)) shim_assert(#x, __LINE__); }

#define DM_MAGIC            0
#define DebugMsg(...)       do {} while (0)
#define DEBUG(...)          do {} while (0)
#define DryosDebugMsg(...)  do {} while (0)

#define FONT_LARGE          0
#define FONT_CANON          0
#define COLOR_BLACK         0
#define bmp_printf(...)     do {} while (0)
#define bmp_fill(...)       do {} while (0)
#define _load_fonts()       do {} while (0)
#define DISPLAY_IS_ON       1
#define SetGUIRequestMode(x) do {} while (0)
#define msleep(x)           do {} while (0)
#define redraw_after(x)     do {} while (0)
#define beep()              do {} while (0)
#define info_led_blink(...) do {} while (0)

#define streq(a, b)         (strcmp(a, b) == 0)
static inline const char * get_current_task_name(void) { return "run_test"; }
extern int shim_fio_malloc_fail;
static inline void * fio_malloc(size_t n) { return shim_fio_malloc_fail ? NULL : malloc(n); }
static inline void fio_free(void * p) { free(p); }

#define CONFIG_INT(name, var, value)    int __attribute__((unused)) var = value
#define SHIM_CAT2(a, b)     a##b
#define SHIM_CAT(a, b)      SHIM_CAT2(a, b)
#define PROP_HANDLER(id)    static void __attribute__((unused)) SHIM_CAT(prop_handler_, __LINE__)(uint32_t * buf)
#define INIT_FUNC(name, f)  void * SHIM_CAT(init_func_, __LINE__) = (void *)(f)

#endif
