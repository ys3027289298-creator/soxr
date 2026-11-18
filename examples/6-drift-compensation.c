/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Example 6: Asynchronous resampling with clock-drift compensation.
 *
 * Models a file being played through a clock device whose rate differs by a
 * few ppm.  A virtual device FIFO is fed by the resampler and drained at the
 * (slightly wrong) device rate; soxr_drift keeps the FIFO at its target level
 * by continuously trimming the resampling ratio.
 *
 * The loop is input-driven: each iteration feeds a fixed number of input
 * frames (one 20 ms file block); the output count varies with the commanded
 * ratio, which is what lets the FIFO respond to the correction.
 *
 * Usage: ./6-drift-compensation [device_ppm] [seconds]
 *
 * Raw float32 samples are written to stdout; progress goes to stderr. */

#include <soxr.h>
#include <soxr-drift.h>
#include "examples-common.h"

#define ORATE       48000
#define IRATE       44100
#define CHANNELS    2
#define ILEN        (IRATE / 50)               /* 20 ms input block. */
#define TARGET_FILL (ORATE / 5)               /* 200 ms FIFO. */
#define PERIOD      ((double)ILEN / IRATE)

int main(int argc, char const * arg[])
{
  double device_ppm = argc > 1? atof(arg[1]) : 5;
  long seconds = argc > 2? atol(arg[2]) : 60;
  size_t n_blocks = (size_t)(seconds / PERIOD);
  size_t block;
  soxr_error_t error = 0;
  soxr_drift_t dc;
  float ibuf[ILEN * CHANNELS], obuf[2048 * CHANNELS];
  double fifo_fill = TARGET_FILL;
  double device_rate = ORATE * (1 + device_ppm * 1e-6);

  dc = soxr_drift_create(IRATE, ORATE, CHANNELS, TARGET_FILL, &error);
  if (error) {
    fprintf(stderr, "create failed: %s\n", error);
    return 1;
  }

  for (block = 0; block < n_blocks; ++block) {
    size_t idone = 0, odone = 0, i;

    /* Any source data would do here; use a quiet tone for the demo. */
    for (i = 0; i < AL(ibuf); ++i)
      ibuf[i] = (float)(.2 * sin(2 * M_PI * (i / CHANNELS) * 440 / IRATE));

    error = soxr_process(soxr_drift_resampler(dc), ibuf, ILEN, &idone,
        obuf, AL(obuf) / CHANNELS, &odone);
    if (error) break;

    if (fwrite(obuf, sizeof(float), odone * CHANNELS, stdout) !=
        odone * CHANNELS) {
      error = "write failed";
      break;
    }

    /* The device drains at its own (drifting) clock over the block period. */
    fifo_fill += (double)odone;
    fifo_fill -= device_rate * PERIOD;

    /* One control update per device write; the ratio is trimmed in place. */
    soxr_drift_update(dc, fifo_fill, odone);

    if ((block % (size_t)(1 / PERIOD)) == 0) /* Once per second. */
      fprintf(stderr,
          "t=%4lus fifo=%8.2f (target %u) phase_err=%8.3f ratio=%.10f\n",
          (unsigned long)(block * ILEN / IRATE), fifo_fill,
          (unsigned)TARGET_FILL, soxr_drift_phase_error(dc),
          soxr_drift_ratio(dc));
  }

  fprintf(stderr, "%-26s %s\n", arg[0], soxr_strerror(error));
  soxr_drift_delete(dc);
  return !!error;
}
