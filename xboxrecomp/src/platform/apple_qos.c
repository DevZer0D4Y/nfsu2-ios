/*
 * apple_qos.c -- Darwin QoS classes for the runtime's and the title's threads.
 *
 * A pthread on Darwin starts in the default QoS class (scheduling priority
 * 31) whatever its creator runs at, and iOS readily moves default-class work
 * onto the efficiency cores. Measured on macOS: the title's main thread, the
 * pushbuffer executor and every other worker ran at p31, only system threads
 * higher. Their work has deadlines -- frames, the EA mixer's refills, its
 * stream readers -- so they move up:
 *
 *   Win32 priority                    QoS class            relative priority
 *   above normal .. time critical     user-interactive     0   (the APU frame
 *                                                               thread, the EA mixer)
 *   normal                            user-interactive    -1   (game threads)
 *   below normal, lowest              user-initiated       0
 *   idle                              utility              0
 *
 * so audio stays above the game threads, as CLAUDE.md asks. Only a thread's
 * own QoS can be set: win32_compat.c calls this on the thread, when it starts
 * (with the priority it was created with or given while suspended) and when
 * it changes its own priority; a change made from another thread waits for
 * neither (rare: the title sets priorities before resuming a thread).
 * RECOMP_QOS=0 leaves every thread in the default class.
 */
#if defined(__APPLE__)

#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>

#include "win32_compat.h"

void xbox_apple_thread_qos(int priority)
{
    static int on = -1;
    qos_class_t cls;
    int rel = 0;

    if (on < 0) {
        const char *e = getenv("RECOMP_QOS");
        on = !(e && *e == '0');
    }
    if (!on)
        return;
    if (priority >= THREAD_PRIORITY_ABOVE_NORMAL) {
        cls = QOS_CLASS_USER_INTERACTIVE;
    } else if (priority == THREAD_PRIORITY_NORMAL) {
        cls = QOS_CLASS_USER_INTERACTIVE;
        rel = -1;
    } else if (priority > THREAD_PRIORITY_IDLE) {
        cls = QOS_CLASS_USER_INITIATED;
    } else {
        cls = QOS_CLASS_UTILITY;
    }
    pthread_set_qos_class_self_np(cls, rel);
}

#endif /* __APPLE__ */
