/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Test 2: dither & noise-shaping --
 *   . quantisation to 8/16/24/32 bits (within int16/int32 containers);
 *   . injected seed gives sample-identical output (incl. across soxr_clear);
 *   . 1st/2nd-order noise-shaping reduces in-band noise by at least 6 dB. */

#ifdef NDEBUG /* N.B. assert used with active statements so enable always. */
#undef NDEBUG /* Must undef above assert.h or other that might include it. */
#endif

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <soxr.h>

#define RATE 48000
#define PI 3.14159265358979323846

static uint32_t ran_state = 1;
static double fran(void) /* Uniform in [-1, 1). */
{
  ran_state = 1664525u * ran_state + 1013904223u;
  return (double)(ran_state >> 8) * (1. / 8388608.) - 1.;
}

/* Resample (1:1) `n' float samples to an integer output type, with the given
 * dither configuration; returns the output sample count. */
static size_t run(float const * in, size_t n, void * out, size_t olen,
    soxr_datatype_t otype, unsigned bits, unsigned shaping,
    unsigned long const * seed, unsigned long io_flags, unsigned ch)
{
  soxr_error_t error;
  soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, otype);
  soxr_t soxr;
  size_t idone, odone;
  io_spec.flags = io_flags;
  soxr = soxr_create(RATE, RATE, ch, &error, &io_spec, NULL, NULL);
  assert(!error);
  if (bits) {
    error = soxr_set_dither(soxr, bits, shaping, seed);
    assert(!error);
  }
  error = soxr_process(soxr, in, n, &idone, out, olen, &odone);
  assert(!error);
  assert(idone == n);
  soxr_delete(soxr);
  return odone;
}

static void test_determinism(void)
{
  enum {N = 4096, CH = 2};
  static float in[N * CH];
  static int16_t out1[N * CH], out2[N * CH], out3[N * CH];
  unsigned long seed = 12345, other_seed = 54321;
  size_t i, odone1, odone2, odone3;

  for (i = 0; i < N * CH; ++i)
    in[i] = (float)(.5 * fran());

  odone1 = run(in, N, out1, N, SOXR_INT16_I, 16, SOXR_NOISE_SHAPING_SECOND,
      &seed, 0, CH);
  odone2 = run(in, N, out2, N, SOXR_INT16_I, 16, SOXR_NOISE_SHAPING_SECOND,
      &seed, 0, CH);
  assert(odone1 == odone2);
  assert(!memcmp(out1, out2, odone1 * CH * sizeof(*out1))); /* Same seed. */

  odone3 = run(in, N, out3, N, SOXR_INT16_I, 16, SOXR_NOISE_SHAPING_SECOND,
      &other_seed, 0, CH);
  assert(odone3 == odone1);
  assert(memcmp(out1, out3, odone1 * CH * sizeof(*out1))); /* Diff. seed. */

  { /* Sample-identical output also across soxr_clear: */
    soxr_error_t error;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t soxr = soxr_create(RATE, RATE, CH, &error, &io_spec, NULL, NULL);
    size_t idone, odone;
    assert(!error);
    error = soxr_set_dither(soxr, 16, SOXR_NOISE_SHAPING_SECOND, &seed);
    assert(!error);
    error = soxr_process(soxr, in, N, &idone, out3, N, &odone);
    assert(!error && odone == odone1);
    error = soxr_clear(soxr);
    assert(!error);
    error = soxr_process(soxr, in, N, &idone, out3, N, &odone);
    assert(!error && odone == odone1);
    assert(!memcmp(out1, out3, odone1 * CH * sizeof(*out1)));
    soxr_delete(soxr);
  }

  { /* Split output buffers give the same per-channel results as
       interleaved (and are likewise deterministic): */
    soxr_error_t error;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_S);
    soxr_t soxr = soxr_create(RATE, RATE, CH, &error, &io_spec, NULL, NULL);
    static int16_t split[CH][N];
    soxr_buf_t outs[CH];
    size_t idone, odone;
    unsigned c;
    assert(!error);
    error = soxr_set_dither(soxr, 16, SOXR_NOISE_SHAPING_SECOND, &seed);
    assert(!error);
    outs[0] = split[0];
    outs[1] = split[1];
    error = soxr_process(soxr, in, N, &idone, outs, N, &odone);
    assert(!error && odone == odone1);
    for (c = 0; c < CH; ++c)
      for (i = 0; i < odone; ++i)
        assert(split[c][i] == out1[i * CH + c]);
    soxr_delete(soxr);
  }
  fprintf(stderr, "determinism: ok\n");
}

static void test_bit_depths(void)
{
  enum {N = 2048};
  static float in[N];
  static int16_t out16[N];
  static int32_t out32[N];
  static const struct {
    soxr_datatype_t otype; unsigned bits; uint32_t mask;
  } configs[] = {
    {SOXR_INT16_I,  8, 0xffu},
    {SOXR_INT16_I, 16, 0x00u},
    {SOXR_INT32_I,  8, 0xffffffu},
    {SOXR_INT32_I, 16, 0xffffu},
    {SOXR_INT32_I, 24, 0xffu},
    {SOXR_INT32_I, 32, 0x00u},
  };
  unsigned long seed = 999;
  size_t i, j, odone;

  for (i = 0; i < N; ++i)
    in[i] = (float)(.9 * fran());

  for (j = 0; j < sizeof(configs) / sizeof(*configs); ++j) {
    unsigned bits = configs[j].bits;
    uint32_t mask = configs[j].mask;
    int int32 = (configs[j].otype & 3) == SOXR_INT32;
    odone = run(in, N, int32? (void *)out32 : (void *)out16, N,
        configs[j].otype, bits, SOXR_NOISE_SHAPING_FIRST, &seed, 0, 1);
    assert(odone >= N - 16); /* Allow for filter tail. */
    for (i = 0; i < odone; ++i) /* Quantised: LSbs of the container are 0. */
      assert(!(int32? ((uint32_t)out32[i] & mask) : ((uint32_t)(uint16_t)out16[i] & mask)));
  }

  { /* Invalid configurations are reported: */
    soxr_error_t error;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t soxr = soxr_create(RATE, RATE, 1, &error, &io_spec, NULL, NULL);
    assert(!error);
    assert(soxr_set_dither(soxr, 12, 0, NULL) != 0); /* Bad bit-depth. */
    assert(soxr_set_dither(soxr, 24, 0, NULL) != 0); /* Exceeds container. */
    assert(soxr_set_dither(soxr, 16, 3, NULL) != 0); /* Bad shaping order. */
    assert(soxr_set_dither(soxr, 16, 2, NULL) == 0);
    soxr_delete(soxr);
    io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr = soxr_create(RATE, RATE, 1, &error, &io_spec, NULL, NULL);
    assert(!error);
    assert(soxr_set_dither(soxr, 16, 0, NULL) != 0); /* Not integer output. */
    soxr_delete(soxr);
  }
  fprintf(stderr, "bit-depths: ok\n");
}

static void test_dither_active(void)
{
  enum {N = 1000};
  static float in[N];
  static int16_t out[N];
  unsigned long seed = 7;
  size_t i;
  int varies;

  for (i = 0; i < N; ++i)
    in[i] = (float)(.3 / 32768.); /* Sub-LSB DC. */

  run(in, N, out, N, SOXR_INT16_I, 16, SOXR_NOISE_SHAPING_NONE, &seed, 0, 1);
  for (varies = 0, i = 1; i < N; ++i)
    varies |= out[i] != out[0];
  assert(varies); /* Dithered: quantised value varies. */

  run(in, N, out, N, SOXR_INT16_I, 0, 0, NULL, SOXR_NO_DITHER, 1);
  for (varies = 0, i = 1; i < N; ++i)
    varies |= out[i] != out[0];
  assert(!varies); /* Undithered: constant. */
  fprintf(stderr, "dither-active: ok\n");
}

/* In-band noise power of `x' (length n), over DFT bins [1, n/8], excluding
 * the signal bin k0 (and its immediate neighbours). */
static double in_band_noise(int16_t const * x, size_t n, unsigned k0)
{
  double noise = 0;
  unsigned k;
  size_t t;
  for (k = 1; k <= n / 8; ++k) {
    double re = 0, im = 0;
    if (k + 1 >= k0 && k <= k0 + 1)
      continue;
    for (t = 0; t < n; ++t) {
      double a = 2 * PI * (double)k * (double)t / (double)n;
      re += x[t] * cos(a);
      im -= x[t] * sin(a);
    }
    noise += re * re + im * im;
  }
  return noise;
}

static void test_noise_shaping(void)
{
  enum {N = 4096, K0 = 32}; /* Signal at bin K0: 375 Hz @ 48 kHz. */
  static float in[N * 4];
  static int16_t out[N * 4];
  static const unsigned shapings[] =
      {SOXR_NOISE_SHAPING_FIRST, SOXR_NOISE_SHAPING_SECOND};
  unsigned long seed = 42;
  double noise_none;
  size_t i, j, odone, skip;

  /* A -48 dBFS sine, exactly on DFT bin K0 (no windowing needed): */
  for (i = 0; i < N * 4; ++i)
    in[i] = (float)(pow(10., -48. / 20.) * sin(2 * PI * K0 * (double)i / N));

  /* Measure with no shaping first: */
  {
    soxr_error_t error;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t soxr = soxr_create(RATE, RATE, 1, &error, &io_spec, NULL, NULL);
    size_t idone;
    assert(!error);
    assert(soxr_set_dither(soxr, 16, SOXR_NOISE_SHAPING_NONE, &seed) == 0);
    skip = (size_t)soxr_delay(soxr) + 512;
    error = soxr_process(soxr, in, N * 4, &idone, out, N * 4, &odone);
    assert(!error && odone >= skip + N);
    soxr_delete(soxr);
  }
  noise_none = in_band_noise(out + skip, N, K0);
  assert(noise_none > 0);

  for (j = 0; j < sizeof(shapings) / sizeof(*shapings); ++j) {
    double noise_shaped, reduction_db;
    odone = run(in, N * 4, out, N * 4, SOXR_INT16_I, 16, shapings[j],
        &seed, 0, 1);
    assert(odone >= skip + N);
    noise_shaped = in_band_noise(out + skip, N, K0);
    reduction_db = 10 * log10(noise_none / noise_shaped);
    fprintf(stderr, "shaping order %u: in-band noise reduced by %.1f dB\n",
        shapings[j], reduction_db);
    assert(reduction_db >= 6);
  }
}

int main(int argc, char const * arg[])
{
  test_determinism();
  test_bit_depths();
  test_dither_active();
  test_noise_shaping();
  fprintf(stderr, "ok\n");
  return 0 * argc * !arg;
}
