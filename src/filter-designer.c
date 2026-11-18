/* SoX Resampler Library
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Anti-aliasing low-pass FIR filter designer.
 *
 * Given a pass-band edge, a stop-band start and a stop-band attenuation,
 * designs a Kaiser-windowed sinc filter using the library's own design
 * functions, measures the actual frequency response of the result, can
 * report the trade-offs of shorter filter lengths, and can export the
 * design as a reusable (and re-verifiable) filter description. */

#include "filter.h"
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORMAT_ID "soxr-fir-1"
#define ATT_TOL_DB 1.0     /* Required agreement: measured vs design. */
#define DESIGN_TIME_LIMIT .5 /* seconds */

#if WITH_DEV_TRACE /* Debug trace, normally provided by soxr.c: */
#include <stdarg.h>
int _soxr_trace_level;
void _soxr_trace(char const * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
}
#endif

typedef struct {
  double fp, fs, fn;   /* Pass-band end, stop-band start, Nyquist. */
  double att;          /* Stop-band attenuation target, dB. */
  double beta;         /* Kaiser beta actually used. */
  int num_taps;
  double * h;          /* Filter coefficients. */
  double measured_att; /* Measured stop-band attenuation, dB. */
  double pb_ripple;    /* Measured pass-band ripple, dB peak-to-peak. */
  double design_secs;
} filter_t;

static void measure(filter_t * f)
{
  int len = 4096, k, k_pb, k_sb;
  double * d, dc, max_sb = 0, min_pb = HUGE_VAL, max_pb = 0;

  while (len < 8 * f->num_taps)
    len <<= 1;
  d = calloc((size_t)len, sizeof(*d));
  if (!d) {
    fprintf(stderr, "filter-designer: out of memory\n");
    exit(2);
  }
  memcpy(d, f->h, (size_t)f->num_taps * sizeof(*d));
  lsx_safe_rdft(len, 1, d);

  k_pb = (int)(f->fp / f->fn * (len >> 1) + .5); /* Last pass-band bin. */
  k_sb = (int)ceil(f->fs / f->fn * (len >> 1));  /* First stop-band bin. */
  dc = fabs(d[0]);
  for (k = 1; k <= len >> 1; ++k) {
    double mag = k == len >> 1? fabs(d[1]) : hypot(d[2 * k], d[2 * k + 1]);
    double r = mag / dc;
    if (k <= k_pb) {
      if (r > max_pb) max_pb = r;
      if (r < min_pb) min_pb = r;
    } else if (k >= k_sb && r > max_sb)
      max_sb = r;
  }
  free(d);
  f->measured_att = -20 * log10(max_sb);
  f->pb_ripple = 20 * log10(max_pb / min_pb);
}

static int design(filter_t * f, int num_taps, double beta)
{
  clock_t t0;
  int n = num_taps;
  double b = beta, tr_bw, fc;

  free(f->h), f->h = 0;
  t0 = clock();
  f->h = lsx_design_lpf(f->fp, f->fs, f->fn, f->att, &n, 1, b);
  f->design_secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  if (!f->h)
    return 1;
  f->num_taps = n;

  /* Recover the beta that lsx_design_lpf selected (phases = 1): */
  if (beta >= 0)
    f->beta = beta;
  else {
    tr_bw = .5 * (f->fs - f->fp) / f->fn;
    fc = f->fs / f->fn - tr_bw;
    tr_bw = min(tr_bw, .5 * f->fs / f->fn);
    n = 0, b = -1;
    lsx_kaiser_params(f->att, fc, tr_bw, &b, &n);
    f->beta = b;
  }
  measure(f);
  return 0;
}

static void report(filter_t const * f, double rate)
{
  double mult = rate? .5 * rate : f->fn;
  printf("design: fp=%g fs=%g att=%gdB (nyquist=%g%s) -> taps=%d beta=%.4f\n",
      f->fp * mult / f->fn, f->fs * mult / f->fn, f->att,
      f->fn * mult / f->fn, rate? " Hz" : "", f->num_taps, f->beta);
  printf("  measured: stop-band=%.2fdB (target %.1f, error %+.2fdB)"
      " pass-band-ripple=%.4fdB delay=%.1f samples design-time=%.1fms\n",
      f->measured_att, f->att, f->measured_att - f->att, f->pb_ripple,
      .5 * (f->num_taps - 1), 1e3 * f->design_secs);
}

static int estimated_taps(filter_t const * f, double att)
{
  double tr_bw = .5 * (f->fs - f->fp) / f->fn, fc = f->fs / f->fn - tr_bw;
  double b = -1;
  int n = 0;
  tr_bw = min(tr_bw, .5 * f->fs / f->fn);
  lsx_kaiser_params(att, fc, tr_bw, &b, &n);
  return n;
}

static void report_tradeoffs(filter_t const * full)
{
  static double const fracs[] = {.85, .7, .55, .4};
  filter_t t = *full;
  size_t i;

  printf("shorter-length trade-offs (pass-band and transition unchanged):\n");
  printf("  taps  %%full  achievable stop-band att(dB)  shortfall vs %.0fdB"
      "  delay(samples)\n", full->att);
  printf("  %4d  100.0             %6.2f                0.00          %6.1f\n",
      full->num_taps, full->measured_att, .5 * (full->num_taps - 1));
  t.h = 0;
  for (i = 0; i < sizeof fracs / sizeof *fracs; ++i) {
    int target = max(3, (int)(full->num_taps * fracs[i] + .5)), iter;
    double lo = 1, hi = 300;
    for (iter = 0; iter < 40; ++iter) { /* Attenuation that `target' taps */
      double mid = .5 * (lo + hi);      /* can actually support:          */
      if (estimated_taps(full, mid) > target) hi = mid; else lo = mid;
    }
    t.att = .5 * (lo + hi);
    if (design(&t, 0, -1))
      break;
    printf("  %4d  %5.1f             %6.2f             %6.2f          %6.1f\n",
        t.num_taps, 100. * t.num_taps / full->num_taps, t.measured_att,
        full->att - t.measured_att, .5 * (t.num_taps - 1));
  }
  free(t.h);
}

static int export_txt(filter_t const * f, char const * path, double rate)
{
  int i;
  FILE * out = fopen(path, "w");
  if (!out)
    return 1;
  fprintf(out, "# soxr anti-aliasing filter description, designed by filter-designer\n");
  fprintf(out, "format          %s\n", FORMAT_ID);
  fprintf(out, "sample_rate     %.17g  # Hz; 0 => normalised frequencies\n", rate);
  fprintf(out, "nyquist         %.17g  # units of passband_edge/stopband_start\n", f->fn);
  fprintf(out, "passband_edge   %.17g\n", f->fp);
  fprintf(out, "stopband_start  %.17g\n", f->fs);
  fprintf(out, "stopband_att_db %.17g  # design target\n", f->att);
  fprintf(out, "kaiser_beta     %.17g\n", f->beta);
  fprintf(out, "num_taps        %d\n", f->num_taps);
  fprintf(out, "measured_stopband_att_db %.6f\n", f->measured_att);
  fprintf(out, "passband_ripple_db       %.6f\n", f->pb_ripple);
  fprintf(out, "coefficients\n");
  for (i = 0; i < f->num_taps; ++i)
    fprintf(out, "%.17g\n", f->h[i]);
  fprintf(out, "end\n");
  return fclose(out) != 0;
}

static int export_c(filter_t const * f, char const * path, double rate)
{
  int i;
  FILE * out = fopen(path, "w");
  if (!out)
    return 1;
  fprintf(out, "/* Anti-aliasing low-pass FIR filter (Kaiser window design).\n");
  fprintf(out, " * Designed by filter-designer (soxr).\n");
  fprintf(out, " * pass-band edge  : %.9g x nyquist", f->fp / f->fn);
  if (rate) fprintf(out, " (%.9g Hz @ %.9g Hz sample rate)", f->fp / f->fn * .5 * rate, rate);
  fprintf(out, "\n * stop-band start : %.9g x nyquist\n", f->fs / f->fn);
  fprintf(out, " * stop-band att.  : %.1f dB design, %.2f dB measured\n",
      f->att, f->measured_att);
  fprintf(out, " * kaiser beta     : %.6f\n", f->beta);
  fprintf(out, " * group delay     : %.1f samples\n */\n", .5 * (f->num_taps - 1));
  fprintf(out, "#define FILTER_NUM_TAPS %d\n", f->num_taps);
  fprintf(out, "static double const filter_coefs[FILTER_NUM_TAPS] = {\n ");
  for (i = 0; i < f->num_taps; ++i)
    fprintf(out, " %.17g,%c", f->h[i], i % 4 == 3 && i + 1 < f->num_taps? '\n' : ' ');
  fprintf(out,"\n};\n");
  return fclose(out) != 0;
}

static int verify(char const * path)
{
  char tok[64];
  double fp = 0, fs = 0, fn = 1, att = 0, file_measured = 0;
  int num_taps = 0, i, ret = 1;
  filter_t f;
  FILE * in = fopen(path, "r");
  if (!in)
    return 1;
  memset(&f, 0, sizeof f);
#define READ(x) do {if (fscanf(in, "%lf", &(x)) != 1) goto done;} while (0)
  while (fscanf(in, "%63s", tok) == 1) {
    if (!strcmp(tok, "#")) {                 /* Comment: skip to end of line. */
      int c; while ((c = getc(in)) != '\n' && c != EOF);
    } else if (!strcmp(tok, "passband_edge"))  READ(fp);
    else if (!strcmp(tok, "stopband_start"))   READ(fs);
    else if (!strcmp(tok, "nyquist"))          READ(fn);
    else if (!strcmp(tok, "stopband_att_db"))  READ(att);
    else if (!strcmp(tok, "measured_stopband_att_db")) READ(file_measured);
    else if (!strcmp(tok, "num_taps")) {
      if (fscanf(in, "%d", &num_taps) != 1) goto done;
    }
    else if (!strcmp(tok, "coefficients"))
      break;
  }
#undef READ
  f.h = malloc((size_t)num_taps * sizeof(*f.h));
  if (!f.h)
    goto done;
  for (i = 0; i < num_taps && fscanf(in, "%lf", &f.h[i]) == 1; ++i);
  if (i != num_taps) {
    fprintf(stderr, "filter-designer: %s: expected %d coefficients, got %d\n",
        path, num_taps, i);
    goto done;
  }
  f.fp = fp, f.fs = fs, f.fn = fn, f.att = att, f.num_taps = num_taps;
  measure(&f);
  printf("verify: %s: re-measured stop-band=%.2fdB (file claims %.2fdB,"
      " design target %.1fdB)\n", path, f.measured_att, file_measured, att);
  if (fabs(f.measured_att - file_measured) > .1)
    fprintf(stderr, "filter-designer: FAIL: description does not reproduce\n");
  else if (fabs(f.measured_att - att) > ATT_TOL_DB)
    fprintf(stderr, "filter-designer: FAIL: >%gdB from design target\n", ATT_TOL_DB);
  else {
    printf("filter-designer: PASS\n");
    ret = 0;
  }
done:
  free(f.h);
  fclose(in);
  return ret;
}

static void usage(FILE * out)
{
  fprintf(out,
"Usage: filter-designer --fp F --fs F --att DB [options]\n"
"Design an anti-aliasing low-pass FIR filter (Kaiser window).\n"
"  --fp F        pass-band edge   (fraction of nyquist, or Hz with --rate)\n"
"  --fs F        stop-band start  (same units as --fp)\n"
"  --att DB      stop-band attenuation target, dB\n"
"  --fn F        nyquist frequency (default 1)\n"
"  --rate R      sample rate in Hz; --fp/--fs are then in Hz\n"
"  --taps N      force filter length (default 0 = auto)\n"
"  --beta B      force Kaiser beta (default -1 = auto)\n");
  fprintf(out,
"  --tradeoffs   report shorter-length trade-offs\n"
"  --out FILE    export reusable filter description\n"
"  --format FMT  export format: txt (default) | c\n"
"  --verify FILE re-measure an exported description and check it\n"
"  --check       fail unless |measured-design| <= %gdB and design time <= %gs\n"
"  --quiet       suppress design report\n",
      ATT_TOL_DB, DESIGN_TIME_LIMIT);
}

int main(int argc, char ** argv)
{
  filter_t f;
  double beta = -1, rate = 0;
  int num_taps = 0, tradeoffs = 0, check = 0, quiet = 0, i, ret = 0;
  char const * out_path = 0, * format = "txt", * verify_path = 0;

  memset(&f, 0, sizeof f);
  f.fn = 1;
  for (i = 1; i < argc; ++i) {
    char const * a = argv[i];
    double v = i + 1 < argc? atof(argv[i + 1]) : 0;
    if (!strcmp(a, "--fp"))        f.fp = v, ++i;
    else if (!strcmp(a, "--fs"))   f.fs = v, ++i;
    else if (!strcmp(a, "--att"))  f.att = v, ++i;
    else if (!strcmp(a, "--fn"))   f.fn = v, ++i;
    else if (!strcmp(a, "--rate")) rate = v, ++i;
    else if (!strcmp(a, "--taps")) num_taps = (int)v, ++i;
    else if (!strcmp(a, "--beta")) beta = v, ++i;
    else if (!strcmp(a, "--tradeoffs")) tradeoffs = 1;
    else if (!strcmp(a, "--check"))     check = 1;
    else if (!strcmp(a, "--quiet"))     quiet = 1;
    else if (!strcmp(a, "--out"))    out_path = argv[++i];
    else if (!strcmp(a, "--format")) format = argv[++i];
    else if (!strcmp(a, "--verify")) verify_path = argv[++i];
    else if (!strcmp(a, "--help")) { usage(stdout); return 0; }
    else {
      fprintf(stderr, "filter-designer: unknown option `%s'\n", a);
      usage(stderr);
      return 2;
    }
  }
  if (verify_path && !out_path)
    return verify(verify_path);
  if (rate > 0)
    f.fn = .5 * rate;
  if (!(f.fp > 0 && f.fs > f.fp && f.fs <= f.fn && f.att > 0)) {
    fprintf(stderr, "filter-designer: need 0 < fp < fs <= nyquist, att > 0\n");
    usage(stderr);
    return 2;
  }

  if (design(&f, num_taps, beta))
    return 2;
  if (!quiet)
    report(&f, rate);
  if (tradeoffs)
    report_tradeoffs(&f);
  if (out_path) {
    int err = !strcmp(format, "c")? export_c(&f, out_path, rate) :
      export_txt(&f, out_path, rate);
    if (err) {
      fprintf(stderr, "filter-designer: cannot write `%s'\n", out_path);
      ret = 2;
    } else if (!quiet)
      printf("exported: %s (%s format)\n", out_path, format);
  }
  if (verify_path && verify(verify_path))
    ret = 1;
  if (check) {
    double err_db = fabs(f.measured_att - f.att);
    if (err_db > ATT_TOL_DB) {
      fprintf(stderr, "filter-designer: FAIL: measured %.2fdB is %.2fdB from"
          " design %.1fdB (limit %gdB)\n",
          f.measured_att, err_db, f.att, ATT_TOL_DB);
      ret = 1;
    } else if (f.design_secs > DESIGN_TIME_LIMIT) {
      fprintf(stderr, "filter-designer: FAIL: design took %.3fs (limit %gs)\n",
          f.design_secs, DESIGN_TIME_LIMIT);
      ret = 1;
    } else if (!quiet)
      printf("filter-designer: PASS (error %.2fdB <= %gdB, design %.1fms <= %gms)\n",
          err_db, ATT_TOL_DB, 1e3 * f.design_secs, 1e3 * DESIGN_TIME_LIMIT);
  }
  free(f.h);
  return ret;
}
