/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Test that resampling a stream in consecutive segments -- saving the
 * resampler state at each segment boundary and restoring it into a fresh
 * resampler for the next segment -- gives output that is sample-for-sample
 * identical to resampling the whole stream in one go (i.e. no artefacts at
 * the segment joins). */

#include <soxr.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 1;
static uint32_t rng(void) /* Deterministic pseudo-random, for chunk sizes. */
{
  return rng_state = rng_state * 1664525u + 1013904223u;
}

typedef struct { /* Appendable buffer of interleaved samples. */
  float * data;
  size_t len, cap;
} buf_t;

static void append(buf_t * b, float const * src, size_t n)
{
  if (b->len + n > b->cap) {
    b->cap = (b->len + n) * 2;
    b->data = realloc(b->data, b->cap * sizeof(*b->data));
    if (!b->data) {fprintf(stderr, "out of memory\n"); exit(1);}
  }
  memcpy(b->data + b->len, src, n * sizeof(*src));
  b->len += n;
}

static void fatal(char const * what, soxr_error_t error)
{
  fprintf(stderr, "%s: %s\n", what, soxr_strerror(error));
  exit(1);
}

/* Save the state of `old', delete it, and return an equivalent resampler
 * restored from the saved state. */
static soxr_t save_delete_restore(soxr_t old)
{
  soxr_error_t error;
  soxr_t fresh;
  size_t len = soxr_state_size(old);
  void * buf;
  if (!len)
    fatal("soxr_state_size", soxr_error(old));
  buf = malloc(len);
  if ((error = soxr_save_state(old, buf, len)))
    fatal("soxr_save_state", error);
  fresh = soxr_restore_state(buf, len, &error);
  if (!fresh)
    fatal("soxr_restore_state", error);
  free(buf);
  soxr_delete(old);
  return fresh;
}

static void feed(soxr_t s, float const * in, size_t n, unsigned ch,
    float * obuf, size_t olen, buf_t * out)
{
  size_t done = 0;
  while (done < n) {
    size_t idone = 0, odone = 0;
    soxr_error_t error = soxr_process(s,
        in + done * ch, n - done, &idone, obuf, olen, &odone);
    if (error)
      fatal("soxr_process", error);
    append(out, obuf, odone * ch);
    done += idone;
    if (!idone && !odone)
      fatal("soxr_process", "stalled");
  }
}

static void drain(soxr_t s, unsigned ch, float * obuf, size_t olen, buf_t * out)
{
  for (;;) {
    size_t odone = 0;
    soxr_error_t error = soxr_process(s, NULL, 0, NULL, obuf, olen, &odone);
    if (error)
      fatal("soxr_process (flush)", error);
    append(out, obuf, odone * ch);
    if (!odone)
      break;
  }
}

/* Resample `in' (n samples per channel) in one call, as the reference: */
static buf_t resample_oneshot(double irate, double orate, unsigned ch,
    float const * in, size_t n, unsigned long q_recipe)
{
  soxr_quality_spec_t q_spec = soxr_quality_spec(q_recipe, 0);
  size_t olen = (size_t)(n * orate / irate) + 4096, idone = 0, odone = 0;
  buf_t out = {0, 0, 0};
  soxr_error_t error;
  out.data = malloc(olen * ch * sizeof(*out.data));
  out.len = out.cap = 0;
  error = soxr_oneshot(irate, orate, ch, in, n, &idone,
      out.data, olen, &odone, NULL, &q_spec, NULL);
  if (error)
    fatal("soxr_oneshot", error);
  if (idone != n)
    fatal("soxr_oneshot", "did not consume all input");
  out.len = odone * ch;
  return out;
}

/* Resample `in' in pseudo-randomly sized segments; `n_cycles' times, save
 * the state, delete the resampler and restore a fresh one from the state.
 * If `cycle_in_flush', do one more save/restore part-way through flushing. */
static buf_t resample_segmented(double irate, double orate, unsigned ch,
    float const * in, size_t n, int n_cycles, int cycle_in_flush,
    unsigned long q_recipe)
{
  soxr_error_t error;
  soxr_quality_spec_t q_spec = soxr_quality_spec(q_recipe, 0);
  soxr_t s = soxr_create(irate, orate, ch, &error, NULL, &q_spec, NULL);
  size_t done = 0, olen = 1024 + rng() % 4096;
  float * obuf;
  buf_t out = {0, 0, 0};
  int cycle = 0;
  if (!s)
    fatal("soxr_create", error);
  obuf = malloc(olen * ch * sizeof(*obuf));
  while (done < n) {
    size_t chunk = 1 + rng() % 5000;
    if (chunk > n - done)
      chunk = n - done;
    feed(s, in + done * ch, chunk, ch, obuf, olen, &out);
    done += chunk;
    if (cycle < n_cycles && done >= n * (size_t)(cycle + 1) / (size_t)(n_cycles + 1))
      s = save_delete_restore(s), ++cycle;
  }
  if (cycle_in_flush) { /* Start flushing, then save/restore mid-flush: */
    size_t odone = 0;
    if ((error = soxr_process(s, NULL, 0, NULL, obuf, olen, &odone)))
      fatal("soxr_process (flush)", error);
    append(&out, obuf, odone * ch);
    s = save_delete_restore(s);
  }
  drain(s, ch, obuf, olen, &out);
  free(obuf);
  soxr_delete(s);
  return out;
}

static int compare(char const * what, buf_t const * a, buf_t const * b)
{
  size_t i, n = a->len < b->len? a->len : b->len;
  if (a->len == b->len && !memcmp(a->data, b->data, a->len * sizeof(float))) {
    printf("%-46s OK (%lu samples)\n", what, (unsigned long)a->len);
    return 0;
  }
  for (i = 0; i < n && a->data[i] == b->data[i]; ++i);
  printf("%-46s FAIL: first diff at sample %lu (%g vs %g), lengths %lu vs %lu\n",
      what, (unsigned long)i,
      i < a->len? a->data[i] : 0., i < b->len? b->data[i] : 0.,
      (unsigned long)a->len, (unsigned long)b->len);
  return 1;
}

static int test_rate_pair(double irate, double orate, unsigned ch,
    unsigned long q_recipe, char const * q_name)
{
  size_t const n = 48000;
  float * in = malloc(n * ch * sizeof(*in));
  size_t i;
  unsigned c;
  int fails = 0;
  char what[128];
  buf_t ref, out;

  for (i = 0; i < n; ++i) /* Sines + noise, full-scale-ish, per channel: */
    for (c = 0; c < ch; ++c)
      in[i * ch + c] = (float)(
          .5 * sin(2 * M_PI * (440. + 110 * c) * i / irate) +
          .2 * sin(2 * M_PI *  997. * i / irate + 1.) +
          .05 * ((rng() >> 8) * (1 / 16777216.) - .5));

  ref = resample_oneshot(irate, orate, ch, in, n, q_recipe);

  rng_state = 7;
  out = resample_segmented(irate, orate, ch, in, n, 0, 0, q_recipe);
  sprintf(what, "%g->%g %uch %s segmented (no save/restore)",
      irate, orate, ch, q_name);
  fails += compare(what, &ref, &out);
  free(out.data);

  rng_state = 7;
  out = resample_segmented(irate, orate, ch, in, n, 3, 0, q_recipe);
  sprintf(what, "%g->%g %uch %s segmented, 3x save/restore",
      irate, orate, ch, q_name);
  fails += compare(what, &ref, &out);
  free(out.data);

  rng_state = 7;
  out = resample_segmented(irate, orate, ch, in, n, 2, 1, q_recipe);
  sprintf(what, "%g->%g %uch %s segmented, save/restore in flush",
      irate, orate, ch, q_name);
  fails += compare(what, &ref, &out);
  free(out.data);

  free(ref.data);
  free(in);
  return fails;
}

static int test_api_errors(void)
{
  soxr_error_t error;
  soxr_t s = soxr_create(48000, 44100, 1, &error, NULL, NULL, NULL);
  size_t len;
  void * buf;
  int fails = 0;
  if (!s)
    fatal("soxr_create", error);
  len = soxr_state_size(s);
  buf = malloc(len);

  if (soxr_save_state(s, buf, len - 1) == 0)
    ++fails, printf("%-46s FAIL: expected error\n", "save into too-small buffer");
  else printf("%-46s OK\n", "save into too-small buffer rejected");

  if (soxr_save_state(s, buf, len) != 0)
    ++fails, printf("%-46s FAIL\n", "save into exact-size buffer");
  else printf("%-46s OK\n", "save into exact-size buffer");

  if (soxr_restore_state(buf, len - 1, &error) != 0)
    ++fails, printf("%-46s FAIL: expected error\n", "restore truncated state");
  else printf("%-46s OK\n", "restore truncated state rejected");

  ((unsigned char *)buf)[0] ^= 0xff; /* Corrupt the magic. */
  if (soxr_restore_state(buf, len, &error) != 0)
    ++fails, printf("%-46s FAIL: expected error\n", "restore corrupt state");
  else printf("%-46s OK\n", "restore corrupt state rejected");

  free(buf);
  soxr_delete(s);

  { /* Variable-rate engine does not support state save/restore: */
    soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_HQ, SOXR_VR);
    s = soxr_create(48000, 44100, 1, &error, NULL, &q_spec, NULL);
    if (!s)
      fatal("soxr_create (vr)", error);
    if (soxr_state_size(s) != 0 || soxr_save_state(s, buf = malloc(4096), 4096) == 0)
      ++fails, printf("%-46s FAIL: expected error\n", "vr engine state save rejected");
    else printf("%-46s OK\n", "vr engine state save rejected");
    free(buf);
    soxr_delete(s);
  }
  return fails;
}

int main(void)
{
  int fails = 0;
  fails += test_rate_pair(48000, 44100, 2, SOXR_HQ, "HQ");
  fails += test_rate_pair(44100, 48000, 2, SOXR_HQ, "HQ");
  fails += test_rate_pair(48000, 44100, 1, SOXR_HQ, "HQ");
  fails += test_rate_pair(48000, 44100, 2, SOXR_VHQ, "VHQ");
  fails += test_rate_pair(44100, 48000, 2, SOXR_VHQ, "VHQ");
  fails += test_rate_pair(48000, 32000, 2, SOXR_MQ, "MQ");
  fails += test_api_errors();
  if (fails) {
    printf("FAILED (%i)\n", fails);
    return 1;
  }
  printf("All save/restore streaming tests passed.\n");
  return 0;
}
