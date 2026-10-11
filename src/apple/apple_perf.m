/*
 * apple_perf.m -- a [perf] line every 10 s on Apple hosts (RECOMP_FPS_LOG=1):
 * the process's CPU time, the busiest threads and the thermal state.
 *
 *   [perf] CPU 182% of a core: game 96%, 0x1a2b3 41%, SDLAudioP1 6%, ...; thermal nominal
 *
 *   [perf] APU 1500 frames/s (1500 = real time), frame thread 12% busy
 *   [perf] APU pacing: ...   (apu_core.c, as in the Switch report)
 *   [perf] audio out: 0 queue underruns, 0 dropped, 8 of 8 blocks queued
 *   [perf] APU ring voices: 12 of 9000 samples stale (0.13%)
 *
 * Renderers are compared at the same frame rate by this (the title caps
 * races at 30 fps, so fps alone does not tell them apart), and sound that
 * breaks up is told apart: the APU behind real time, the game's mixer late
 * (stale ring samples), or the output queue running dry. Thread times come
 * from Mach; unnamed threads show their thread ID.
 */
#import <Foundation/Foundation.h>

#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apu_xaudio2.h"

int   mcpx_apu_frames_per_second(void);                     /* apu_core.c */
float mcpx_apu_utilization(void);
int   mcpx_apu_pacing_stats(char *buf, int cap);
void  mcpx_apu_ring_stats(uint32_t *stale, uint32_t *total); /* apu_vp.c */

#define PERF_THREADS 256
#define PERF_TOP     6

typedef struct {
    uint64_t id;
    uint64_t us;               /* user + system CPU time at the last sample */
} PerfSeen;

static PerfSeen s_seen[PERF_THREADS];
static uint32_t s_nseen;

static uint64_t seen_swap(uint64_t id, uint64_t us)
{
    uint32_t i;
    for (i = 0; i < s_nseen; i++)
        if (s_seen[i].id == id) {
            uint64_t prev = s_seen[i].us;
            s_seen[i].us = us;
            return prev;
        }
    if (s_nseen < PERF_THREADS) {
        s_seen[s_nseen].id = id;
        s_seen[s_nseen].us = us;
        s_nseen++;
    }
    return 0;                   /* new thread: all its time is this window's */
}

static const char *thermal_name(void)
{
    switch ([NSProcessInfo processInfo].thermalState) {
    case NSProcessInfoThermalStateNominal:  return "nominal";
    case NSProcessInfoThermalStateFair:     return "fair";
    case NSProcessInfoThermalStateSerious:  return "serious";
    case NSProcessInfoThermalStateCritical: return "critical";
    default:                                return "unknown";
    }
}

typedef struct {
    char     name[64];
    uint64_t us;
} PerfTop;

static void audio_report(void)
{
    static uint64_t last_under, last_drop;
    char pb[256];
    Xa2Stats st;
    uint32_t stale, total;

    fprintf(stderr, "[perf] APU %d frames/s (1500 = real time), frame thread %.0f%% busy\n",
            mcpx_apu_frames_per_second(), mcpx_apu_utilization() * 100.0f);
    mcpx_apu_pacing_stats(pb, sizeof pb);
    fprintf(stderr, "[perf] APU pacing: %s\n", pb);
    if (xa2_is_active()) {
        xa2_get_stats(&st);
        fprintf(stderr, "[perf] audio out: %llu queue underruns, %llu dropped, %d of %d blocks"
                        " queued, peak %d\n", (unsigned long long)(st.underruns - last_under),
                (unsigned long long)(st.dropped - last_drop), xa2_queued(), xa2_queue_target(),
                st.peak);
        last_under = st.underruns;
        last_drop = st.dropped;
    }
    mcpx_apu_ring_stats(&stale, &total);
    fprintf(stderr, "[perf] APU ring voices: %u of %u samples stale (%u.%02u%%)\n",
            stale, total, total ? (unsigned)(100ull * stale / total) : 0,
            total ? (unsigned)(10000ull * stale / total % 100) : 0);
}

static void sample(double secs, int print)
{
    thread_act_array_t th;
    mach_msg_type_number_t n = 0, i;
    PerfTop top[PERF_TOP];
    uint64_t total = 0;
    uint32_t ntop = 0, k;
    char line[512];
    size_t at;

    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS)
        return;
    for (i = 0; i < n; i++) {
        thread_basic_info_data_t bi;
        thread_identifier_info_data_t ii;
        thread_extended_info_data_t ei;
        mach_msg_type_number_t c;
        uint64_t us, d;

        c = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS)
            goto next;
        c = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &c) != KERN_SUCCESS)
            goto next;
        us = (uint64_t)bi.user_time.seconds * 1000000u + (uint64_t)bi.user_time.microseconds
           + (uint64_t)bi.system_time.seconds * 1000000u + (uint64_t)bi.system_time.microseconds;
        d = us - seen_swap(ii.thread_id, us);
        total += d;
        if (ntop < PERF_TOP || d > top[ntop - 1].us) {
            PerfTop t;
            t.us = d;
            c = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ei, &c) == KERN_SUCCESS
                && ei.pth_name[0])
                snprintf(t.name, sizeof t.name, "%s", ei.pth_name);
            else
                snprintf(t.name, sizeof t.name, "0x%llx", (unsigned long long)ii.thread_id);
            if (ntop < PERF_TOP)
                ntop++;
            for (k = ntop - 1; k > 0 && top[k - 1].us < t.us; k--)
                top[k] = top[k - 1];
            top[k] = t;
        }
next:
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
    if (!print)
        return;
    at = (size_t)snprintf(line, sizeof line, "[perf] CPU %.0f%% of a core:", total / (secs * 1e4));
    for (k = 0; k < ntop && at < sizeof line; k++)
        at += (size_t)snprintf(line + at, sizeof line - at, "%s %s %.0f%%", k ? "," : "",
                               top[k].name, top[k].us / (secs * 1e4));
    if (at < sizeof line)
        snprintf(line + at, sizeof line - at, "; thermal %s, %u threads", thermal_name(), n);
    fprintf(stderr, "%s\n", line);
    audio_report();
}

static void *perf_thread(void *arg)
{
    struct timespec last, now;
    (void)arg;
    pthread_setname_np("perf");
    clock_gettime(CLOCK_MONOTONIC, &last);
    sample(10.0, 0);                            /* baseline */
    for (;;) {
        double dt;
        struct timespec ts = { 10, 0 };
        nanosleep(&ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
        last = now;
        @autoreleasepool {
            sample(dt, 1);
        }
    }
    return NULL;
}

void apple_perf_start(void)
{
    const char *e = getenv("RECOMP_FPS_LOG");
    pthread_t t;
    if (!(e && *e == '1'))
        return;
    if (pthread_create(&t, NULL, perf_thread, NULL) == 0)
        pthread_detach(t);
}
