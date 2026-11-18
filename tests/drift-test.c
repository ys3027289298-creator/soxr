/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Verifies the asynchronous drift-compensation loop (soxr-drift).  A virtual
 * playback device is simulated: its clock differs from the file clock by a
 * configurable number of ppm, and it drains a FIFO that the resampler feeds.
 *
 * The model is input-driven, matching an asynchronous resampler in a real
 * playback path: each iteration feeds a fixed number of INPUT frames; the
 * number of output frames produced varies with the commanded ratio.  The
 * virtual device then consumes at its own (drifting) rate over the same
 * wall-clock period, so the FIFO responds to ratio changes and the loop is
 * genuinely closed.
 *
 * Checks against the design requirements:
 *   1. After a 10-hour session, the end-point (FIFO phase) error is below one
 *      output sample.
 *   2. The steady-state commanded ratio settles to the true clock offset.
 *   3. Per-update ratio changes never exceed the slew limit, and the ratio
 *      jitter in steady state is below 1 part in 10,000 (1e-4).
 *   4. The control-loop overhead is under 5% of the resampling CPU time.
 *   5. The loop recovers from an initial FIFO offset and from a drift step.   */

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <soxr.h>
#include <soxr-drift.h>

#undef min
#define min(x,y) ((x)<(y)?(x):(y))
#undef max
#define max(x,y) ((x)>(y)?(x):(y))
#undef AL
#define AL(a) (sizeof(a)/sizeof((a)[0]))
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ORATE         48000
#define IRATE         44100
#define CHANNELS      2
#define BLOCK_MS      20
#define ILEN          ((size_t)(IRATE * BLOCK_MS / 1000)) /* fixed input  */
#define TARGET_FILL   ((size_t)(ORATE * 2 / 10))  /* 200 ms device buffer. */
#define HOURS         10
#define PERIOD        ((double)ILEN / IRATE)      /* seconds per iteration */
#define N_BLOCKS      ((size_t)(HOURS * 3600 / PERIOD))
#define STEADY_MARGIN ((size_t)(3600 / PERIOD))   /* last hour = steady.   */

static int failures;

static void check(int condition, char const * what, double got, double want)
{
  printf("  [%s] %-44s got=%.10g want=%g\n",
      condition? "PASS" : "FAIL", what, got, want);
  failures += !condition;
}

/* One simulated playback session.  device_ppm: device clock relative to the
 * nominal output rate (positive = device runs fast).  initial_offset: FIFO
 * error in frames at session start.  step_block/step_ppm: optional drift step. */
static void run_session(double device_ppm, double initial_offset,
    size_t step_block, double step_ppm, double * final_phase,
    double * settled_ratio, double * max_abs_delta, double * jitter)
{
  soxr_error_t error = 0;
  soxr_drift_t dc;
  soxr_t soxr;
  float in[8192 * CHANNELS], out[4096 * CHANNELS];
  size_t block, i;
  double fifo_fill = (double)TARGET_FILL + initial_offset;
  double device_rate = (double)ORATE * (1 + device_ppm * 1e-6);
  double max_delta = 0, prev_ratio = (double)IRATE / ORATE;
  double jitter_sum = 0, jitter_n = 0;
  double ppm_now = device_ppm;

  dc = soxr_drift_create(IRATE, ORATE, CHANNELS, TARGET_FILL, &error);
  assert(!error);
  soxr = soxr_drift_resampler(dc);

  /* Deterministic, spectrally harmless input (a low-frequency sine). */
  for (i = 0; i < AL(in); ++i)
    in[i] = (float)(.5 * sin(2 * M_PI * ((double)i / CHANNELS) *
        440 / (double)IRATE));

  for (block = 0; block < N_BLOCKS; ++block) {
    size_t idone = 0, odone = 0;
    double ratio, delta;

    if (step_block && block == step_block) {
      ppm_now = step_ppm;
      device_rate = (double)ORATE * (1 + ppm_now * 1e-6);
    }

    /* Input-driven: feed a fixed number of input frames; the output count
     * varies with the ratio the controller currently commands. */
    error = soxr_process(soxr, in, ILEN, &idone, out, AL(out) / CHANNELS,
        &odone);
    assert(!error);
    assert(idone == ILEN);

    /* The device drains over the wall-clock span of one input block. */
    fifo_fill += (double)odone;
    fifo_fill -= device_rate * PERIOD;

    /* One control update per device write. */
    ratio = soxr_drift_update(dc, fifo_fill, odone);
    delta = fabs(ratio - prev_ratio);
    if (delta > max_delta) max_delta = delta;
    prev_ratio = ratio;

    /* Measure jitter over the steady-state region (last hour). */
    if (block >= N_BLOCKS - STEADY_MARGIN) {
      jitter_sum += delta;
      ++jitter_n;
    }
    assert(fifo_fill > 0); /* Under-run would invalidate the session. */
  }

  *final_phase = soxr_drift_phase_error(dc);
  *settled_ratio = soxr_drift_ratio(dc);
  *max_abs_delta = max_delta;
  *jitter = jitter_sum / jitter_n; /* Mean |delta| in the steady region. */
  soxr_drift_delete(dc);
}

/* The true I/O ratio when the device clock deviates by ppm: the resampler must
 * output at the device rate, so io_ratio = irate / (orate*(1+ppm*1e-6)). */
static double expected_ratio(double device_ppm)
{
  return (double)IRATE / ((double)ORATE * (1 + device_ppm * 1e-6));
}

/* CPU cost of the controller relative to resampling one block. */
static void measure_overhead(void)
{
  enum {REPEAT = 20000};
  soxr_error_t error = 0;
  soxr_drift_t dc;
  soxr_t soxr;
  float in[4096 * CHANNELS], out[4096 * CHANNELS];
  clock_t t0, t1;
  double t_resample, t_control;
  size_t k, idone, odone;

  dc = soxr_drift_create(IRATE, ORATE, CHANNELS, TARGET_FILL, &error);
  assert(!error);
  soxr = soxr_drift_resampler(dc);
  for (k = 0; k < AL(in); ++k)
    in[k] = (float)(.5 * sin(2 * M_PI * ((double)k / CHANNELS) *
        440 / (double)IRATE));

  /* Warm up (cache, branch prediction). */
  for (k = 0; k < 100; ++k) {
    soxr_process(soxr, in, ILEN, &idone, out, AL(out) / CHANNELS, &odone);
    soxr_drift_update(dc, (double)TARGET_FILL, odone);
  }

  t0 = clock();
  for (k = 0; k < REPEAT; ++k)
    soxr_process(soxr, in, ILEN, &idone, out, AL(out) / CHANNELS, &odone);
  t1 = clock();
  t_resample = (double)(t1 - t0) / CLOCKS_PER_SEC;

  t0 = clock();
  for (k = 0; k < REPEAT; ++k)
    soxr_drift_update(dc, (double)TARGET_FILL + (double)(k % 7) - 3, ILEN);
  t1 = clock();
  t_control = (double)(t1 - t0) / CLOCKS_PER_SEC;

  printf("  resample=%.4fs control=%.6fs over %d blocks\n",
      t_resample, t_control, REPEAT);
  check(t_control / t_resample < 0.05,
      "control overhead < 5% of resampling", t_control / t_resample, .05);
  soxr_drift_delete(dc);
}

int main(void)
{
  double final_phase, settled, max_delta, jitter;

  printf("Drift compensation test: %d hours, input block=%u frames (%.0f Hz)\n",
      HOURS, (unsigned)ILEN, (double)IRATE);
  printf("iterations/session=%zu\n", (size_t)N_BLOCKS);

  printf("Session 1: device +5 ppm, nominal start\n");
  run_session(+5, 0, 0, 0, &final_phase, &settled, &max_delta, &jitter);
  check(fabs(final_phase) < 1, "10 h end-point phase error < 1 sample",
      final_phase, 1);
  check(fabs(settled - expected_ratio(5)) < 1e-7,
      "settled ratio matches +5 ppm clock", settled, expected_ratio(5));
  check(max_delta <= 5e-5 + 1e-12, "slew limit 5e-5 per update never exceeded",
      max_delta, 5e-5);
  check(jitter < 1e-4, "steady-state ratio jitter < 1e-4", jitter, 1e-4);

  printf("Session 2: device -20 ppm (slow), 50-frame initial FIFO deficit\n");
  run_session(-20, -50, 0, 0, &final_phase, &settled, &max_delta, &jitter);
  check(fabs(final_phase) < 1, "10 h end-point phase error < 1 sample",
      final_phase, 1);
  check(fabs(settled - expected_ratio(-20)) < 1e-7,
      "settled ratio matches -20 ppm clock", settled, expected_ratio(-20));
  check(max_delta <= 5e-5 + 1e-12, "slew limit 5e-5 per update never exceeded",
      max_delta, 5e-5);

  printf("Session 3: drift step +2 -> +15 ppm after 3 hours\n");
  run_session(2, 0, (size_t)(3 * 3600 / PERIOD), 15,
      &final_phase, &settled, &max_delta, &jitter);
  check(fabs(final_phase) < 1, "post-step end-point phase error < 1 sample",
      final_phase, 1);
  check(fabs(settled - expected_ratio(15)) < 1e-7,
      "settled ratio tracks the new clock", settled, expected_ratio(15));

  printf("Overhead measurement:\n");
  measure_overhead();

  printf(failures? "\nFAILURE: %d check(s) failed\n" : "\nAll checks passed\n",
      failures);
  return failures != 0;
}
