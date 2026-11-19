/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Example 6: Group-delay measurement & equalisation for cascaded resampling.
 *
 * When several tracks are resampled through different multi-stage cascades
 * (e.g. 96 kHz -> 44.1 kHz directly, or via 48 kHz, or with different
 * quality/phase settings), each cascade exhibits a different end-to-end
 * group delay, so the tracks fall out of alignment.  The total delay of a
 * cascade has two components, both measured here:
 *
 *   - the intrinsic pipeline latency of the resampling chain (the delay a
 *     host would use for plug-in delay compensation), measured as the
 *     output still pending after a large block has been streamed through
 *     the chain without flushing;
 *   - the content group delay vs. frequency, measured by a stepped sine
 *     sweep: group delay = -d(phase)/d(omega).  (soxr time-aligns its
 *     output, so for linear-phase settings this is ~0, but non-linear
 *     phase settings give a frequency-dependent delay of a few samples.)
 *
 * For each of 4 cascade configurations this example then:
 *
 *   1. Measures both delay components (the sweep uses the peak of the
 *      cascade impulse response as a coarse reference for phase unwrap).
 *   2. Designs a short FIR pre-filter by weighted least-squares so that
 *      filter + cascade meets a linear-phase target with a configurable
 *      residual (target) delay.  The bulk of the correction is realised
 *      as a zero-cost integer offset; the FIR only provides the sub-sample
 *      delay and the phase equalisation, and has (by construction) a flat
 *      magnitude response.
 *   3. Verifies by re-measurement: residual group-delay flatness < 0.1
 *      sample, introduced magnitude ripple < 0.01 dB, and added-latency
 *      overhead < 3% of the slowest cascade's total delay.
 *
 * Optional argument: TARGET-DELAY (in output samples) to which all
 * configurations are aligned; the default is the smallest delay reachable
 * by all configurations.
 *
 * Exit status is 0 iff all configurations meet all criteria.
 */

#define _POSIX_C_SOURCE 199309L /* clock_gettime */

#include <soxr.h>
#include "examples-common.h"
#include <time.h>

#define NUM_CONFIGS 4
#define MAX_STAGES  3
#define SWEEP_PTS   64          /* Measurement frequencies per response.  */
#define F_LO        0.01        /* Sweep range, as a fraction of fs_out.  */
#define F_HI        0.42        /* (Kept inside the cascade pass-band.)   */
#define SIG_LEN     (1 << 18)   /* Sine length per measurement frequency. */
#define MAX_FIR_LEN 96

#define FLAT_LIMIT   0.1        /* Residual GD flatness, output samples.  */
#define RIPPLE_LIMIT 0.01       /* Introduced magnitude ripple, dB.       */
#define OH_LIMIT     3.0        /* Added-latency overhead, %.             */

typedef struct { double irate, orate; unsigned long recipe; } stage_t;
typedef struct { char const * name; stage_t stages[MAX_STAGES]; int num_stages; }
  cascade_t;

/* Compensation pre-filter: y[n] = sum_k h[k] * x[n - offset - k]; the
 * integer `offset' is a pure re-alignment (zero run-time cost), only the
 * short FIR h[] touches the signal. */
typedef struct { double h[MAX_FIR_LEN]; int len, offset; } comp_t;

/* 4 tracks, all resampled from 96 kHz to 44.1 kHz via different cascades: */
static cascade_t const configs[NUM_CONFIGS] = {
  {"A: 96k>44k1 1-stage HQ",       {{96000, 44100, SOXR_HQ}}, 1},
  {"B: 96k>48k>44k1 2-stage HQ",   {{96000, 48000, SOXR_HQ},
                                    {48000, 44100, SOXR_HQ}}, 2},
  {"C: 96k>44k1 1-stage VHQ steep",{{96000, 44100, SOXR_VHQ|SOXR_STEEP_FILTER}}, 1},
  {"D: 96k>44k1 1-stage HQ interm",{{96000, 44100, SOXR_HQ|SOXR_INTERMEDIATE_PHASE}}, 1},
};

static double now_s(void)
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static double * resample(stage_t const * s, double const * in, size_t n,
    size_t * n_out)
{
  size_t cap = (size_t)((double)n * s->orate / s->irate) + (1 << 16);
  double * out = malloc(sizeof(*out) * cap);
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
  soxr_quality_spec_t q = soxr_quality_spec(s->recipe, 0);
  soxr_error_t e = soxr_oneshot(s->irate, s->orate, 1,
      in, n, 0, out, cap, n_out, &io, &q, 0);
  if (e || *n_out == cap) {
    fprintf(stderr, "resample failed: %s\n", e? e : "output buffer full");
    exit(1);
  }
  return out;
}

static double * apply_comp(comp_t const * c, double const * x, size_t n)
{
  double * y = malloc(sizeof(*y) * n);
  size_t i = 0, lead = (size_t)(c->offset + c->len);
  int k;
  for (; i < n && i < lead; ++i) {              /* Boundary: zero history. */
    double a = 0;
    for (k = 0; k < c->len; ++k) {
      long j = (long)i - c->offset - k;
      if (j >= 0) a += c->h[k] * x[j];
    }
    y[i] = a;
  }
  for (; i < n; ++i) {                                   /* Steady state. */
    double const * p = x + (i - (size_t)c->offset);
    double a = 0;
    for (k = 0; k < c->len; ++k) a += c->h[k] * p[-k];
    y[i] = a;
  }
  return y;
}

static double * run_cascade(cascade_t const * c, comp_t const * comp,
    double const * in, size_t n, size_t * n_out)
{
  double const * x = in;
  double * owned = 0;
  int s;
  if (comp) x = owned = apply_comp(comp, in, n);
  for (s = 0; s < c->num_stages; ++s) {
    double * o = resample(&c->stages[s], x, n, n_out);
    free(owned);
    x = o; owned = o; n = *n_out;
  }
  return owned;
}

/* Coarse delay reference: peak of the cascade impulse response (output
 * samples); used only to make phase unwrapping unambiguous. */
static double impulse_delay(cascade_t const * c)
{
  size_t n = 1 << 18, m = 0, i, best = 0;
  double * x = calloc(n, sizeof(*x)), * y, peak = 0;
  x[0] = 1;
  y = run_cascade(c, 0, x, n, &m);
  for (i = 0; i < m; ++i)
    if (fabs(y[i]) > peak) { peak = fabs(y[i]); best = i; }
  free(x); free(y);
  return (double)best;
}

/* Intrinsic pipeline latency of the cascade (output samples): stream one
 * large block through the chain without flushing and count the output
 * still pending, i.e. the delay a host would use for delay compensation. */
static double pipe_latency(cascade_t const * c)
{
  size_t n = 1 << 18, m = n, i;
  double ratio = 1, * x = calloc(n, sizeof(*x)), * y = 0;
  double const * in = x;
  for (i = 0; i < (size_t)c->num_stages; ++i) ratio *= c->stages[i].orate / c->stages[i].irate;
  for (i = 0; i < (size_t)c->num_stages; ++i) {
    stage_t const * s = &c->stages[i];
    size_t cap = (size_t)(m * s->orate / s->irate) + (1 << 16), idone = 0, odone = 0;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    soxr_quality_spec_t q = soxr_quality_spec(s->recipe, 0);
    soxr_error_t e;
    soxr_t r = soxr_create(s->irate, s->orate, 1, &e, &io, &q, 0);
    free(y);
    y = malloc(sizeof(*y) * cap);
    e = soxr_process(r, in, m, &idone, y, cap, &odone);
    if (e) { fprintf(stderr, "soxr error: %s\n", e); exit(1); }
    m = odone;
    in = y;
    soxr_delete(r);
  }
  free(x); free(y);
  return n * ratio - (double)m;
}

/* Stepped sine sweep: complex response of (comp + cascade) at each freq. */
static void measure(cascade_t const * c, comp_t const * comp,
    double const * freqs, int K, double * amp, double * ph)
{
  double fs_in = c->stages[0].irate;
  double fs_out = c->stages[c->num_stages - 1].orate;
  double * x = malloc(sizeof(*x) * SIG_LEN);
  int k;
  for (k = 0; k < K; ++k) {
    double wi = 2 * M_PI * freqs[k] / fs_in, wo = 2 * M_PI * freqs[k] / fs_out;
    double cw = cos(wo), sw = sin(wo), ca = 1, sa = 0, re = 0, im = 0, t;
    double * y;
    size_t i, m = 0, s, e;
    for (i = 0; i < SIG_LEN; ++i) x[i] = sin(wi * (double)i);
    y = run_cascade(c, comp, x, SIG_LEN, &m);
    s = m / 4; e = m - s;              /* Use only the steady-state middle. */
    ca = cos(wo * (double)s); sa = sin(wo * (double)s);
    for (i = s; i < e; ++i) {
      re += y[i] * ca; im -= y[i] * sa;
      t = ca * cw - sa * sw; sa = sa * cw + ca * sw; ca = t;
    }
    ph[k] = atan2(im, re) + M_PI / 2;  /* Input sine has phase -pi/2. */
    amp[k] = 2 * sqrt(re * re + im * im) / (double)(e - s);
    free(y);
  }
  free(x);
}

static void unwrap(double * r, int n)
{
  int i;
  for (i = 1; i < n; ++i)
    r[i] += 2 * M_PI * floor((r[i-1] - r[i]) / (2 * M_PI) + .5);
}

/* Unwrapped total phase from measured phases, given a coarse delay ref. */
static void total_phase(double const * ph, double const * freqs, int K,
    double fs_out, double d_ref, double * ph_total)
{
  int k;
  for (k = 0; k < K; ++k)
    ph_total[k] = ph[k] + 2 * M_PI * freqs[k] / fs_out * d_ref;
  unwrap(ph_total, K);
  for (k = 0; k < K; ++k)
    ph_total[k] -= 2 * M_PI * freqs[k] / fs_out * d_ref;
}

/* Group delay (output samples) = -dphi/d-omega of the unwrapped phase. */
static void gd_from_phase(double const * pht, double const * freqs, int K,
    double fs_out, double * gd)
{
  int k;
  for (k = 0; k < K; ++k) {
    int k0 = k? k - 1 : 0, k1 = k + 1 < K? k + 1 : K - 1;
    double dw = 2 * M_PI * (freqs[k1] - freqs[k0]) / fs_out;
    gd[k] = -(pht[k1] - pht[k0]) / dw;
  }
}

static void gd_stats(double const * gd, int K, int skip, double * mean,
    double * pp)
{
  double mn = 1e300, mx = -1e300, s = 0;
  int k, n = 0;
  for (k = skip; k < K - skip; ++k) {
    s += gd[k]; ++n;
    if (gd[k] < mn) mn = gd[k];
    if (gd[k] > mx) mx = gd[k];
  }
  *mean = s / n; *pp = mx - mn;
}

/* Solve a symmetric (Toeplitz) system a x = b in place; b <- x. */
static int solve_sym(double * a, double * b, int n)
{
  int i, j, k;
  for (i = 0; i < n; ++i) {
    int piv = i;
    for (j = i + 1; j < n; ++j)
      if (fabs(a[j*n+i]) > fabs(a[piv*n+i])) piv = j;
    if (fabs(a[piv*n+i]) < 1e-12) return 1;
    if (piv != i) {
      for (k = i; k < n; ++k)
        { double t = a[i*n+k]; a[i*n+k] = a[piv*n+k]; a[piv*n+k] = t; }
      { double t = b[i]; b[i] = b[piv]; b[piv] = t; }
    }
    for (j = i + 1; j < n; ++j) {
      double f = a[j*n+i] / a[i*n+i];
      for (k = i; k < n; ++k) a[j*n+k] -= f * a[i*n+k];
      b[j] -= f * b[i];
    }
  }
  for (i = n - 1; i >= 0; --i) {
    for (j = i + 1; j < n; ++j) b[i] -= a[i*n+j] * b[j];
    b[i] /= a[i*n+i];
  }
  return 0;
}

/* Design the compensation FIR for a cascade by weighted least squares:
 * the desired response is unit-magnitude with phase such that
 * filter + cascade has linear phase with total delay d_target (output
 * samples); d_pipe is the cascade's pipeline latency, which needs no
 * signal-processing correction.  Returns the predicted residual GD
 * flatness (peak-peak, output samples) and stores the introduced ripple
 * in *ripple_db; -1 if d_target is too small to be reached causally with
 * L taps. */
static double design_comp(cascade_t const * c, int L, double const * freqs,
    double const * ph_total, double d_cascade, double d_pipe, double d_target,
    int K, comp_t * comp, double * ripple_db)
{
  double fs_in = c->stages[0].irate;
  double fs_out = c->stages[c->num_stages - 1].orate;
  double d_comp = (d_target - d_cascade) * fs_in / fs_out; /* Input samples. */
  double d_cont = d_target - d_pipe;   /* Content-domain target delay.     */
  int offset = (int)floor(d_comp - (L - 1) / 2.);
  double t[MAX_FIR_LEN], b[MAX_FIR_LEN], a[MAX_FIR_LEN * MAX_FIR_LEN];
  double php[SWEEP_PTS], ang[SWEEP_PTS], gd[SWEEP_PTS], mean, pp, rip = 0;
  int i, j, k, l, d;
  if (offset < 0) return -1;
  memset(t, 0, sizeof(t)); memset(b, 0, sizeof(b));
  for (k = 0; k < K; ++k) {
    double wi = 2 * M_PI * freqs[k] / fs_in, wo = 2 * M_PI * freqs[k] / fs_out;
    double theta = -wo * d_cont - ph_total[k] + wi * offset;
    for (d = 0; d < L; ++d) t[d] += cos(wi * d);
    for (l = 0; l < L; ++l) b[l] += cos(theta + wi * l);
  }
  t[0] *= 1 + 1e-6;                           /* Tikhonov regularisation. */
  for (i = 0; i < L; ++i)
    for (j = 0; j < L; ++j)
      a[i*L+j] = t[abs(i - j)];
  if (solve_sym(a, b, L)) return -1;
  memcpy(comp->h, b, (size_t)L * sizeof(*b));
  comp->len = L; comp->offset = offset;
  for (k = 0; k < K; ++k) {                   /* Predicted total response: */
    double wi = 2 * M_PI * freqs[k] / fs_in, re = 0, im = 0;
    for (l = 0; l < L; ++l)
      { re += comp->h[l] * cos(wi * l); im -= comp->h[l] * sin(wi * l); }
    ang[k] = atan2(im, re);
    if (k > 1 && k < K - 2) {
      double r = fabs(20 * log10(sqrt(re * re + im * im)));
      if (r > rip) rip = r;
    }
  }
  unwrap(ang, K);
  for (k = 0; k < K; ++k)
    php[k] = ph_total[k] - 2 * M_PI * freqs[k] / fs_in * offset + ang[k];
  gd_from_phase(php, freqs, K, fs_out, gd);
  gd_stats(gd, K, 2, &mean, &pp);
  *ripple_db = rip;
  return pp;
}

int main(int argc, char * argv[])
{
  double d_target_opt = argc > 1? atof(argv[1]) : -1;
  static double freqs[SWEEP_PTS];
  static double amp0[NUM_CONFIGS][SWEEP_PTS], amp1[NUM_CONFIGS][SWEEP_PTS];
  static double ph0[NUM_CONFIGS][SWEEP_PTS], ph1[NUM_CONFIGS][SWEEP_PTS];
  static double pht0[NUM_CONFIGS][SWEEP_PTS], pht1[NUM_CONFIGS][SWEEP_PTS];
  static double gd0[NUM_CONFIGS][SWEEP_PTS], gd1[NUM_CONFIGS][SWEEP_PTS];
  static comp_t comps[NUM_CONFIGS];
  static int lens[NUM_CONFIGS];
  double d_casc[NUM_CONFIGS], d_pipe[NUM_CONFIGS], d_gd[NUM_CONFIGS];
  double flat0[NUM_CONFIGS], fs_out = 0, d_target;
  double mean1[NUM_CONFIGS], flat1[NUM_CONFIGS];
  double max_d_casc = 0, overhead;
  static int const l_cand[] = {16, 24, 32, 48, 64, 96};
  int c, k, li, fails = 0;
  USE_STD_STDIO;

  for (c = 0; c < NUM_CONFIGS; ++c) {   /* Common sweep grid (all end at */
    double fo = configs[c].stages[configs[c].num_stages-1].orate; /* 44k1) */
    if (!fs_out) fs_out = fo;
    if (fo != fs_out) { fprintf(stderr, "configs must share fs_out\n"); return 2; }
  }
  for (k = 0; k < SWEEP_PTS; ++k)
    freqs[k] = (F_LO + (F_HI - F_LO) * k / (SWEEP_PTS - 1.)) * fs_out;

  /* Pass 1: measure each cascade, choose the shortest adequate FIR length. */
  for (c = 0; c < NUM_CONFIGS; ++c) {
    cascade_t const * cc = &configs[c];
    double fi = cc->stages[0].irate, fo = cc->stages[cc->num_stages-1].orate;
    double d_ref = impulse_delay(cc), mean, pp;
    measure(cc, 0, freqs, SWEEP_PTS, amp0[c], ph0[c]);
    total_phase(ph0[c], freqs, SWEEP_PTS, fo, d_ref, pht0[c]);
    gd_from_phase(pht0[c], freqs, SWEEP_PTS, fo, gd0[c]);
    gd_stats(gd0[c], SWEEP_PTS, 2, &mean, &pp);
    d_pipe[c] = pipe_latency(cc);      /* Total cascade delay = pipeline  */
    d_gd[c] = mean;                    /* latency + content group delay.  */
    d_casc[c] = d_pipe[c] + mean;
    flat0[c] = pp;
    if (d_casc[c] > max_d_casc) max_d_casc = d_casc[c];
    for (li = 0; li < (int)AL(l_cand); ++li) {  /* Smallest L that is      */
      comp_t tmp; double rip;                   /* predicted to meet spec. */
      double prov = d_casc[c] + ((l_cand[li] - 1) / 2. + 1) * fo / fi;
      pp = design_comp(cc, l_cand[li], freqs, pht0[c], d_casc[c], d_pipe[c], prov,
          SWEEP_PTS, &tmp, &rip);
      if (getenv("GD_DEBUG"))
        fprintf(stderr, "cfg%d L=%d prov=%.1f pred_pp=%.4f rip=%.5f (pipe=%.1f gd=%.2f flat0=%.4f)\n",
            c, l_cand[li], prov, pp, rip, d_pipe[c], mean, flat0[c]);
      if (pp >= 0 && pp < FLAT_LIMIT / 2 && rip < RIPPLE_LIMIT / 2) break;
    }
    if (li == (int)AL(l_cand)) { fprintf(stderr, "no adequate FIR length\n"); return 2; }
    lens[c] = l_cand[li];
  }

  /* Common alignment target: smallest delay reachable by all configs. */
  d_target = d_target_opt;
  if (d_target < 0) {
    d_target = 0;
    for (c = 0; c < NUM_CONFIGS; ++c) {
      cascade_t const * cc = &configs[c];
      double fi = cc->stages[0].irate, fo = cc->stages[cc->num_stages-1].orate;
      double need = d_casc[c] + ((lens[c] - 1) / 2. + 1) * fo / fi;
      if (need > d_target) d_target = need;
    }
    d_target = ceil(d_target);
  }
  for (c = 0; c < NUM_CONFIGS; ++c) {   /* Validate causality up front. */
    cascade_t const * cc = &configs[c];
    double fi = cc->stages[0].irate, fo = cc->stages[cc->num_stages-1].orate;
    if (d_target < d_casc[c] + ((lens[c] - 1) / 2.) * fo / fi) {
      fprintf(stderr, "%s: target delay %.0f too small for causal compensation"
          " (needs >= %.1f)\n", cc->name, d_target,
          d_casc[c] + ((lens[c] - 1) / 2.) * fo / fi);
      return 2;
    }
  }

  /* Pass 2: final designs, verification sweep, overhead measurements. */
  printf("target (residual) delay: %.0f output samples%s\n\n", d_target,
      d_target_opt >= 0? " (user-specified)" : " (minimum reachable)");
  printf("%-27s | %9s %8s %7s | %4s %6s | %9s %8s %8s | %9s %7s\n",
      "configuration", "pipe", "GD", "GD", "L", "offset",
      "res.delay", "res.flat", "ripple", "added", "cpu");
  printf("%-27s | %9s %8s %7s | %4s %6s | %9s %8s %8s | %9s %7s\n",
      "", "latency", "mean", "p-p", "", "",
      "= target", "(p-p)", "(dB)", "delay", "(info)");
  for (c = 0; c < NUM_CONFIGS; ++c) {
    cascade_t const * cc = &configs[c];
    double fo = cc->stages[cc->num_stages-1].orate;
    double rip_pred, pp_pred, t0, t_casc = 1e300, t_fir = 1e300;
    size_t n = 1 << 22, m = 0, i;
    double * x, * y;
    int r;
    pp_pred = design_comp(cc, lens[c], freqs, pht0[c], d_casc[c], d_pipe[c], d_target,
        SWEEP_PTS, &comps[c], &rip_pred);
    if (pp_pred < 0) {
      fprintf(stderr, "%s: target delay %.0f too small for causal compensation\n",
          cc->name, d_target);
      return 2;
    }
    measure(cc, &comps[c], freqs, SWEEP_PTS, amp1[c], ph1[c]);
    total_phase(ph1[c], freqs, SWEEP_PTS, fo, d_target - d_pipe[c], pht1[c]);
    gd_from_phase(pht1[c], freqs, SWEEP_PTS, fo, gd1[c]);
    gd_stats(gd1[c], SWEEP_PTS, 2, &mean1[c], &flat1[c]);
    { double ripple = 0;
      for (k = 2; k < SWEEP_PTS - 2; ++k) {
        double rr = fabs(20 * log10(amp1[c][k] / amp0[c][k]));
        if (rr > ripple) ripple = rr;
      }
    x = malloc(sizeof(*x) * n);                       /* Overhead timing. */
    for (i = 0; i < n; ++i) x[i] = sin(2 * M_PI * .1 * (double)i) + .3 * sin(2 * M_PI * .23 * (double)i);
    for (r = 0; r < 3; ++r) {
      t0 = now_s(); y = run_cascade(cc, 0, x, n, &m); free(y);
      t_casc = min(t_casc, now_s() - t0);
      t0 = now_s(); y = apply_comp(&comps[c], x, n); free(y);
      t_fir = min(t_fir, now_s() - t0);
    }
    free(x);
    mean1[c] += d_pipe[c];             /* Total residual delay vs target. */
    printf("%-27s | %9.2f %8.4f %7.4f | %4d %6d | %9.4f %8.4f %8.5f | %9.2f %6.1f%%\n",
        cc->name, d_pipe[c], d_gd[c], flat0[c], lens[c], comps[c].offset,
        mean1[c], flat1[c], ripple, d_target - d_casc[c],
        100 * t_fir / t_casc);
    fails += flat1[c] >= FLAT_LIMIT || ripple >= RIPPLE_LIMIT;
    }
  }
  { double mn = d_casc[0], mx = d_casc[0], mn1 = mean1[0], mx1 = mean1[0];
    for (c = 1; c < NUM_CONFIGS; ++c) {
      mn = min(mn, d_casc[c]); mx = max(mx, d_casc[c]);
      mn1 = min(mn1, mean1[c]); mx1 = max(mx1, mean1[c]);
    }
    printf("\nmisalignment before: %.2f samples (spread of cascade delays)\n",
        mx - mn);
    printf("misalignment after:  %.4f samples (spread of residual delays)\n",
        mx1 - mn1);
  }
  overhead = 100 * (d_target - max_d_casc) / max_d_casc;
  printf("added-latency overhead: %.2f%% of the slowest cascade", overhead);
  if (d_target_opt >= 0)
    printf(" (user-specified target; informational only)\n");
  else {
    printf(" (limit %.0f%%)\n", OH_LIMIT);
    fails += overhead >= OH_LIMIT;
  }
  printf("\nnotes: the integer `offset' part of each compensation is a pure\n"
      "re-alignment (zero run-time cost); only the short FIR touches the\n"
      "signal.  The cpu column gives the FIR's cost relative to the cascade\n"
      "for this scalar reference implementation; in production the few FIR\n"
      "taps would be folded into the resampler's polyphase coefficients at\n"
      "~zero marginal cost.\n");
  printf("\ncriteria: residual GD flatness < %.1f sample, ripple < %.2f dB, "
      "latency overhead < %.0f%%\n", FLAT_LIMIT, RIPPLE_LIMIT, OH_LIMIT);
  printf("result: %s\n", fails? "FAIL" : "PASS");
  return !!fails;
}
