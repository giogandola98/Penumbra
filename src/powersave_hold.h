/* Bookkeeping for ML's holds on Canon's CPU power save (see powersave.c, docs/analysis/powersave-hold.md).
 *
 * Pure C, no DryOS: the caller serialises access (powersave.c uses a semaphore) and turns the
 * returned edge into Canon's DisablePowerSave / EnablePowerSave. Header-only (one includer per
 * program) so the host unit test (tests/host/test_powersave_hold.c) runs the exact code the
 * camera runs; not `inline`, so -Os keeps one copy of each instead of inlining into every op.
 *
 * Holders are identified by name (compared by content, since the same literal in a module and in
 * the core has different addresses). A name may be held several times (nested); a release only
 * drops a hold that name owns, so one buggy holder cannot drop another's hold and no count ever
 * goes negative. Only the 0 -> 1 and 1 -> 0 transitions of the total reach Canon, so ML adds at
 * most one to Canon's counter (which asserts on underflow and on more than 32 holds).
 */
#ifndef _powersave_hold_h_
#define _powersave_hold_h_

#define PS_HOLD_SLOTS       8       /* distinct holders tracked by name */
#define PS_HOLD_MAX_NEST    255     /* per holder; more means a leak */

#define PS_HOLD_ERR         (-1)    /* refused: nothing changed */
#define PS_HOLD_NO_EDGE     0       /* counted; Canon state unchanged */
#define PS_HOLD_FIRST       1       /* total went 0 -> 1: call DisablePowerSave */
#define PS_HOLD_LAST        2       /* total went 1 -> 0: call EnablePowerSave */

struct ps_hold_slot
{
    const char * who;               /* NULL = free */
    int n;
};

struct ps_hold
{
    struct ps_hold_slot slot[PS_HOLD_SLOTS];
    int overflow;                   /* holds whose name did not fit in slot[] */
    int total;                      /* sum of all of the above */
};

static int ps_hold_streq(const char * a, const char * b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct ps_hold_slot * ps_hold_find(struct ps_hold * s, const char * who)
{
    for (int i = 0; i < PS_HOLD_SLOTS; i++)
        if (s->slot[i].who && ps_hold_streq(s->slot[i].who, who))
            return &s->slot[i];
    return 0;
}

static int ps_hold_take(struct ps_hold * s, const char * who)
{
    if (!who) return PS_HOLD_ERR;

    struct ps_hold_slot * h = ps_hold_find(s, who);
    if (!h)
    {
        for (int i = 0; i < PS_HOLD_SLOTS && !h; i++)
            if (!s->slot[i].who)
                h = &s->slot[i];
        if (h)
        {
            h->who = who;
            h->n = 0;
        }
    }

    if (h)
    {
        if (h->n >= PS_HOLD_MAX_NEST) return PS_HOLD_ERR;
        h->n++;
    }
    else
    {
        /* table full: still keep the camera awake, just without a name */
        if (s->overflow >= PS_HOLD_MAX_NEST) return PS_HOLD_ERR;
        s->overflow++;
    }

    return ++s->total == 1 ? PS_HOLD_FIRST : PS_HOLD_NO_EDGE;
}

static int ps_hold_drop(struct ps_hold * s, const char * who)
{
    if (!who) return PS_HOLD_ERR;

    struct ps_hold_slot * h = ps_hold_find(s, who);
    if (h)
    {
        if (--h->n == 0)
            h->who = 0;
    }
    else if (s->overflow > 0)
    {
        /* a name that never got a slot (table was full when it took its hold) */
        s->overflow--;
    }
    else
    {
        /* release without a matching hold */
        return PS_HOLD_ERR;
    }

    return --s->total == 0 ? PS_HOLD_LAST : PS_HOLD_NO_EDGE;
}

static int ps_hold_count(const struct ps_hold * s, const char * who)
{
    for (int i = 0; i < PS_HOLD_SLOTS; i++)
        if (s->slot[i].who && ps_hold_streq(s->slot[i].who, who))
            return s->slot[i].n;
    return 0;
}

/* Level-triggered form for features whose state is a flag: make `who` hold exactly once (on)
 * or not at all (off). Only meant for names used this way and nowhere else. */
static int ps_hold_set(struct ps_hold * s, const char * who, int on)
{
    int n = ps_hold_count(s, who);
    if (on && n == 0) return ps_hold_take(s, who);
    if (!on && n > 0) return ps_hold_drop(s, who);
    return PS_HOLD_NO_EDGE;
}

#endif
