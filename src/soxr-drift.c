/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Asynchronous (clock-drift compensating) resampling: a software phase-locked
 * loop that nudges a variable-rate soxr resampler so as to hold the playback
 * device FIFO at a target fill level.  See soxr-drift.h for the model.
 *
 * Control design (all internal quantities in seconds/dimensionless units):
 *   x = window-averaged FIFO phase error, in seconds;
 *   u = relative I/O-ratio correction, so ratio = nominal_ratio * (1 + u);
 *   plant  dx/dt = -u (one second of ratio mismatch moves one second of
 *                   phase), controlled by a continuous-time PI:
 *     u   = Kp*x + I
 *     I  += Ki*T*x,   Kp = 2*zeta/tau,  Ki = 1/tau^2
 *   with pole placement for a time constant tau and damping zeta.  The output
 *   is clamped to +/- max_ratio_ppm and each update is slew-rate limited.    */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "soxr.h"
#include "soxr-drift.h"
#include "internal.h"

struct soxr_drift {
  soxr_t soxr;

  /* Configuration: */
  double nominal_ratio;   /* input_rate / output_rate, without correction.  */
  double sample_rate;     /* Output (device) rate in Hz.                    */
  size_t target_fill;     /* Desired device FIFO occupancy (out. frames).   */
  double kp, ki;          /* PI gains (x in seconds; u dimensionless).      */
  double slew;            /* Limit on |ratio change| per update.            */
  double max_u;           /* Clamp on |relative correction| (ppm/1e6).      */

  /* Sliding-window phase-error estimator (ring buffer of raw errors): */
  double * window;
  size_t window_len;
  size_t window_count;    /* Samples stored so far (<= window_len).         */
  size_t window_index;    /* Next slot to overwrite.                        */
  double window_sum;

  /* Loop state: */
  double phase_error;     /* Window-averaged error, output frames.          */
  double integral;        /* Integral term I, in relative-ratio units.      */
  double ratio;           /* Currently commanded I/O ratio.                 */
};



soxr_drift_spec_t soxr_drift_default_spec(double output_rate)
{
  soxr_drift_spec_t spec;
  memset(&spec, 0, sizeof(spec));
  spec.window_len = 32;
  spec.loop_tau = 60;
  spec.loop_damping = 1;
  spec.max_ratio_delta = 5e-5;
  spec.max_ratio_ppm = 100;
  spec.sample_rate = output_rate;
  return spec;
}



soxr_drift_t soxr_drift_create_spec(
    double input_rate,
    double output_rate,
    unsigned num_channels,
    size_t target_fill,
    soxr_drift_spec_t const * drift_spec0,
    soxr_io_spec_t const * io_spec,
    soxr_quality_spec_t const * quality_spec0,
    soxr_error_t * error0)
{
  soxr_drift_t p;
  soxr_error_t error = 0;
  soxr_drift_spec_t spec;
  soxr_quality_spec_t q_spec;
  double nominal_ratio, ppm;

  if (input_rate <= 0 || output_rate <= 0 || !num_channels) {
    if (error0) *error0 = "invalid rate or channel argument";
    return 0;
  }

  spec = drift_spec0? *drift_spec0 : soxr_drift_default_spec(output_rate);
  if (spec.window_len == 0)        spec.window_len = 32;
  if (spec.loop_tau <= 0)          spec.loop_tau = 60;
  if (spec.loop_damping <= 0)      spec.loop_damping = 1;
  if (spec.max_ratio_delta <= 0)   spec.max_ratio_delta = 5e-5;
  if (spec.max_ratio_ppm <= 0)     spec.max_ratio_ppm = 100;
  if (spec.sample_rate <= 0)       spec.sample_rate = output_rate;

  nominal_ratio = input_rate / output_rate;

  if (!(p = calloc(1, sizeof(*p)))) {
    if (error0) *error0 = "malloc failed";
    return 0;
  }
  if (!(p->window = calloc(spec.window_len, sizeof(*p->window)))) {
    free(p);
    if (error0) *error0 = "malloc failed";
    return 0;
  }

  p->nominal_ratio = nominal_ratio;
  p->sample_rate = spec.sample_rate;
  p->target_fill = target_fill;
  p->window_len = spec.window_len;
  p->ratio = nominal_ratio;

  /* Continuous-time PI pole placement (see file header). */
  p->kp = 2 * spec.loop_damping / spec.loop_tau;
  p->ki = 1 / (spec.loop_tau * spec.loop_tau);
  p->slew = spec.max_ratio_delta;
  p->max_u = spec.max_ratio_ppm * 1e-6;

  /* A variable-rate resampler is required; the maximum ratio ever commanded
   * is the clamp limit, used only for internal stage sizing. */
  ppm = spec.max_ratio_ppm * 1e-6;
  q_spec = quality_spec0? *quality_spec0 : soxr_quality_spec(SOXR_HQ, SOXR_VR);
  q_spec.flags |= SOXR_VR;
  p->soxr = soxr_create(input_rate * (1 + ppm), output_rate, num_channels,
      &error, io_spec, &q_spec, 0);
  if (error) {
    free(p->window);
    free(p);
    if (error0) *error0 = error;
    return 0;
  }
  /* Establish the exact starting ratio (3rd arg 0 = no slewing). */
  error = soxr_set_io_ratio(p->soxr, nominal_ratio, 0);
  if (error) {
    soxr_delete(p->soxr);
    free(p->window);
    free(p);
    if (error0) *error0 = error;
    return 0;
  }

  if (error0) *error0 = 0;
  return p;
}



soxr_drift_t soxr_drift_create(
    double input_rate,
    double output_rate,
    unsigned num_channels,
    size_t target_fill,
    soxr_error_t * error)
{
  return soxr_drift_create_spec(input_rate, output_rate, num_channels,
      target_fill, 0, 0, 0, error);
}



void soxr_drift_delete(soxr_drift_t p)
{
  if (p) {
    soxr_delete(p->soxr);
    free(p->window);
    free(p);
  }
}



soxr_t soxr_drift_resampler(soxr_drift_t p)
{
  return p? p->soxr : 0;
}



double soxr_drift_update(soxr_drift_t p, double fifo_fill, size_t block_len)
{
  double raw_error, x, u, target_ratio, delta, period;
  size_t block_n = block_len? block_len : 1;

  if (!p) return 0;

  /* 1. Phase error = FIFO deviation from target; average over sliding window
   *    to suppress the sawtooth from block-wise FIFO reads. */
  raw_error = fifo_fill - (double)p->target_fill;
  if (p->window_count == p->window_len)
    p->window_sum -= p->window[p->window_index];
  else
    ++p->window_count;
  p->window[p->window_index] = raw_error;
  p->window_sum += raw_error;
  p->window_index = (p->window_index + 1) % p->window_len;
  p->phase_error = p->window_sum / (double)p->window_count;

  /* 2. PI control on the phase error expressed in seconds.  A negative error
   *    means the FIFO is draining (device clock runs fast), so the relative
   *    correction u goes negative, lowering the ratio and producing more
   *    output frames per input to refill the FIFO. */
  x = p->phase_error / p->sample_rate;
  period = (double)block_n / p->sample_rate;
  p->integral += p->ki * period * x;
  p->integral = range_limit(p->integral, -p->max_u, p->max_u);
  u = p->kp * x + p->integral;
  u = range_limit(u, -p->max_u, p->max_u);
  target_ratio = p->nominal_ratio * (1 + u);

  /* 3. Per-update slew limiting bounds the commanded-ratio jitter. */
  delta = target_ratio - p->ratio;
  if (delta > p->slew)
    target_ratio = p->ratio + p->slew;
  else if (delta < -p->slew)
    target_ratio = p->ratio - p->slew;
  p->ratio = target_ratio;

  /* 4. Command the resampler to ramp to the new ratio across this block, so
   *    the ratio changes sample-by-sample rather than in a step. */
  soxr_set_io_ratio(p->soxr, p->ratio, block_n);
  return p->ratio;
}



soxr_error_t soxr_drift_reset(soxr_drift_t p)
{
  soxr_error_t error;
  if (!p) return "invalid pointer";
  p->window_count = p->window_index = 0;
  p->window_sum = p->phase_error = p->integral = 0;
  p->ratio = p->nominal_ratio;
  error = soxr_set_io_ratio(p->soxr, p->nominal_ratio, 0);
  return error;
}



double soxr_drift_phase_error(soxr_drift_t p)
{
  return p? p->phase_error : 0;
}

double soxr_drift_ratio(soxr_drift_t p)
{
  return p? p->ratio : 0;
}

double soxr_drift_nominal_ratio(soxr_drift_t p)
{
  return p? p->nominal_ratio : 0;
}
