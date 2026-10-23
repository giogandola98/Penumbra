#ifndef _powersave_h_
#define _powersave_h_

/* reset the powersave timer (as if you would press a button) */
void powersave_prolong();

/* disable powersave timer */
void powersave_prohibit();

/* re-enable powersave timer */
void powersave_permit();

/* Counted holds on Canon's CPU power save (DisablePowerSave / EnablePowerSave).
 * Take one while a feature needs msleep / timers to keep running with nobody touching the camera
 * (display off, idle): Canon's deep idle sleep stops the timers. Holds nest per name (`who`, a
 * string that must stay valid while held); release only drops a hold that `who` owns.
 * Task context only (uses a semaphore); not from interrupts or timer callbacks.
 * Only CONFIG_POWERSAVE_HOLD builds forward the holds to Canon; others just count them.
 * Exported to modules. See docs/analysis/powersave-hold.md. */
int powersave_hold(const char * who);       /* 0 on success, -1 if refused */
int powersave_release(const char * who);    /* 0 on success, -1 if `who` held nothing */

/* For hot paths that may also run before ML init, from an interrupt or with IRQs masked (the FIO
 * wrappers): takes the hold only where powersave_hold() is allowed. Returns 1 if it took one; the
 * caller releases only then (`if (held) powersave_release(who);`). */
int powersave_hold_try(const char * who);

/* level-triggered: `who` holds exactly once while `on`, none otherwise; cheap to poll */
void powersave_hold_set(const char * who, int on);

int powersave_holders();                    /* total ML holds (debug, tests) */
int powersave_hold_count(const char * who);

/* called once from ML init, before the first hold */
void powersave_hold_init();

/* stop LiveView activity, leave shutter open */
/* the sensor is turned off, and the LV image will freeze */
/* method similar to what Canon code does when entering PLAY mode from LV */
void PauseLiveView();

/* back to LiveView from paused state */
int ResumeLiveView();

/* turn display on/off */
void display_on();
void display_off();

/* ML powersave in LV */
int idle_is_powersave_enabled();
int idle_is_powersave_active();
int idle_is_powersave_enabled_on_info_disp_key();

/* internal hooks (to be refactored as CBRs) */
void idle_led_blink_step(int k);
void idle_powersave_step();
int handle_powersave_key(struct event * event);
void idle_kill_flicker();
#endif
