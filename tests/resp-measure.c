/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* resp-measure: objective frequency-response measurement of the resampler.
 *
 * For each of 4 quality configurations (LQ/MQ/HQ/VHQ), at 48000 -> 24000 Hz:
 *   - Excites the resampler with (a) a linear swept sine to 0.49*Fs_in and
 *     (b) white noise, and derives the transfer function by
 *     deconvolution (sweep) and cross-spectral averaging (noise), with
 *     time-domain windowing correction of the deconvolved impulse response.
 *   - Compares both measured responses against the theoretical response
 *     (FFT of the resampler's own impulse response); must agree < 0.5 dB.
 *   - Reports passband ripple, stopband/alias level (tracking-heterodyne
 *     dechirp of the sweep), and group delay with sub-sample resolution.
 *
 * Exit status 0 iff all checks pass and total runtime < 5 s. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "soxr.h"

/* pffft is used in this project as a single-include library: */
static void * xaligned(size_t n) {void * p = 0; return posix_memalign(&p, 64, n)? 0 : p;}
#define pffft_aligned_malloc(n) xaligned(n)
#define pffft_aligned_free      free
#include "pffft.c"

#define FS_IN   48000.
#define FS_OUT  24000.
#define T       5.461333333333333           /* Analysis length, seconds. */
#define N_IN    262144                      /* = FS_IN *T = 2^18 (pffft: 2^k only) */
#define N_OUT   131072                      /* = FS_OUT*T = 2^17           */
#define LEAD_I  48000                       /* 1 s lead-in/out each side */
#define LEAD_O  24000
#define TOT_I   (N_IN + 2 * LEAD_I)
#define TOT_O   (N_OUT + 2 * LEAD_O)
#define F0      10.                         /* Sweep start, Hz */
#define F1      (0.49 * FS_IN)              /* Sweep end = 23520 Hz */
#define SW_AMP  0.25
#define NZ_RMS  0.1
#define SEG_O   16384                       /* Welch segment (output) = 2^14 */
#define SEG_I   32768                       /* Welch segment (input) = 2^15 */
#define HOP_O   8192
#define HOP_I   16384
#define NSEG    ((N_OUT - SEG_O) / HOP_O + 1)
#define K_NYQ   (N_OUT / 2)                 /* Bins: 0..65536, 0.24414 Hz each */
#define DB_MIN  (-30.)                      /* Comparison floor for |H_ref| */
#define DEV_MAX 0.5                         /* Required agreement, dB */
#define ALIAS_MAX (-80.)                    /* Required alias suppression, dB */
#define TIME_MAX 5.0                        /* Required max runtime, s */

#define PI M_PI

typedef struct { double re, im; } cplx;

static void * emalloc(size_t n) {           /* emalloc: exit on failure */
  void * p = calloc(1, n);
  if (!p) fprintf(stderr, "out of memory\n"), exit(1);
  return p;
}

/* ---- pffft helpers (ordered real layout: [re0, reN/2, re1, im1, ...]) ---- */

static PFFFT_Setup * fft_setup(int n) {
  PFFFT_Setup * s = pffft_new_setup(n, PFFFT_REAL);
  if (!s) fprintf(stderr, "pffft: bad size %d\n", n), exit(1);
  return s;
}

static float * fft_work(int n) {            /* Aligned scratch for pffft */
  return pffft_aligned_malloc((size_t)n * sizeof(float));
}

static void rfft(PFFFT_Setup * s, float const * in, float * out, float * work) {
  pffft_transform_ordered(s, in, out, work, PFFFT_FORWARD);
}

static void irfft(PFFFT_Setup * s, float * inout, float * work, int n) {
  pffft_transform_ordered(s, inout, inout, work, PFFFT_BACKWARD);
  { int i; for (i = 0; i < n; ++i) inout[i] /= n; }
}

static cplx bin(float const * v, int k, int n) { /* k = 0..n/2 */
  cplx c;
  if (k == 0)      { c.re = v[0]; c.im = 0; }
  else if (k == n/2) { c.re = v[1]; c.im = 0; }
  else             { c.re = v[2*k]; c.im = v[2*k+1]; }
  return c;
}

static void set_bin(float * v, int k, int n, cplx c) {
  if (k == 0)      { v[0] = (float)c.re; }
  else if (k == n/2) { v[1] = (float)c.re; }
  else             { v[2*k] = (float)c.re; v[2*k+1] = (float)c.im; }
}

/* ---- signal generation ---- */

static double sweep_phase(double t) {       /* Cycles; linear chirp F0->F1 over T s */
  return F0 * t + (F1 - F0) * t * t / (2 * T);
}

static void gen_sweep(double * x) {
  int i;
  for (i = 0; i < TOT_I; ++i) {
    double t = i / FS_IN - 1., dummy, ph = sweep_phase(t);
    x[i] = SW_AMP * sin(2 * PI * modf(ph, &dummy));
  }
}

static void gen_noise(double * x) {         /* Fixed-seed white noise */
  unsigned long long s = 0x9E3779B97F4A7C15ull;
  int i;
  double a = NZ_RMS * sqrt(3.);
  for (i = 0; i < TOT_I; ++i) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;      /* xorshift64 */
    x[i] = a * (2. * (s >> 11) * (1. / 9007199254740992.) - 1.);
  }
}

static void gen_impulse(double * x) {
  memset(x, 0, TOT_I * sizeof(*x));
  /* soxr_oneshot centres the (zero-phase) IR at the impulse's output
   * position; place the impulse so the IR lands centred in the window: */
  x[LEAD_I + N_OUT] = 1.;
}

/* ---- resampling ---- */

static size_t resample(unsigned long recipe, double const * in, double * out) {
  size_t idone, odone = 0;
  soxr_quality_spec_t q = soxr_quality_spec(recipe, 0);
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
  soxr_error_t e = soxr_oneshot(FS_IN, FS_OUT, 1, in, TOT_I, &idone,
                                out, TOT_O, &odone, &io, &q, 0);
  if (e) fprintf(stderr, "soxr: %s\n", e), exit(1);
  return odone;
}

/* ---- analysis helpers ---- */

static double to_db(double mag) { return 20 * log10(mag + 1e-300); }

/* Least-squares line fit; returns slope, optionally intercept & slope stderr. */
static double linfit(double const * x, double const * y, int n,
                     double * intercept, double * slope_err) {
  double sx = 0, sy = 0, sxx = 0, sxy = 0, b, a, r = 0;
  int i;
  for (i = 0; i < n; ++i) sx += x[i], sy += y[i];
  sx /= n; sy /= n;
  for (i = 0; i < n; ++i) sxx += (x[i]-sx)*(x[i]-sx), sxy += (x[i]-sx)*(y[i]-sy);
  b = sxy / sxx; a = sy - b * sx;
  for (i = 0; i < n; ++i) { double d = y[i] - a - b*x[i]; r += d*d; }
  if (intercept) *intercept = a;
  if (slope_err) *slope_err = n > 2? sqrt(r / (n-2) / sxx) : 0;
  return b;
}

/* Unwrapped-phase group delay (output samples) from spectrum on N_OUT grid,
 * over bins [k1, k2]. Returns delay; *res = fit stderr (resolution). */
static double group_delay(float const * spec, int nfft, int k1, int k2,
                          double * res) {
  double * ks = emalloc(sizeof(double) * (k2-k1+1));
  double * ph = emalloc(sizeof(double) * (k2-k1+1));
  double prev = 0, off = 0, b, be;
  int k, n = 0;
  for (k = k1; k <= k2; ++k) {
    cplx c = bin(spec, k, nfft);
    double p = atan2(c.im, c.re);
    if (n) {
      double d = p - prev;
      while (d >  PI) d -= 2*PI, off -= 2*PI;
      while (d < -PI) d += 2*PI, off += 2*PI;
    }
    ks[n] = k; ph[n] = p + off; prev = p; ++n;
  }
  b = linfit(ks, ph, n, 0, &be);            /* phi = a + b*k */
  free(ks); free(ph);
  *res = be * nfft / (2*PI);
  return -b * nfft / (2*PI);
}

/* ---- per-configuration measurement ---- */

typedef struct {
  char const * name;
  unsigned long recipe;
} config_t;

static int check(char const * what, double val, double lo, double hi) {
  int ok = val >= lo && val <= hi;
  printf("  %-34s %13.6g  [%g .. %g]  %s\n", what, val, lo, hi,
         ok? "ok" : "FAIL");
  return ok;
}

int main(void) {
  static config_t const configs[] = {
    {"LQ", SOXR_LQ}, {"MQ", SOXR_MQ}, {"HQ", SOXR_HQ}, {"VHQ", SOXR_VHQ}};
  double * x_sweep = emalloc(TOT_I * sizeof(double));
  double * x_noise = emalloc(TOT_I * sizeof(double));
  double * x_imp   = emalloc(TOT_I * sizeof(double));
  double * y       = emalloc(TOT_O * sizeof(double));
  float  * fx      = emalloc(N_IN  * sizeof(float));
  float  * fy      = emalloc(N_IN  * sizeof(float)); /* max(N_IN, N_OUT) */
  float  * spec_x  = emalloc((N_IN + 2) * sizeof(float));
  float  * spec_y  = emalloc((N_IN + 2) * sizeof(float));
  float  * h       = emalloc(N_OUT * sizeof(float));
  double * w       = emalloc(N_OUT * sizeof(double)); /* window */
  float * work_in  = fft_work(N_IN);
  float * work_out = fft_work(N_OUT);
  float * work_si  = fft_work(SEG_I);
  float * work_so  = fft_work(SEG_O);
  PFFFT_Setup * fft_in  = fft_setup(N_IN);
  PFFFT_Setup * fft_out = fft_setup(N_OUT);
  PFFFT_Setup * fft_si  = fft_setup(SEG_I);
  PFFFT_Setup * fft_so  = fft_setup(SEG_O);
  struct timespec t0, t1;
  int all_ok = 1, ci, i, k;

  gen_sweep(x_sweep);
  gen_noise(x_noise);
  gen_impulse(x_imp);

  /* Hann windows for Welch: */
  double * hann_i = emalloc(SEG_I * sizeof(double));
  double * hann_o = emalloc(SEG_O * sizeof(double));
  for (i = 0; i < SEG_I; ++i) hann_i[i] = .5 - .5*cos(2*PI*i/(SEG_I-1));
  for (i = 0; i < SEG_O; ++i) hann_o[i] = .5 - .5*cos(2*PI*i/(SEG_O-1));

  clock_gettime(CLOCK_MONOTONIC, &t0);
  printf("resp-measure: %.0f -> %.0f Hz, sweep %.0f..%.0f Hz + white noise,"
         " T=%.3f s\n\n", FS_IN, FS_OUT, F0, F1, T);

  for (ci = 0; ci < 4; ++ci) {
    config_t const * cfg = &configs[ci];
    soxr_quality_spec_t q = soxr_quality_spec(cfg->recipe, 0);
    double rej_db = q.precision * 6.0206;
    double pb_end_hz = q.passband_end * FS_OUT / 2;
    int ok = 1;

    /* --- theoretical reference: impulse response --- */
    resample(cfg->recipe, x_imp, y);
    double t_ref = 0, wsum = 0, max_mag = 0, hmax = 0;
    for (i = 0; i < N_OUT; ++i) {
      double v = y[LEAD_O + i];
      fy[i] = (float)v;
      t_ref += i * v*v; wsum += v*v;
      hmax = fmax(hmax, fabs(v));
    }
    t_ref /= wsum;                            /* IR centre (samples) */
    double gd0 = t_ref - N_OUT/2;             /* Residual group delay */
    int W = 512;                              /* IR support half-width: */
    for (i = 0; i < N_OUT; ++i) {             /* max circular dist from centre */
      int d = abs(i - N_OUT/2); if (d > N_OUT/2) d = N_OUT - d;
      if (fabs(fy[i]) > 1e-6 * hmax && d + 256 > W) W = d + 256;
    }
    if (W > N_OUT/2 - 1) W = N_OUT/2 - 1;
    rfft(fft_out, fy, spec_y, work_out);       /* spec_y := H_ref */
    double * ref_db = emalloc((K_NYQ+1) * sizeof(double));
    for (k = 0; k <= K_NYQ; ++k) {
      cplx c = bin(spec_y, k, N_OUT);
      ref_db[k] = to_db(hypot(c.re, c.im));
      if (k) max_mag = fmax(max_mag, ref_db[k]);
    }

    /* --- sweep excitation: deconvolution + windowing correction --- */
    resample(cfg->recipe, x_sweep, y);
    for (i = 0; i < N_IN;  ++i) fx[i] = (float)x_sweep[LEAD_I + i];
    for (i = 0; i < N_OUT; ++i) fy[i] = (float)y[LEAD_O + i];
    rfft(fft_in,  fx, spec_x, work_in);
    rfft(fft_out, fy, spec_y, work_out);
    double xmax = 0;
    for (k = 1; k < N_IN/2; ++k) {
      cplx c = bin(spec_x, k, N_IN);
      xmax = fmax(xmax, c.re*c.re + c.im*c.im);
    }
    xmax = sqrt(xmax) * 1e-4;                 /* Regularisation threshold */
    for (k = 0; k <= K_NYQ; ++k) {            /* H = Y/X on common Hz grid */
      cplx cx = bin(spec_x, k, N_IN), cy = bin(spec_y, k, N_OUT), r;
      double m = cx.re*cx.re + cx.im*cx.im;
      if (m > xmax*xmax) {
        r.re = (cy.re*cx.re + cy.im*cx.im) / m;
        r.im = (cy.im*cx.re - cy.re*cx.im) / m;
      } else r.re = r.im = 0;
      set_bin(spec_y, k, N_OUT, r);
    }
    memcpy(h, spec_y, N_OUT * sizeof(float));
    irfft(fft_out, h, work_out, N_OUT);       /* h := deconvolved IR */
    int peak = 0;
    double pv = 0;
    for (i = 0; i < N_OUT; ++i)
      if (fabs(h[i]) > pv) pv = fabs(h[i]), peak = i;
    for (i = 0; i < N_OUT; ++i) {             /* Blackman window about peak, */
      int d = abs(i - peak);                  /* circular (IR wraps at t=0)  */
      if (d > N_OUT/2) d = N_OUT - d;
      if (d > W) w[i] = 0;
      else w[i] = .42 + .5*cos(PI*d/W) + .08*cos(2*PI*d/W);
      h[i] = (float)(h[i] * w[i]);
    }
    rfft(fft_out, h, spec_y, work_out);       /* spec_y := H_sweep (windowed) */
    double * sw_db = emalloc((K_NYQ+1) * sizeof(double));
    for (k = 0; k <= K_NYQ; ++k) {
      cplx c = bin(spec_y, k, N_OUT);
      sw_db[k] = to_db(hypot(c.re, c.im));
    }

    /* --- white-noise excitation: Welch cross-spectrum H = Sxy/Sxx --- */
    resample(cfg->recipe, x_noise, y);
    /* Residual delay of this config (output samples), from impulse run: */
    double d_out = gd0;
    double i_exact0 = (0. - d_out) * (FS_IN/FS_OUT) + LEAD_I;
    long i_r0 = lround(i_exact0);
    double delta_in = i_exact0 - i_r0;        /* constant for all segments */
    cplx * sxy = emalloc((SEG_O/2+1) * sizeof(cplx));
    double * sxx = emalloc((SEG_O/2+1) * sizeof(double));
    memset(sxy, 0, (SEG_O/2+1) * sizeof(*sxy));
    memset(sxx, 0, (SEG_O/2+1) * sizeof(*sxx));
    float * sx = emalloc((SEG_I+2) * sizeof(float));
    float * sy = emalloc((SEG_O+2) * sizeof(float));
    float * seg  = emalloc(SEG_I * sizeof(float));
    int s;
    for (s = 0; s < NSEG; ++s) {
      long i0 = i_r0 + (long)s * HOP_I;
      long o0 = LEAD_O + (long)s * HOP_O;
      for (i = 0; i < SEG_I; ++i) seg[i] = (float)(x_noise[i0+i] * hann_i[i]);
      rfft(fft_si, seg, sx, work_si);
      for (i = 0; i < SEG_O; ++i) seg[i] = (float)(y[o0+i] * hann_o[i]);
      rfft(fft_so, seg, sy, work_so);
      for (k = 0; k <= SEG_O/2; ++k) {
        cplx cx = bin(sx, k, SEG_I), cy = bin(sy, k, SEG_O);
        sxy[k].re += cy.re*cx.re + cy.im*cx.im;
        sxy[k].im += cy.im*cx.re - cy.re*cx.im;
        sxx[k] += cx.re*cx.re + cx.im*cx.im;
      }
    }
    float * spec_nz = emalloc((SEG_O+2) * sizeof(float));
    for (k = 0; k <= SEG_O/2; ++k) {
      cplx r = {sxy[k].re / sxx[k], sxy[k].im / sxx[k]};
      set_bin(spec_nz, k, SEG_O, r);
    }

    /* --- dechirp (tracking heterodyne) for stopband / alias level --- */
    resample(cfg->recipe, x_sweep, y);        /* Reuse sweep output */
    double dly = gd0;                         /* Align heterodyne to signal */
    int L = 2048, hop = 512, m, nenv = (N_OUT - L)/hop + 1;
    double env_max_alias = 0, env_ref = 0; int nref = 0;
    for (m = 0; m < nenv; ++m) {
      double zr = 0, zi = 0, t_c = (m*hop + L/2 - dly) / FS_OUT;
      double f_c = F0 + (F1 - F0) * t_c / T;
      if (f_c < 500) continue;                /* Skip DC/startup region */
      for (i = 0; i < L; ++i) {
        int n = m*hop + i;
        double ph = sweep_phase((n - dly) / FS_OUT);
        zr += y[LEAD_O + n] * cos(2*PI*ph);
        zi -= y[LEAD_O + n] * sin(2*PI*ph);
      }
      { double e = hypot(zr, zi) / L;
        if (f_c >= 1000 && f_c <= 6000) env_ref += e, ++nref;
        else if (f_c >= FS_OUT/2 + 500 && e > env_max_alias) env_max_alias = e;
      }
    }
    env_ref /= nref;
    double alias_db = to_db(env_max_alias / env_ref);

    /* --- metrics --- */
    int k_100  = (int)ceil(100. / (FS_OUT/N_OUT));
    int k_pb   = (int)(0.98 * pb_end_hz / (FS_OUT/N_OUT));
    int k_gd1  = (int)ceil(200. / (FS_OUT/N_OUT));
    int k_gd2  = (int)(0.90 * pb_end_hz / (FS_OUT/N_OUT));
    double rip_lo = 1e9, rip_hi = -1e9, dev_sw = 0, dev_nz = 0;
    for (k = k_100; k <= k_pb; ++k) {
      rip_lo = fmin(rip_lo, sw_db[k]);
      rip_hi = fmax(rip_hi, sw_db[k]);
    }
    { int kmax = 0;
    for (k = k_100; k <= K_NYQ - 8; ++k)      /* Compare where ref >= -30 dB */
      if (ref_db[k] >= max_mag + DB_MIN) {
        double d = fabs(sw_db[k] - ref_db[k]);
        if (d > dev_sw) dev_sw = d, kmax = k;
      }
    if (getenv("RESP_DEBUG"))
      fprintf(stderr, "DEBUG %s: dev_sw max at f=%.0f Hz, ref=%.2f dB\n",
              cfg->name, kmax*(FS_OUT/N_OUT), ref_db[kmax]); }
    for (k = (int)ceil(100./(FS_OUT/SEG_O)); k <= SEG_O/2 - 1; ++k)
      if (ref_db[k*8] >= max_mag + DB_MIN) {
        cplx c = bin(spec_nz, k, SEG_O);
        dev_nz = fmax(dev_nz, fabs(to_db(hypot(c.re,c.im)) - ref_db[k*8]));
      }
    double res_sw, res_nz;
    double gd_sw = group_delay(spec_y, N_OUT, k_gd1, k_gd2, &res_sw);
    double gd_nz = group_delay(spec_nz, SEG_O,
                               (int)ceil(200./(FS_OUT/SEG_O)),
                               (int)(0.90*pb_end_hz/(FS_OUT/SEG_O)), &res_nz);
    gd_nz -= delta_in * (FS_OUT/FS_IN);       /* Segment-alignment correction */

    printf("%s (precision %g bits, theory rej %.0f dB, passband to %.0f Hz)\n",
           cfg->name, q.precision, rej_db, pb_end_hz);
    ok &= check("passband ripple, dB p-p", rip_hi - rip_lo, 0, 1.0);
    ok &= check("stopband/alias level, dB", alias_db, -300, ALIAS_MAX);
    ok &= check("|H_sweep - H_theory| max, dB", dev_sw, 0, DEV_MAX);
    ok &= check("|H_noise - H_theory| max, dB", dev_nz, 0, DEV_MAX);
    ok &= check("group delay, output samples", gd_sw, gd0 - 1, gd0 + 1);
    ok &= check("group delay (noise), samples", gd_nz, gd0 - 1, gd0 + 1);
    ok &= check("group-delay resolution, samples",
                fmax(res_sw, res_nz), 0, 1.0);
    printf("  %-34s %13.6f  (reference)\n", "group delay theory, samples", gd0);
    all_ok &= ok;
    printf("  => %s\n\n", ok? "PASS" : "FAIL");

    free(ref_db); free(sw_db); free(sxy); free(sxx);
    free(sx); free(sy); free(seg); free(spec_nz);
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  {
    double dt = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
    all_ok &= check("total runtime, s", dt, 0, TIME_MAX);
  }
  printf("\noverall: %s\n", all_ok? "PASS" : "FAIL");
  return !all_ok;
}
