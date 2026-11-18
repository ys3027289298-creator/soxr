/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

#if !defined soxr_dither_included
#define soxr_dither_included

#include "soxr.h"
#include "std-types.h"

/* Dither & noise-shaping engine, used when converting resampled output to an
 * integer datatype.  Supports quantisation to 8, 16, 24 or 32 bits (within
 * the int16/int32 output container), TPDF dither, and 1st/2nd-order noise
 * shaping by error feedback.  Given the same seed, output is deterministic
 * (sample-identical across runs and platforms). */

typedef struct soxr_dither soxr_dither_t;

soxr_dither_t * _soxr_dither_create(
    unsigned num_channels,
    soxr_datatype_t otype,   /* SOXR_INT16 or SOXR_INT32. */
    unsigned bits,           /* Quantisation bit-depth: 8, 16, 24 or 32. */
    unsigned shaping,        /* 0 = none, 1 = 1st-order, 2 = 2nd-order. */
    unsigned long seed);

void _soxr_dither_delete(soxr_dither_t *);

/* The following return the number of clipped samples, and are called only
 * from _soxr_interleave / _soxr_interleave_f.  `ch0' is the index (into the
 * engine's per-channel state) of the first of the `ch' channels. */

size_t _soxr_dither_interleave(soxr_dither_t *, unsigned ch0,
    void * * dest, double const * const * src, size_t n, unsigned ch);

size_t _soxr_dither_interleave_f(soxr_dither_t *, unsigned ch0,
    void * * dest, float const * const * src, size_t n, unsigned ch);

#endif
