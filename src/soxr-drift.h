/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Asynchronous (clock-drift compensating) resampling.
 *
 * When a file (or other source) clocked at one sample rate is played through a
 * device clocked at a slightly different rate, the difference between the two
 * clocks (typically a few parts per million) accumulates as a phase error:
 * the device buffer slowly drains (device clock is faster) or overflows (device
 * clock is slower).  This module implements a software phase-locked loop on
 * top of a variable-rate soxr resampler:
 *
 *   1. The phase error is estimated, in output samples, as the deviation of
 *      the device buffer (FIFO) fill level from its target level, averaged over
 *      a sliding window to attenuate the block-granularity sawtooth.
 *   2. A discrete PI controller converts the phase error into a small
 *      correction of the resampling I/O ratio.
 *   3. Per-update ratio changes are slew-rate limited, so the applied ratio
 *      jitter stays below 1 part in 10,000 (1e-4).
 *
 * Typical usage (one control update per block written to the device):
 *
 *   soxr_drift_t dc = soxr_drift_create(irate, orate, nch, target_fill, &err);
 *   for each block:
 *     soxr_process(soxr_drift_resampler(dc), ...);
 *     write odone samples to the device;
 *     soxr_drift_update(dc, fifo_fill_after_write, odone);
 *
 * All time/fill quantities are in samples per channel (frames), matching the
 * conventions of the main soxr API.                                    */

#if !defined soxr_drift_included
#define soxr_drift_included

#include "soxr.h"

/* soxr.h #undef's its SOXR visibility macro on exit; re-derive an equivalent. */
#if defined SOXR_DLL
  #if defined soxr_EXPORTS
    #define SOXR_DRIFT __declspec(dllexport)
  #else
    #define SOXR_DRIFT __declspec(dllimport)
  #endif
#elif defined SOXR_VISIBILITY && defined __GNUC__ && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1))
  #define SOXR_DRIFT __attribute__ ((visibility("default")))
#else
  #define SOXR_DRIFT
#endif

#if defined __cplusplus
  extern "C" {
#endif

typedef struct soxr_drift * soxr_drift_t;

/* Tunable parameters; soxr_drift_default_spec() supplies the defaults below.
 * All rates/levels are expressed in output samples (frames). */
typedef struct soxr_drift_spec {
  size_t window_len;     /* Sliding-window length, in control updates.  32    */
  double loop_tau;       /* PLL time constant, in seconds.               60   */
  double loop_damping;   /* PLL damping factor (1 = critically damped).  1    */
  double max_ratio_delta;/* Slew limit: |ratio change| per update.       5e-5 */
  double max_ratio_ppm;  /* Clamp of |ratio - nominal|, in ppm.          100  */
  double sample_rate;    /* Output (device) rate in Hz; 0 => use orate.  0    */
} soxr_drift_spec_t;

/* Library-default controller parameters (also used when a spec field is 0). */
SOXR_DRIFT soxr_drift_spec_t soxr_drift_default_spec(double output_rate);

/* Create a variable-rate resampler plus its drift-control loop.
 * target_fill: device FIFO level (output frames) that the loop maintains.
 * The underlying resampler is created with the SOXR_VR quality flag, which is
 * what permits its ratio to be changed at run time. */
SOXR_DRIFT soxr_drift_t soxr_drift_create(
    double input_rate,
    double output_rate,
    unsigned num_channels,
    size_t target_fill,
    soxr_error_t * error);

/* As above, with explicit controller parameters and I/O/quality specs
 * (any of which may be NULL for the library defaults). */
SOXR_DRIFT soxr_drift_t soxr_drift_create_spec(
    double input_rate,
    double output_rate,
    unsigned num_channels,
    size_t target_fill,
    soxr_drift_spec_t const * drift_spec,
    soxr_io_spec_t const * io_spec,
    soxr_quality_spec_t const * quality_spec,
    soxr_error_t * error);

SOXR_DRIFT void soxr_drift_delete(soxr_drift_t);

/* The underlying resampler, for soxr_process()/soxr_output() calls. */
SOXR_DRIFT soxr_t soxr_drift_resampler(soxr_drift_t);

/* One control iteration, invoked after a block has been written to the device.
 *   fifo_fill: device FIFO occupancy (output frames) after the write.
 *   block_len: number of output frames in the block just written.
 * Returns the (slew-limited) I/O ratio now commanded to the resampler. */
SOXR_DRIFT double soxr_drift_update(soxr_drift_t, double fifo_fill,
    size_t block_len);

/* Reset the loop (window history, integrator, commanded ratio) while keeping
 * the resampler itself and its configuration. */
SOXR_DRIFT soxr_error_t soxr_drift_reset(soxr_drift_t);

/* Current loop state, mainly for diagnostics/testing. */
SOXR_DRIFT double soxr_drift_phase_error(soxr_drift_t); /* Smoothed, out. frames. */
SOXR_DRIFT double soxr_drift_ratio(soxr_drift_t);        /* Commanded I/O ratio.  */
SOXR_DRIFT double soxr_drift_nominal_ratio(soxr_drift_t);

#if defined __cplusplus
  }
#endif

#endif
