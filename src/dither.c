/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dither.h"
#include "internal.h"



typedef struct {         /* Per-channel state: */
  uint32_t ran;          /*   PRNG state (fixed 32-bit => portable results). */
  double e1, e2;         /*   Quantisation-error feedback history. */
} channel_state_t;



struct soxr_dither {
  soxr_datatype_t otype;
  unsigned shaping;
  unsigned num_channels;
  double step;           /* Quantiser step, in output-container LSBs. */
  double inv_step;
  double qmin, qmax;     /* Container clip limits, in units of `step'. */
  double a1, a2;         /* Error-feedback coefficients. */
  channel_state_t * channels;
};



soxr_dither_t * _soxr_dither_create(unsigned num_channels,
    soxr_datatype_t otype, unsigned bits, unsigned shaping, unsigned long seed)
{
  static double const a[3][2] = {{0, 0}, {1, 0}, {2, -1}}; /* H = 1-a1/z-a2/z² */
  unsigned container_bits, i;
  soxr_dither_t * d;

  otype = (soxr_datatype_t)(otype & 3);
  if (otype != SOXR_INT16 && otype != SOXR_INT32)
    return 0;
  container_bits = otype == SOXR_INT16? 16 : 32;
  if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
      bits > container_bits || shaping > 2 || !num_channels)
    return 0;
  if (!(d = calloc(1, sizeof(*d))))
    return 0;
  if (!(d->channels = calloc(num_channels, sizeof(*d->channels)))) {
    free(d);
    return 0;
  }
  d->otype = otype;
  d->shaping = shaping;
  d->num_channels = num_channels;
  d->step = (double)(1UL << (container_bits - bits));
  d->inv_step = 1 / d->step;
  d->qmax = (container_bits == 16? 32767. : 2147483647.) / d->step;
  d->qmin = (container_bits == 16? -32768. : -2147483648.) / d->step;
  d->a1 = a[shaping][0];
  d->a2 = a[shaping][1];
  for (i = 0; i < num_channels; ++i) /* Decorrelate the channels' sequences. */
    d->channels[i].ran = (uint32_t)seed + 0x9E3779B9u * (i + 1);
  return d;
}



void _soxr_dither_delete(soxr_dither_t * d)
{
  if (d) {
    free(d->channels);
    free(d);
  }
}



static __inline double tpdf(uint32_t * s) /* Triangular PDF, range (-1, 1). */
{
  double a;
  *s = *s * 1664525u + 1013904223u;
  a = (double)(*s >> 8);
  *s = *s * 1664525u + 1013904223u;
  return (a - (double)(*s >> 8)) * (1. / 16777216.);
}



static __inline int32_t quantise(channel_state_t * c,
    soxr_dither_t const * d, double x, size_t * clips)
{
  double const dither = d->step * tpdf(&c->ran);
  double u = x + dither, q, y;
  if (d->shaping) {
    u += d->a1 * c->e1;
    if (d->shaping > 1)
      u += d->a2 * c->e2;
  }
  q = floor(u * d->inv_step + .5);
  if (q > d->qmax)
    q = d->qmax, ++*clips;
  else if (q < d->qmin)
    q = d->qmin, ++*clips;
  y = q * d->step;
  c->e2 = c->e1;
  c->e1 = u - dither - y;  /* Feedback excludes the dither, so that the
                              dither itself is noise-shaped as well. */
  return (int32_t)y;
}



#define DITHER_INTERLEAVE(FNAME, FLOATX) \
size_t FNAME(soxr_dither_t * d, unsigned ch0, \
    void * * dest0, FLOATX const * const * src, size_t n, unsigned ch) \
{ \
  size_t clips = 0, i; \
  unsigned j; \
  if (d->otype == SOXR_INT16) { \
    int16_t * dest = *(int16_t * *)dest0; \
    for (j = 0; j < ch; ++j) { \
      channel_state_t * c = &d->channels[ch0 + j]; \
      FLOATX const * s = src[j]; \
      int16_t * dst = dest + j; \
      for (i = 0; i < n; ++i) \
        dst[i * ch] = (int16_t)quantise(c, d, (double)s[i], &clips); \
    } \
    *dest0 = dest + n * ch; \
  } else { \
    int32_t * dest = *(int32_t * *)dest0; \
    for (j = 0; j < ch; ++j) { \
      channel_state_t * c = &d->channels[ch0 + j]; \
      FLOATX const * s = src[j]; \
      int32_t * dst = dest + j; \
      for (i = 0; i < n; ++i) \
        dst[i * ch] = quantise(c, d, (double)s[i], &clips); \
    } \
    *dest0 = dest + n * ch; \
  } \
  return clips; \
}

DITHER_INTERLEAVE(_soxr_dither_interleave, double)
DITHER_INTERLEAVE(_soxr_dither_interleave_f, float)
