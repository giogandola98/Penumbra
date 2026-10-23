#ifndef _property_whitelist_h_
#define _property_whitelist_h_

#include "property.h"

/* 4000D: prop_request_change() is enabled one property at a time (CONFIG_PROP_WRITE_WHITELIST in the
 * photo profile, see src/property.c). Writing a wrong value to a Canon property can leave the camera in a
 * bad state, so only the exposure and shutter properties needed by the photo features are allowed; every
 * other write is dropped silently, exactly as with CONFIG_PROP_REQUEST_CHANGE off.
 *
 * Evidence: _prop_request_change (0xFE104D60) is PropertyMgr's request-change entry (allocates
 * len + 8, copies the data, posts message 6 to PropMgr; "pRequestChange" assert in PropertyMgr.c),
 * same signature as on the 1100D. The IDs below are the DIGIC 4 ones used by every ML DIGIC 4 port;
 * 0x80000003/5/6/7/8 and 0x8000002C appear as literals in this firmware's property code.
 * prop_request_change() also refuses a write whose length differs from Canon's own (prop_get_prop_len). */

/* allow writes / allow prop_request_change() for these: */
const uint32_t prop_write_allow[] =
{
    PROP_SHUTTER,           /* HDR bracketing, bulb (BULB is the shutter value past 30" in M) */
    PROP_SHUTTER_AUTO,      /* restored together with PROP_SHUTTER by the bulb code */
    PROP_APERTURE,          /* HDR bracketing (aperture is restored after the sequence) */
    PROP_ISO,               /* HDR bracketing (ISO steps) */
    PROP_AE,                /* HDR bracketing in P/Av/Tv (exposure compensation) */
    PROP_DRIVE,             /* single shot during bulb / bracketing, restored after */
    PROP_REMOTE_SW1,        /* half-shutter press for bulb exposures */
    PROP_REMOTE_SW2,        /* full-shutter press for bulb exposures */
};

#endif
