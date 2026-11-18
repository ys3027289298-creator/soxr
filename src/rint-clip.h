/* SoX Resampler Library      Copyright (c) 2007-16 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

#define DO_16 _;_;_;_;_;_;_;_;_;_;_;_;_;_;_;_



#if defined FE_INVALID && defined FPU_RINT
static void RINT_CLIP(RINT_T * const dest, FLOATX const * const src,
    unsigned stride, size_t i, size_t const n, size_t * const clips)
{
  for (; i < n; ++i) {
    FLOATX const d = src[i];
    RINT(dest[stride * i], d);
    if (fe_test_invalid()) {
      fe_clear_invalid();
      dest[stride * i] = d > 0? RINT_MAX : -RINT_MAX - 1;
      ++*clips;
    }
  }
}
#endif



static size_t LSX_RINT_CLIP(void * * const dest0, FLOATX const * const src,
    size_t const n)
{
  size_t i, clips = 0;
  RINT_T * dest = *dest0;
#if defined FE_INVALID && defined FPU_RINT
#define _ RINT(dest[i], src[i]); ++i
  for (i = 0; i < (n & ~15u);) {
    DO_16;
    if (fe_test_invalid()) {
      fe_clear_invalid();
      RINT_CLIP(dest, src, 1, i - 16, i, &clips);
    }
  }
  RINT_CLIP(dest, src, 1, i, n, &clips);
#else
#define _ d = src[i], dest[i++] = (RINT_T)(d > 0? \
    d+.5 >= N? ++clips, N-1 : d+.5 : d-.5 <= -N-1? ++clips, -N:d-.5)
  const double N = 1. + RINT_MAX;
  double d;
  for (i = 0; i < (n & ~15u);)
    DO_16;
  for (; i < n; _);
#endif
  *dest0 = dest + n;
  return clips;
}
#undef _



static size_t LSX_RINT_CLIP_2(void * * dest0, FLOATX const * const * srcs,
    unsigned const stride, size_t const n)
{
  unsigned j;
  size_t i, clips = 0;
  RINT_T * dest = *dest0;
#if defined FE_INVALID && defined FPU_RINT
#define _ RINT(dest[stride * i], src[i]); ++i
  for (j = 0; j < stride; ++j, ++dest) {
    FLOATX const * const src = srcs[j];
    for (i = 0; i < (n & ~15u);) {
      DO_16;
      if (fe_test_invalid()) {
        fe_clear_invalid();
        RINT_CLIP(dest, src, stride, i - 16, i, &clips);
      }
    }
    RINT_CLIP(dest, src, stride, i, n, &clips);
  }
#else
#define _ d = src[i], dest[stride * i++] = (RINT_T)(d > 0? \
    d+.5 >= N? ++clips, N-1 : d+.5 : d-.5 <= -N-1? ++clips, -N:d-.5)
  const double N = 1. + RINT_MAX;
  double d;
  for (j = 0; j < stride; ++j, ++dest) {
    FLOATX const * const src = srcs[j];
    for (i = 0; i < (n & ~15u);)
      DO_16;
    for (; i < n; _);
  }
#endif
  *dest0 = dest + stride * (n - 1);
  return clips;
}
#undef _

#undef RINT_MAX
#undef RINT_T
#undef FPU_RINT
#undef RINT
#undef RINT_CLIP
#undef LSX_RINT_CLIP
#undef LSX_RINT_CLIP_2
