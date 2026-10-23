/* Host tests for the powersave hold bookkeeping (src/powersave_hold.h).
 *
 * The camera side (powersave.c) calls DisablePowerSave on PS_HOLD_FIRST and EnablePowerSave on
 * PS_HOLD_LAST. A fake Canon counter here applies the same rule and checks Canon's own
 * invariants (never below 0, never above 32) plus ML's: at most one Canon hold from ML, and
 * Canon held exactly when ML's total is non-zero. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "powersave_hold.h"

static int failures;
static int canon;           /* Canon's counter, as far as ML's calls go */
static int canon_max;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* what powersave.c does with the edge */
static int apply(int edge)
{
    if (edge == PS_HOLD_FIRST) canon++;
    if (edge == PS_HOLD_LAST) canon--;
    if (canon > canon_max) canon_max = canon;
    return edge;
}

static int take(struct ps_hold * s, const char * who) { return apply(ps_hold_take(s, who)); }
static int drop(struct ps_hold * s, const char * who) { return apply(ps_hold_drop(s, who)); }
static int set(struct ps_hold * s, const char * who, int on) { return apply(ps_hold_set(s, who, on)); }

static void invariants(const struct ps_hold * s)
{
    int sum = s->overflow;
    for (int i = 0; i < PS_HOLD_SLOTS; i++)
    {
        CHECK(s->slot[i].n >= 0);
        CHECK(!s->slot[i].who == !s->slot[i].n);    /* free slot <=> zero count */
        sum += s->slot[i].n;
    }
    CHECK(s->overflow >= 0);
    CHECK(s->total == sum);
    CHECK(canon == (s->total > 0));
}

static void test_nesting(void)
{
    struct ps_hold s = {0};
    canon = 0;

    CHECK(take(&s, "boot") == PS_HOLD_FIRST);
    CHECK(take(&s, "menu") == PS_HOLD_NO_EDGE);
    CHECK(take(&s, "boot") == PS_HOLD_NO_EDGE);
    CHECK(ps_hold_count(&s, "boot") == 2);
    CHECK(s.total == 3);
    invariants(&s);

    CHECK(drop(&s, "boot") == PS_HOLD_NO_EDGE);
    CHECK(drop(&s, "boot") == PS_HOLD_NO_EDGE);
    CHECK(ps_hold_count(&s, "boot") == 0);
    CHECK(drop(&s, "menu") == PS_HOLD_LAST);
    CHECK(s.total == 0);
    invariants(&s);
}

static void test_never_negative(void)
{
    struct ps_hold s = {0};
    canon = 0;

    /* release with nothing held, or by a name that holds nothing */
    CHECK(drop(&s, "intervalometer") == PS_HOLD_ERR);
    CHECK(s.total == 0 && canon == 0);
    CHECK(take(&s, "lua task") == PS_HOLD_FIRST);
    CHECK(drop(&s, "bulb") == PS_HOLD_ERR);           /* cannot drop another holder's hold */
    CHECK(ps_hold_count(&s, "lua task") == 1);
    CHECK(drop(&s, "lua task") == PS_HOLD_LAST);
    CHECK(drop(&s, "lua task") == PS_HOLD_ERR);       /* double release */
    CHECK(drop(&s, 0) == PS_HOLD_ERR);
    CHECK(take(&s, 0) == PS_HOLD_ERR);
    invariants(&s);
}

static void test_names_by_content(void)
{
    struct ps_hold s = {0};
    canon = 0;
    /* a module's literal and the core's literal have different addresses */
    char a[] = "lua script", b[] = "lua script";
    CHECK(take(&s, a) == PS_HOLD_FIRST);
    CHECK(ps_hold_count(&s, b) == 1);
    CHECK(drop(&s, b) == PS_HOLD_LAST);
    invariants(&s);
}

static void test_level_set(void)
{
    struct ps_hold s = {0};
    canon = 0;
    for (int i = 0; i < 5; i++)
        set(&s, "intervalometer", 1);                /* polled every loop: holds once */
    CHECK(ps_hold_count(&s, "intervalometer") == 1);
    CHECK(set(&s, "menu", 1) == PS_HOLD_NO_EDGE);
    for (int i = 0; i < 5; i++)
        set(&s, "intervalometer", 0);
    CHECK(ps_hold_count(&s, "intervalometer") == 0);
    CHECK(set(&s, "menu", 0) == PS_HOLD_LAST);
    CHECK(set(&s, "menu", 0) == PS_HOLD_NO_EDGE);    /* off when already off is not an error */
    invariants(&s);
}

static void test_table_full(void)
{
    struct ps_hold s = {0};
    canon = 0;
    char names[PS_HOLD_SLOTS + 3][8];
    for (int i = 0; i < PS_HOLD_SLOTS + 3; i++)
    {
        snprintf(names[i], sizeof(names[i]), "h%d", i);
        CHECK(take(&s, names[i]) == (i == 0 ? PS_HOLD_FIRST : PS_HOLD_NO_EDGE));
    }
    CHECK(s.overflow == 3);
    invariants(&s);

    /* the unnamed holds still keep Canon held until they are all released */
    for (int i = 0; i < PS_HOLD_SLOTS; i++)
        CHECK(drop(&s, names[i]) == PS_HOLD_NO_EDGE);
    CHECK(canon == 1);
    for (int i = PS_HOLD_SLOTS; i < PS_HOLD_SLOTS + 3; i++)
        CHECK(drop(&s, names[i]) == (i == PS_HOLD_SLOTS + 2 ? PS_HOLD_LAST : PS_HOLD_NO_EDGE));
    CHECK(drop(&s, "h0") == PS_HOLD_ERR);
    invariants(&s);

    /* freed slots are reused */
    CHECK(take(&s, "again") == PS_HOLD_FIRST);
    CHECK(s.overflow == 0 && ps_hold_count(&s, "again") == 1);
    CHECK(drop(&s, "again") == PS_HOLD_LAST);
    invariants(&s);
}

static void test_nest_limit(void)
{
    struct ps_hold s = {0};
    canon = 0;
    for (int i = 0; i < PS_HOLD_MAX_NEST; i++)
        take(&s, "leak");
    CHECK(take(&s, "leak") == PS_HOLD_ERR);
    CHECK(ps_hold_count(&s, "leak") == PS_HOLD_MAX_NEST);
    for (int i = 0; i < PS_HOLD_MAX_NEST; i++)
        drop(&s, "leak");
    CHECK(s.total == 0);
    invariants(&s);
}

/* random sequences of the three operations against a reference model
 * (as many names as slots: the unnamed overflow is covered by test_table_full) */
static void test_random(void)
{
    static const char * names[PS_HOLD_SLOTS] = { "boot", "menu", "intervalometer", "bulb",
        "hdr shot", "lua script", "lua task", "lua events" };
    enum { N = sizeof(names) / sizeof(names[0]) };
    struct ps_hold s = {0};
    int model[N] = {0};
    canon = 0;
    canon_max = 0;
    srand(12345);

    for (int step = 0; step < 200000; step++)
    {
        int k = rand() % N;
        int op = rand() % 3;
        int r;
        int before = s.total;
        if (op == 0)
        {
            r = take(&s, names[k]);
            if (r != PS_HOLD_ERR) model[k]++;
        }
        else if (op == 1)
        {
            r = drop(&s, names[k]);
            if (model[k] == 0)
                CHECK(r == PS_HOLD_ERR);
            else
                model[k]--;
        }
        else
        {
            int on = rand() & 1;
            int had = ps_hold_count(&s, names[k]);
            r = set(&s, names[k], on);
            if (on && !had) model[k]++;
            if (!on && had) model[k]--;
        }

        if (r == PS_HOLD_FIRST) CHECK(before == 0 && s.total == 1);
        if (r == PS_HOLD_LAST)  CHECK(before == 1 && s.total == 0);

        int sum = 0;
        for (int i = 0; i < N; i++) sum += model[i];
        CHECK(sum == s.total);
        invariants(&s);
        if (failures > 20) return;
    }
    CHECK(canon_max <= 1);      /* ML never stacks holds in Canon's counter (max 32) */
}

int main(void)
{
    test_nesting();
    test_never_negative();
    test_names_by_content();
    test_level_set();
    test_table_full();
    test_nest_limit();
    test_random();

    if (failures)
    {
        printf("powersave_hold: %d failure(s)\n", failures);
        return 1;
    }
    printf("powersave_hold: all tests passed\n");
    return 0;
}
