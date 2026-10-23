/* Host shim: the part of src/powersave.h that fio-ml.c uses; implemented by test_fio_hold.c. */
#ifndef _fio_shim_powersave_h_
#define _fio_shim_powersave_h_

int powersave_hold_try(const char * who);
int powersave_release(const char * who);

#endif
