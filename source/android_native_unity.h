/* Native-window, looper, sensor and HID bridge declarations. */
#ifndef ANDROID_NATIVE_UNITY_H
#define ANDROID_NATIVE_UNITY_H

#include <stddef.h>
#include <stdint.h>

typedef struct ANativeWindow ANativeWindow;
typedef struct ALooper       ALooper;

void  android_native_input_init(void);
/* Synchronize the physical display mode and return nonzero after a live
 * dock/undock transition (the first initialization is not a transition). */
int   android_native_update_mode(void);
/* Apply a pending native-buffer geometry only while no NWindow slots are
 * registered.  Returns zero or a negative errno-style Android status. */
int   android_native_apply_window_geometry(void);
void  android_native_feed_hid(uint8_t (*inject)(void*,void*,void*,int),
                              void *env, void *thiz);
void  android_native_vibration_standard(int length_ms);
void  android_native_vibration_haptic(int style);
void  android_native_vibration_update(void);
void  android_native_vibration_shutdown(void);

void     ANativeWindow_acquire(ANativeWindow *);
void     ANativeWindow_release(ANativeWindow *);
ANativeWindow *ANativeWindow_fromSurface(void *, void *);
int32_t  ANativeWindow_getWidth(ANativeWindow *);
int32_t  ANativeWindow_getHeight(ANativeWindow *);
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *, int32_t, int32_t, int32_t);

ALooper *ALooper_prepare(int);
ALooper *ALooper_forThread(void);
void     ALooper_acquire(ALooper *);
void     ALooper_release(ALooper *);
void     ALooper_wake(ALooper *);
int      ALooper_pollOnce(int, int *, int *, void **);
int      ALooper_addFd(ALooper *, int fd, int ident, int events,
                       int (*callback)(int, int, void *), void *data);
int      ALooper_removeFd(ALooper *, int fd);
/* Guest close/dup replacement hook: retire registrations tied to the old
 * open-file description before its integer descriptor can be reused. */
void     android_native_looper_forget_fd(int fd);
/* Drive only callback-only loopers on the host thread which substitutes for
 * Android's Java MessageQueue.  Returns the number of bounded callback batches. */
int      android_native_looper_pump_callbacks(unsigned max_batches);

void *ASensorManager_getInstance(void);
int   ASensorManager_getSensorList(void *, void **);
void *ASensorManager_getDefaultSensor(void *, int);
void *ASensorManager_createEventQueue(void *, void *, int, void *, void *);
int   ASensorManager_destroyEventQueue(void *, void *);
int   ASensorEventQueue_enableSensor(void *, const void *);
int   ASensorEventQueue_disableSensor(void *, const void *);
int   ASensorEventQueue_setEventRate(void *, const void *, int32_t);
int   ASensorEventQueue_getEvents(void *, void *, size_t);
int   ASensorEventQueue_hasEvents(void *);
const char *ASensor_getName(const void *);
const char *ASensor_getVendor(const void *);
int         ASensor_getType(const void *);
float       ASensor_getResolution(const void *);
int         ASensor_getMinDelay(const void *);

void android_get_orientation(float *x, float *y, float *z);

int  fakefd_is_fake(int fd);
int  fakefd_is_live(int fd);
int  fakefd_pipe(int fds[2]);
int  fakefd_pipe2(int fds[2], int flags);
int  fakefd_eventfd(unsigned initial_value, int flags);
int  fakefd_random(int flags);
int  fakefd_dup(int fd);
int  fakefd_dup2(int fd, int target);
long fakefd_read(int fd, void *buf, unsigned long n);
long fakefd_write(int fd, const void *buf, unsigned long n);
int  fakefd_close(int fd);
int  fakefd_fcntl(int fd, int command, intptr_t argument);
int  fakefd_ioctl(int fd, unsigned long request, void *argument);
short fakefd_poll_revents(int fd, short events);
void fakefd_wait(unsigned long long timeout_ns);
int  fakefd_select_bit(int fd);
int  fakefd_from_select_bit(int bit);


#endif
