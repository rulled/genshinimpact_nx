/*
 * Optional per-install device identity.
 *
 * Loaded from sdmc:/config/genshinimpact_nx/device_profile.ini (simple
 * key=value lines, '#' comments).  Harvest the values from a real Android
 * device that already runs the supported client (see
 * tools/harvest_device_profile.ps1) so every identity surface - system
 * properties, JNI Build.*, Passport/Combo HTTP headers, ANDROID_ID -
 * reports one consistent device instead of the default synthetic
 * Nintendo/Switch profile the risk control rejects at game entry.
 *
 * Absent keys keep the hardware-proven defaults; the file is optional.
 */

#ifndef DEVICE_PROFILE_H
#define DEVICE_PROFILE_H

void device_profile_init(void);

/* Returns the configured value for a key, or NULL when unset/invalid. */
const char *device_profile_get(const char *key);

/* Returns the configured value for a key, or fallback when unset. */
const char *device_profile_or(const char *key, const char *fallback);

#endif
