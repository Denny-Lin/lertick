// SPDX-License-Identifier: GPL-2.0-only
/*
 * gamesim: the CPU side of a game under background load.
 *
 * A main thread wakes at every frame boundary, computes for main_ms of CPU
 * time and wakes a render thread, which computes for render_ms; the frame is
 * done when render finishes. An audio thread wakes every audio_period_ms and
 * computes for audio_ms. Work is measured in the thread's own CPU time, so a
 * preempted thread still does all of it.
 *
 * Reports frames that finished after their frame period (missed), frame time
 * percentiles, audio deadline misses, and the wakeup delay of the main and
 * audio threads.
 *
 * Usage: gamesim FPS MAIN_MS RENDER_MS AUDIO_PERIOD_MS AUDIO_MS SECONDS
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN 200000

static double fps, main_ms, render_ms, audio_period_ms, audio_ms, seconds;
static uint64_t start_ns, end_ns;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static uint64_t	       frame_seq, render_seq;
static uint64_t	       frame_start[MAXN];
static double	       frame_ms[MAXN], main_wake_us[MAXN], audio_wake_us[MAXN];
static int	       nframes, nmain, naudio, audio_missed;
static volatile int    stop;

static uint64_t now_ns(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Spin until this thread has used @ms of CPU time. */
static void compute(double ms)
{
	uint64_t t0 = now_ns(CLOCK_THREAD_CPUTIME_ID);
	uint64_t want = (uint64_t)(ms * 1e6);
	volatile uint64_t x = 0;

	while (now_ns(CLOCK_THREAD_CPUTIME_ID) - t0 < want)
		for (int i = 0; i < 1000; i++)
			x += i;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / 1000000000ull,
			       .tv_nsec = t % 1000000000ull };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL))
		;
}

static void *main_thread(void *arg)
{
	uint64_t period = (uint64_t)(1e9 / fps), t = start_ns;

	(void)arg;
	while (!stop) {
		t += period;
		if (t >= end_ns)
			break;
		sleep_until(t);
		if (nmain < MAXN)
			main_wake_us[nmain++] = (now_ns(CLOCK_MONOTONIC) - t) / 1e3;
		compute(main_ms);
		pthread_mutex_lock(&mu);
		if (frame_seq < MAXN)
			frame_start[frame_seq] = t;
		frame_seq++;
		pthread_cond_signal(&cv);
		pthread_mutex_unlock(&mu);
	}
	pthread_mutex_lock(&mu);
	stop = 1;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	return NULL;
}

static void *render_thread(void *arg)
{
	(void)arg;
	for (;;) {
		uint64_t seq;

		pthread_mutex_lock(&mu);
		while (render_seq == frame_seq && !stop)
			pthread_cond_wait(&cv, &mu);
		if (render_seq == frame_seq && stop) {
			pthread_mutex_unlock(&mu);
			break;
		}
		seq = render_seq++;
		pthread_mutex_unlock(&mu);
		compute(render_ms);
		if (seq < MAXN && nframes < MAXN)
			frame_ms[nframes++] =
				(now_ns(CLOCK_MONOTONIC) - frame_start[seq]) / 1e6;
	}
	return NULL;
}

static void *audio_thread(void *arg)
{
	uint64_t period = (uint64_t)(audio_period_ms * 1e6), t = start_ns;

	(void)arg;
	while (!stop) {
		t += period;
		if (t >= end_ns)
			break;
		sleep_until(t);
		if (naudio < MAXN)
			audio_wake_us[naudio++] = (now_ns(CLOCK_MONOTONIC) - t) / 1e3;
		compute(audio_ms);
		if (now_ns(CLOCK_MONOTONIC) - t > period)
			audio_missed++;
	}
	return NULL;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static double pct(double *v, int n, double p)
{
	if (!n)
		return 0;
	qsort(v, n, sizeof(*v), cmp);
	return v[(int)(p * (n - 1))];
}

int main(int argc, char **argv)
{
	pthread_t m, r, a;
	double	  period_ms;
	int	  missed = 0;

	if (argc != 7) {
		fprintf(stderr, "usage: %s FPS MAIN_MS RENDER_MS AUDIO_PERIOD_MS AUDIO_MS SECONDS\n", argv[0]);
		return 1;
	}
	fps = atof(argv[1]);
	main_ms = atof(argv[2]);
	render_ms = atof(argv[3]);
	audio_period_ms = atof(argv[4]);
	audio_ms = atof(argv[5]);
	seconds = atof(argv[6]);
	period_ms = 1000.0 / fps;

	start_ns = now_ns(CLOCK_MONOTONIC) + 100000000ull;
	end_ns = start_ns + (uint64_t)(seconds * 1e9);
	pthread_create(&r, NULL, render_thread, NULL);
	pthread_create(&a, NULL, audio_thread, NULL);
	pthread_create(&m, NULL, main_thread, NULL);
	pthread_join(m, NULL);
	pthread_join(r, NULL);
	pthread_join(a, NULL);

	for (int i = 0; i < nframes; i++)
		missed += frame_ms[i] > period_ms;
	printf("frames %d missed %d (%.2f%%)\n", nframes, missed,
	       nframes ? 100.0 * missed / nframes : 0);
	printf("frame_ms p50 %.2f p99 %.2f max %.2f\n", pct(frame_ms, nframes, .5),
	       pct(frame_ms, nframes, .99), pct(frame_ms, nframes, 1));
	printf("main_wake_us p50 %.0f p99 %.0f\n", pct(main_wake_us, nmain, .5),
	       pct(main_wake_us, nmain, .99));
	printf("audio %d missed %d (%.2f%%) wake_us p50 %.0f p99 %.0f\n", naudio,
	       audio_missed, naudio ? 100.0 * audio_missed / naudio : 0,
	       pct(audio_wake_us, naudio, .5), pct(audio_wake_us, naudio, .99));
	return 0;
}
