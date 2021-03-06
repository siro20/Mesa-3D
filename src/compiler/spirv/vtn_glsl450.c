/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include <math.h>

#include "nir/nir_builtin_builder.h"

#include "vtn_private.h"
#include "GLSL.std.450.h"

#define M_PIf   ((float) M_PI)
#define M_PI_2f ((float) M_PI_2)
#define M_PI_4f ((float) M_PI_4)

static nir_ssa_def *
build_mat2_det(nir_builder *b, nir_ssa_def *col[2])
{
   unsigned swiz[2] = {1, 0 };
   nir_ssa_def *p = nir_fmul(b, col[0], nir_swizzle(b, col[1], swiz, 2, true));
   return nir_fsub(b, nir_channel(b, p, 0), nir_channel(b, p, 1));
}

static nir_ssa_def *
build_mat3_det(nir_builder *b, nir_ssa_def *col[3])
{
   unsigned yzx[3] = {1, 2, 0 };
   unsigned zxy[3] = {2, 0, 1 };

   nir_ssa_def *prod0 =
      nir_fmul(b, col[0],
               nir_fmul(b, nir_swizzle(b, col[1], yzx, 3, true),
                           nir_swizzle(b, col[2], zxy, 3, true)));
   nir_ssa_def *prod1 =
      nir_fmul(b, col[0],
               nir_fmul(b, nir_swizzle(b, col[1], zxy, 3, true),
                           nir_swizzle(b, col[2], yzx, 3, true)));

   nir_ssa_def *diff = nir_fsub(b, prod0, prod1);

   return nir_fadd(b, nir_channel(b, diff, 0),
                      nir_fadd(b, nir_channel(b, diff, 1),
                                  nir_channel(b, diff, 2)));
}

static nir_ssa_def *
build_mat4_det(nir_builder *b, nir_ssa_def **col)
{
   nir_ssa_def *subdet[4];
   for (unsigned i = 0; i < 4; i++) {
      unsigned swiz[3];
      for (unsigned j = 0; j < 3; j++)
         swiz[j] = j + (j >= i);

      nir_ssa_def *subcol[3];
      subcol[0] = nir_swizzle(b, col[1], swiz, 3, true);
      subcol[1] = nir_swizzle(b, col[2], swiz, 3, true);
      subcol[2] = nir_swizzle(b, col[3], swiz, 3, true);

      subdet[i] = build_mat3_det(b, subcol);
   }

   nir_ssa_def *prod = nir_fmul(b, col[0], nir_vec(b, subdet, 4));

   return nir_fadd(b, nir_fsub(b, nir_channel(b, prod, 0),
                                  nir_channel(b, prod, 1)),
                      nir_fsub(b, nir_channel(b, prod, 2),
                                  nir_channel(b, prod, 3)));
}

static nir_ssa_def *
build_mat_det(struct vtn_builder *b, struct vtn_ssa_value *src)
{
   unsigned size = glsl_get_vector_elements(src->type);

   nir_ssa_def *cols[4];
   for (unsigned i = 0; i < size; i++)
      cols[i] = src->elems[i]->def;

   switch(size) {
   case 2: return build_mat2_det(&b->nb, cols);
   case 3: return build_mat3_det(&b->nb, cols);
   case 4: return build_mat4_det(&b->nb, cols);
   default:
      vtn_fail("Invalid matrix size");
   }
}

/* Computes the determinate of the submatrix given by taking src and
 * removing the specified row and column.
 */
static nir_ssa_def *
build_mat_subdet(struct nir_builder *b, struct vtn_ssa_value *src,
                 unsigned size, unsigned row, unsigned col)
{
   assert(row < size && col < size);
   if (size == 2) {
      return nir_channel(b, src->elems[1 - col]->def, 1 - row);
   } else {
      /* Swizzle to get all but the specified row */
      unsigned swiz[3];
      for (unsigned j = 0; j < 3; j++)
         swiz[j] = j + (j >= row);

      /* Grab all but the specified column */
      nir_ssa_def *subcol[3];
      for (unsigned j = 0; j < size; j++) {
         if (j != col) {
            subcol[j - (j > col)] = nir_swizzle(b, src->elems[j]->def,
                                                swiz, size - 1, true);
         }
      }

      if (size == 3) {
         return build_mat2_det(b, subcol);
      } else {
         assert(size == 4);
         return build_mat3_det(b, subcol);
      }
   }
}

static struct vtn_ssa_value *
matrix_inverse(struct vtn_builder *b, struct vtn_ssa_value *src)
{
   nir_ssa_def *adj_col[4];
   unsigned size = glsl_get_vector_elements(src->type);

   /* Build up an adjugate matrix */
   for (unsigned c = 0; c < size; c++) {
      nir_ssa_def *elem[4];
      for (unsigned r = 0; r < size; r++) {
         elem[r] = build_mat_subdet(&b->nb, src, size, c, r);

         if ((r + c) % 2)
            elem[r] = nir_fneg(&b->nb, elem[r]);
      }

      adj_col[c] = nir_vec(&b->nb, elem, size);
   }

   nir_ssa_def *det_inv = nir_frcp(&b->nb, build_mat_det(b, src));

   struct vtn_ssa_value *val = vtn_create_ssa_value(b, src->type);
   for (unsigned i = 0; i < size; i++)
      val->elems[i]->def = nir_fmul(&b->nb, adj_col[i], det_inv);

   return val;
}

/**
 * Return e^x.
 */
static nir_ssa_def *
build_exp(nir_builder *b, nir_ssa_def *x)
{
   return nir_fexp2(b, nir_fmul_imm(b, x, M_LOG2E));
}

/**
 * Return ln(x) - the natural logarithm of x.
 */
static nir_ssa_def *
build_log(nir_builder *b, nir_ssa_def *x)
{
   return nir_fmul_imm(b, nir_flog2(b, x), 1.0 / M_LOG2E);
}

/**
 * Approximate asin(x) by the formula:
 *    asin~(x) = sign(x) * (pi/2 - sqrt(1 - |x|) * (pi/2 + |x|(pi/4 - 1 + |x|(p0 + |x|p1))))
 *
 * which is correct to first order at x=0 and x=±1 regardless of the p
 * coefficients but can be made second-order correct at both ends by selecting
 * the fit coefficients appropriately.  Different p coefficients can be used
 * in the asin and acos implementation to minimize some relative error metric
 * in each case.
 */
static nir_ssa_def *
build_asin(nir_builder *b, nir_ssa_def *x, float p0, float p1)
{
   if (x->bit_size == 16) {
      /* The polynomial approximation isn't precise enough to meet half-float
       * precision requirements. Alternatively, we could implement this using
       * the formula:
       *
       * asin(x) = atan2(x, sqrt(1 - x*x))
       *
       * But that is very expensive, so instead we just do the polynomial
       * approximation in 32-bit math and then we convert the result back to
       * 16-bit.
       */
      return nir_f2f16(b, build_asin(b, nir_f2f32(b, x), p0, p1));
   }

   nir_ssa_def *one = nir_imm_floatN_t(b, 1.0f, x->bit_size);
   nir_ssa_def *abs_x = nir_fabs(b, x);

   nir_ssa_def *p0_plus_xp1 = nir_fadd_imm(b, nir_fmul_imm(b, abs_x, p1), p0);

   nir_ssa_def *expr_tail =
      nir_fadd_imm(b, nir_fmul(b, abs_x,
                                  nir_fadd_imm(b, nir_fmul(b, abs_x,
                                                               p0_plus_xp1),
                                                  M_PI_4f - 1.0f)),
                      M_PI_2f);

   return nir_fmul(b, nir_fsign(b, x),
                      nir_fsub(b, nir_imm_floatN_t(b, M_PI_2f, x->bit_size),
                                  nir_fmul(b, nir_fsqrt(b, nir_fsub(b, one, abs_x)),
                                                           expr_tail)));
}

/**
 * Compute xs[0] + xs[1] + xs[2] + ... using fadd.
 */
static nir_ssa_def *
build_fsum(nir_builder *b, nir_ssa_def **xs, int terms)
{
   nir_ssa_def *accum = xs[0];

   for (int i = 1; i < terms; i++)
      accum = nir_fadd(b, accum, xs[i]);

   return accum;
}

static nir_ssa_def *
build_atan(nir_builder *b, nir_ssa_def *y_over_x)
{
   const uint32_t bit_size = y_over_x->bit_size;

   nir_ssa_def *abs_y_over_x = nir_fabs(b, y_over_x);
   nir_ssa_def *one = nir_imm_floatN_t(b, 1.0f, bit_size);

   /*
    * range-reduction, first step:
    *
    *      / y_over_x         if |y_over_x| <= 1.0;
    * x = <
    *      \ 1.0 / y_over_x   otherwise
    */
   nir_ssa_def *x = nir_fdiv(b, nir_fmin(b, abs_y_over_x, one),
                                nir_fmax(b, abs_y_over_x, one));

   /*
    * approximate atan by evaluating polynomial:
    *
    * x   * 0.9999793128310355 - x^3  * 0.3326756418091246 +
    * x^5 * 0.1938924977115610 - x^7  * 0.1173503194786851 +
    * x^9 * 0.0536813784310406 - x^11 * 0.0121323213173444
    */
   nir_ssa_def *x_2  = nir_fmul(b, x,   x);
   nir_ssa_def *x_3  = nir_fmul(b, x_2, x);
   nir_ssa_def *x_5  = nir_fmul(b, x_3, x_2);
   nir_ssa_def *x_7  = nir_fmul(b, x_5, x_2);
   nir_ssa_def *x_9  = nir_fmul(b, x_7, x_2);
   nir_ssa_def *x_11 = nir_fmul(b, x_9, x_2);

   nir_ssa_def *polynomial_terms[] = {
      nir_fmul_imm(b, x,     0.9999793128310355f),
      nir_fmul_imm(b, x_3,  -0.3326756418091246f),
      nir_fmul_imm(b, x_5,   0.1938924977115610f),
      nir_fmul_imm(b, x_7,  -0.1173503194786851f),
      nir_fmul_imm(b, x_9,   0.0536813784310406f),
      nir_fmul_imm(b, x_11, -0.0121323213173444f),
   };

   nir_ssa_def *tmp =
      build_fsum(b, polynomial_terms, ARRAY_SIZE(polynomial_terms));

   /* range-reduction fixup */
   tmp = nir_fadd(b, tmp,
                  nir_fmul(b, nir_b2f(b, nir_flt(b, one, abs_y_over_x), bit_size),
                           nir_fadd_imm(b, nir_fmul_imm(b, tmp, -2.0f), M_PI_2f)));

   /* sign fixup */
   return nir_fmul(b, tmp, nir_fsign(b, y_over_x));
}

static nir_ssa_def *
build_atan2(nir_builder *b, nir_ssa_def *y, nir_ssa_def *x)
{
   assert(y->bit_size == x->bit_size);
   const uint32_t bit_size = x->bit_size;

   nir_ssa_def *zero = nir_imm_floatN_t(b, 0, bit_size);
   nir_ssa_def *one = nir_imm_floatN_t(b, 1, bit_size);

   /* If we're on the left half-plane rotate the coordinates π/2 clock-wise
    * for the y=0 discontinuity to end up aligned with the vertical
    * discontinuity of atan(s/t) along t=0.  This also makes sure that we
    * don't attempt to divide by zero along the vertical line, which may give
    * unspecified results on non-GLSL 4.1-capable hardware.
    */
   nir_ssa_def *flip = nir_fge(b, zero, x);
   nir_ssa_def *s = nir_bcsel(b, flip, nir_fabs(b, x), y);
   nir_ssa_def *t = nir_bcsel(b, flip, y, nir_fabs(b, x));

   /* If the magnitude of the denominator exceeds some huge value, scale down
    * the arguments in order to prevent the reciprocal operation from flushing
    * its result to zero, which would cause precision problems, and for s
    * infinite would cause us to return a NaN instead of the correct finite
    * value.
    *
    * If fmin and fmax are respectively the smallest and largest positive
    * normalized floating point values representable by the implementation,
    * the constants below should be in agreement with:
    *
    *    huge <= 1 / fmin
    *    scale <= 1 / fmin / fmax (for |t| >= huge)
    *
    * In addition scale should be a negative power of two in order to avoid
    * loss of precision.  The values chosen below should work for most usual
    * floating point representations with at least the dynamic range of ATI's
    * 24-bit representation.
    */
   const double huge_val = bit_size >= 32 ? 1e18 : 16384;
   nir_ssa_def *huge = nir_imm_floatN_t(b,  huge_val, bit_size);
   nir_ssa_def *scale = nir_bcsel(b, nir_fge(b, nir_fabs(b, t), huge),
                                  nir_imm_floatN_t(b, 0.25, bit_size), one);
   nir_ssa_def *rcp_scaled_t = nir_frcp(b, nir_fmul(b, t, scale));
   nir_ssa_def *s_over_t = nir_fmul(b, nir_fmul(b, s, scale), rcp_scaled_t);

   /* For |x| = |y| assume tan = 1 even if infinite (i.e. pretend momentarily
    * that ∞/∞ = 1) in order to comply with the rather artificial rules
    * inherited from IEEE 754-2008, namely:
    *
    *  "atan2(±∞, −∞) is ±3π/4
    *   atan2(±∞, +∞) is ±π/4"
    *
    * Note that this is inconsistent with the rules for the neighborhood of
    * zero that are based on iterated limits:
    *
    *  "atan2(±0, −0) is ±π
    *   atan2(±0, +0) is ±0"
    *
    * but GLSL specifically allows implementations to deviate from IEEE rules
    * at (0,0), so we take that license (i.e. pretend that 0/0 = 1 here as
    * well).
    */
   nir_ssa_def *tan = nir_bcsel(b, nir_feq(b, nir_fabs(b, x), nir_fabs(b, y)),
                                one, nir_fabs(b, s_over_t));

   /* Calculate the arctangent and fix up the result if we had flipped the
    * coordinate system.
    */
   nir_ssa_def *arc =
      nir_fadd(b, nir_fmul_imm(b, nir_b2f(b, flip, bit_size), M_PI_2f),
                  build_atan(b, tan));

   /* Rather convoluted calculation of the sign of the result.  When x < 0 we
    * cannot use fsign because we need to be able to distinguish between
    * negative and positive zero.  We don't use bitwise arithmetic tricks for
    * consistency with the GLSL front-end.  When x >= 0 rcp_scaled_t will
    * always be non-negative so this won't be able to distinguish between
    * negative and positive zero, but we don't care because atan2 is
    * continuous along the whole positive y = 0 half-line, so it won't affect
    * the result significantly.
    */
   return nir_bcsel(b, nir_flt(b, nir_fmin(b, y, rcp_scaled_t), zero),
                    nir_fneg(b, arc), arc);
}

static nir_ssa_def *
build_frexp16(nir_builder *b, nir_ssa_def *x, nir_ssa_def **exponent)
{
   assert(x->bit_size == 16);

   nir_ssa_def *abs_x = nir_fabs(b, x);
   nir_ssa_def *zero = nir_imm_floatN_t(b, 0, 16);

   /* Half-precision floating-point values are stored as
    *   1 sign bit;
    *   5 exponent bits;
    *   10 mantissa bits.
    *
    * An exponent shift of 10 will shift the mantissa out, leaving only the
    * exponent and sign bit (which itself may be zero, if the absolute value
    * was taken before the bitcast and shift).
    */
   nir_ssa_def *exponent_shift = nir_imm_int(b, 10);
   nir_ssa_def *exponent_bias = nir_imm_intN_t(b, -14, 16);

   nir_ssa_def *sign_mantissa_mask = nir_imm_intN_t(b, 0x83ffu, 16);

   /* Exponent of floating-point values in the range [0.5, 1.0). */
   nir_ssa_def *exponent_value = nir_imm_intN_t(b, 0x3800u, 16);

   nir_ssa_def *is_not_zero = nir_fne(b, abs_x, zero);

   /* Significand return must be of the same type as the input, but the
    * exponent must be a 32-bit integer.
    */
   *exponent =
      nir_i2i32(b,
                nir_iadd(b, nir_ushr(b, abs_x, exponent_shift),
                            nir_bcsel(b, is_not_zero, exponent_bias, zero)));

   return nir_ior(b, nir_iand(b, x, sign_mantissa_mask),
                     nir_bcsel(b, is_not_zero, exponent_value, zero));
}

static nir_ssa_def *
build_frexp32(nir_builder *b, nir_ssa_def *x, nir_ssa_def **exponent)
{
   nir_ssa_def *abs_x = nir_fabs(b, x);
   nir_ssa_def *zero = nir_imm_float(b, 0.0f);

   /* Single-precision floating-point values are stored as
    *   1 sign bit;
    *   8 exponent bits;
    *   23 mantissa bits.
    *
    * An exponent shift of 23 will shift the mantissa out, leaving only the
    * exponent and sign bit (which itself may be zero, if the absolute value
    * was taken before the bitcast and shift.
    */
   nir_ssa_def *exponent_shift = nir_imm_int(b, 23);
   nir_ssa_def *exponent_bias = nir_imm_int(b, -126);

   nir_ssa_def *sign_mantissa_mask = nir_imm_int(b, 0x807fffffu);

   /* Exponent of floating-point values in the range [0.5, 1.0). */
   nir_ssa_def *exponent_value = nir_imm_int(b, 0x3f000000u);

   nir_ssa_def *is_not_zero = nir_fne(b, abs_x, zero);

   *exponent =
      nir_iadd(b, nir_ushr(b, abs_x, exponent_shift),
                  nir_bcsel(b, is_not_zero, exponent_bias, zero));

   return nir_ior(b, nir_iand(b, x, sign_mantissa_mask),
                     nir_bcsel(b, is_not_zero, exponent_value, zero));
}

static nir_ssa_def *
build_frexp64(nir_builder *b, nir_ssa_def *x, nir_ssa_def **exponent)
{
   nir_ssa_def *abs_x = nir_fabs(b, x);
   nir_ssa_def *zero = nir_imm_double(b, 0.0);
   nir_ssa_def *zero32 = nir_imm_float(b, 0.0f);

   /* Double-precision floating-point values are stored as
    *   1 sign bit;
    *   11 exponent bits;
    *   52 mantissa bits.
    *
    * We only need to deal with the exponent so first we extract the upper 32
    * bits using nir_unpack_64_2x32_split_y.
    */
   nir_ssa_def *upper_x = nir_unpack_64_2x32_split_y(b, x);
   nir_ssa_def *abs_upper_x = nir_unpack_64_2x32_split_y(b, abs_x);

   /* An exponent shift of 20 will shift the remaining mantissa bits out,
    * leaving only the exponent and sign bit (which itself may be zero, if the
    * absolute value was taken before the bitcast and shift.
    */
   nir_ssa_def *exponent_shift = nir_imm_int(b, 20);
   nir_ssa_def *exponent_bias = nir_imm_int(b, -1022);

   nir_ssa_def *sign_mantissa_mask = nir_imm_int(b, 0x800fffffu);

   /* Exponent of floating-point values in the range [0.5, 1.0). */
   nir_ssa_def *exponent_value = nir_imm_int(b, 0x3fe00000u);

   nir_ssa_def *is_not_zero = nir_fne(b, abs_x, zero);

   *exponent =
      nir_iadd(b, nir_ushr(b, abs_upper_x, exponent_shift),
                  nir_bcsel(b, is_not_zero, exponent_bias, zero32));

   nir_ssa_def *new_upper =
      nir_ior(b, nir_iand(b, upper_x, sign_mantissa_mask),
                 nir_bcsel(b, is_not_zero, exponent_value, zero32));

   nir_ssa_def *lower_x = nir_unpack_64_2x32_split_x(b, x);

   return nir_pack_64_2x32_split(b, lower_x, new_upper);
}

static nir_op
vtn_nir_alu_op_for_spirv_glsl_opcode(struct vtn_builder *b,
                                     enum GLSLstd450 opcode)
{
   switch (opcode) {
   case GLSLstd450Round:         return nir_op_fround_even;
   case GLSLstd450RoundEven:     return nir_op_fround_even;
   case GLSLstd450Trunc:         return nir_op_ftrunc;
   case GLSLstd450FAbs:          return nir_op_fabs;
   case GLSLstd450SAbs:          return nir_op_iabs;
   case GLSLstd450FSign:         return nir_op_fsign;
   case GLSLstd450SSign:         return nir_op_isign;
   case GLSLstd450Floor:         return nir_op_ffloor;
   case GLSLstd450Ceil:          return nir_op_fceil;
   case GLSLstd450Fract:         return nir_op_ffract;
   case GLSLstd450Sin:           return nir_op_fsin;
   case GLSLstd450Cos:           return nir_op_fcos;
   case GLSLstd450Pow:           return nir_op_fpow;
   case GLSLstd450Exp2:          return nir_op_fexp2;
   case GLSLstd450Log2:          return nir_op_flog2;
   case GLSLstd450Sqrt:          return nir_op_fsqrt;
   case GLSLstd450InverseSqrt:   return nir_op_frsq;
   case GLSLstd450NMin:          return nir_op_fmin;
   case GLSLstd450FMin:          return nir_op_fmin;
   case GLSLstd450UMin:          return nir_op_umin;
   case GLSLstd450SMin:          return nir_op_imin;
   case GLSLstd450NMax:          return nir_op_fmax;
   case GLSLstd450FMax:          return nir_op_fmax;
   case GLSLstd450UMax:          return nir_op_umax;
   case GLSLstd450SMax:          return nir_op_imax;
   case GLSLstd450FMix:          return nir_op_flrp;
   case GLSLstd450Fma:           return nir_op_ffma;
   case GLSLstd450Ldexp:         return nir_op_ldexp;
   case GLSLstd450FindILsb:      return nir_op_find_lsb;
   case GLSLstd450FindSMsb:      return nir_op_ifind_msb;
   case GLSLstd450FindUMsb:      return nir_op_ufind_msb;

   /* Packing/Unpacking functions */
   case GLSLstd450PackSnorm4x8:     return nir_op_pack_snorm_4x8;
   case GLSLstd450PackUnorm4x8:     return nir_op_pack_unorm_4x8;
   case GLSLstd450PackSnorm2x16:    return nir_op_pack_snorm_2x16;
   case GLSLstd450PackUnorm2x16:    return nir_op_pack_unorm_2x16;
   case GLSLstd450PackHalf2x16:     return nir_op_pack_half_2x16;
   case GLSLstd450PackDouble2x32:   return nir_op_pack_64_2x32;
   case GLSLstd450UnpackSnorm4x8:   return nir_op_unpack_snorm_4x8;
   case GLSLstd450UnpackUnorm4x8:   return nir_op_unpack_unorm_4x8;
   case GLSLstd450UnpackSnorm2x16:  return nir_op_unpack_snorm_2x16;
   case GLSLstd450UnpackUnorm2x16:  return nir_op_unpack_unorm_2x16;
   case GLSLstd450UnpackHalf2x16:   return nir_op_unpack_half_2x16;
   case GLSLstd450UnpackDouble2x32: return nir_op_unpack_64_2x32;

   default:
      vtn_fail("No NIR equivalent");
   }
}

#define NIR_IMM_FP(n, v) (nir_imm_floatN_t(n, v, src[0]->bit_size))

static void
handle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,
                   const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);

   /* Collect the various SSA sources */
   unsigned num_inputs = count - 5;
   nir_ssa_def *src[3] = { NULL, };
   for (unsigned i = 0; i < num_inputs; i++) {
      /* These are handled specially below */
      if (vtn_untyped_value(b, w[i + 5])->value_type == vtn_value_type_pointer)
         continue;

      src[i] = vtn_ssa_value(b, w[i + 5])->def;
   }

   switch (entrypoint) {
   case GLSLstd450Radians:
      val->ssa->def = nir_radians(nb, src[0]);
      return;
   case GLSLstd450Degrees:
      val->ssa->def = nir_degrees(nb, src[0]);
      return;
   case GLSLstd450Tan:
      val->ssa->def = nir_fdiv(nb, nir_fsin(nb, src[0]),
                               nir_fcos(nb, src[0]));
      return;

   case GLSLstd450Modf: {
      nir_ssa_def *sign = nir_fsign(nb, src[0]);
      nir_ssa_def *abs = nir_fabs(nb, src[0]);
      val->ssa->def = nir_fmul(nb, sign, nir_ffract(nb, abs));
      nir_store_deref(nb, vtn_nir_deref(b, w[6]),
                      nir_fmul(nb, sign, nir_ffloor(nb, abs)), 0xf);
      return;
   }

   case GLSLstd450ModfStruct: {
      nir_ssa_def *sign = nir_fsign(nb, src[0]);
      nir_ssa_def *abs = nir_fabs(nb, src[0]);
      vtn_assert(glsl_type_is_struct(val->ssa->type));
      val->ssa->elems[0]->def = nir_fmul(nb, sign, nir_ffract(nb, abs));
      val->ssa->elems[1]->def = nir_fmul(nb, sign, nir_ffloor(nb, abs));
      return;
   }

   case GLSLstd450Step:
      val->ssa->def = nir_sge(nb, src[1], src[0]);
      return;

   case GLSLstd450Length:
      val->ssa->def = nir_fast_length(nb, src[0]);
      return;
   case GLSLstd450Distance:
      val->ssa->def = nir_fast_distance(nb, src[0], src[1]);
      return;
   case GLSLstd450Normalize:
      val->ssa->def = nir_fast_normalize(nb, src[0]);
      return;

   case GLSLstd450Exp:
      val->ssa->def = build_exp(nb, src[0]);
      return;

   case GLSLstd450Log:
      val->ssa->def = build_log(nb, src[0]);
      return;

   case GLSLstd450FClamp:
   case GLSLstd450NClamp:
      val->ssa->def = nir_fclamp(nb, src[0], src[1], src[2]);
      return;
   case GLSLstd450UClamp:
      val->ssa->def = nir_uclamp(nb, src[0], src[1], src[2]);
      return;
   case GLSLstd450SClamp:
      val->ssa->def = nir_iclamp(nb, src[0], src[1], src[2]);
      return;

   case GLSLstd450Cross: {
      val->ssa->def = nir_cross(nb, src[0], src[1]);
      return;
   }

   case GLSLstd450SmoothStep: {
      val->ssa->def = nir_smoothstep(nb, src[0], src[1], src[2]);
      return;
   }

   case GLSLstd450FaceForward:
      val->ssa->def =
         nir_bcsel(nb, nir_flt(nb, nir_fdot(nb, src[2], src[1]),
                                   NIR_IMM_FP(nb, 0.0)),
                       src[0], nir_fneg(nb, src[0]));
      return;

   case GLSLstd450Reflect:
      /* I - 2 * dot(N, I) * N */
      val->ssa->def =
         nir_fsub(nb, src[0], nir_fmul(nb, NIR_IMM_FP(nb, 2.0),
                              nir_fmul(nb, nir_fdot(nb, src[0], src[1]),
                                           src[1])));
      return;

   case GLSLstd450Refract: {
      nir_ssa_def *I = src[0];
      nir_ssa_def *N = src[1];
      nir_ssa_def *eta = src[2];
      nir_ssa_def *n_dot_i = nir_fdot(nb, N, I);
      nir_ssa_def *one = NIR_IMM_FP(nb, 1.0);
      nir_ssa_def *zero = NIR_IMM_FP(nb, 0.0);
      /* According to the SPIR-V and GLSL specs, eta is always a float
       * regardless of the type of the other operands. However in practice it
       * seems that if you try to pass it a float then glslang will just
       * promote it to a double and generate invalid SPIR-V. In order to
       * support a hypothetical fixed version of glslang we’ll promote eta to
       * double if the other operands are double also.
       */
      if (I->bit_size != eta->bit_size) {
         nir_op conversion_op =
            nir_type_conversion_op(nir_type_float | eta->bit_size,
                                   nir_type_float | I->bit_size,
                                   nir_rounding_mode_undef);
         eta = nir_build_alu(nb, conversion_op, eta, NULL, NULL, NULL);
      }
      /* k = 1.0 - eta * eta * (1.0 - dot(N, I) * dot(N, I)) */
      nir_ssa_def *k =
         nir_fsub(nb, one, nir_fmul(nb, eta, nir_fmul(nb, eta,
                      nir_fsub(nb, one, nir_fmul(nb, n_dot_i, n_dot_i)))));
      nir_ssa_def *result =
         nir_fsub(nb, nir_fmul(nb, eta, I),
                      nir_fmul(nb, nir_fadd(nb, nir_fmul(nb, eta, n_dot_i),
                                                nir_fsqrt(nb, k)), N));
      /* XXX: bcsel, or if statement? */
      val->ssa->def = nir_bcsel(nb, nir_flt(nb, k, zero), zero, result);
      return;
   }

   case GLSLstd450Sinh:
      /* 0.5 * (e^x - e^(-x)) */
      val->ssa->def =
         nir_fmul_imm(nb, nir_fsub(nb, build_exp(nb, src[0]),
                                       build_exp(nb, nir_fneg(nb, src[0]))),
                          0.5f);
      return;

   case GLSLstd450Cosh:
      /* 0.5 * (e^x + e^(-x)) */
      val->ssa->def =
         nir_fmul_imm(nb, nir_fadd(nb, build_exp(nb, src[0]),
                                       build_exp(nb, nir_fneg(nb, src[0]))),
                          0.5f);
      return;

   case GLSLstd450Tanh: {
      /* tanh(x) := (0.5 * (e^x - e^(-x))) / (0.5 * (e^x + e^(-x)))
       *
       * With a little algebra this reduces to (e^2x - 1) / (e^2x + 1)
       *
       * We clamp x to (-inf, +10] to avoid precision problems.  When x > 10,
       * e^2x is so much larger than 1.0 that 1.0 gets flushed to zero in the
       * computation e^2x +/- 1 so it can be ignored.
       *
       * For 16-bit precision we clamp x to (-inf, +4.2] since the maximum
       * representable number is only 65,504 and e^(2*6) exceeds that. Also,
       * if x > 4.2, tanh(x) will return 1.0 in fp16.
       */
      const uint32_t bit_size = src[0]->bit_size;
      const double clamped_x = bit_size > 16 ? 10.0 : 4.2;
      nir_ssa_def *x = nir_fmin(nb, src[0],
                                    nir_imm_floatN_t(nb, clamped_x, bit_size));
      nir_ssa_def *exp2x = build_exp(nb, nir_fmul_imm(nb, x, 2.0));
      val->ssa->def = nir_fdiv(nb, nir_fadd_imm(nb, exp2x, -1.0),
                                   nir_fadd_imm(nb, exp2x, 1.0));
      return;
   }

   case GLSLstd450Asinh:
      val->ssa->def = nir_fmul(nb, nir_fsign(nb, src[0]),
         build_log(nb, nir_fadd(nb, nir_fabs(nb, src[0]),
                       nir_fsqrt(nb, nir_fadd_imm(nb, nir_fmul(nb, src[0], src[0]),
                                                      1.0f)))));
      return;
   case GLSLstd450Acosh:
      val->ssa->def = build_log(nb, nir_fadd(nb, src[0],
         nir_fsqrt(nb, nir_fadd_imm(nb, nir_fmul(nb, src[0], src[0]),
                                        -1.0f))));
      return;
   case GLSLstd450Atanh: {
      nir_ssa_def *one = nir_imm_floatN_t(nb, 1.0, src[0]->bit_size);
      val->ssa->def =
         nir_fmul_imm(nb, build_log(nb, nir_fdiv(nb, nir_fadd(nb, src[0], one),
                                        nir_fsub(nb, one, src[0]))),
                          0.5f);
      return;
   }

   case GLSLstd450Asin:
      val->ssa->def = build_asin(nb, src[0], 0.086566724, -0.03102955);
      return;

   case GLSLstd450Acos:
      val->ssa->def =
         nir_fsub(nb, nir_imm_floatN_t(nb, M_PI_2f, src[0]->bit_size),
                      build_asin(nb, src[0], 0.08132463, -0.02363318));
      return;

   case GLSLstd450Atan:
      val->ssa->def = build_atan(nb, src[0]);
      return;

   case GLSLstd450Atan2:
      val->ssa->def = build_atan2(nb, src[0], src[1]);
      return;

   case GLSLstd450Frexp: {
      nir_ssa_def *exponent;
      if (src[0]->bit_size == 64)
         val->ssa->def = build_frexp64(nb, src[0], &exponent);
      else if (src[0]->bit_size == 32)
         val->ssa->def = build_frexp32(nb, src[0], &exponent);
      else
         val->ssa->def = build_frexp16(nb, src[0], &exponent);
      nir_store_deref(nb, vtn_nir_deref(b, w[6]), exponent, 0xf);
      return;
   }

   case GLSLstd450FrexpStruct: {
      vtn_assert(glsl_type_is_struct(val->ssa->type));
      if (src[0]->bit_size == 64)
         val->ssa->elems[0]->def = build_frexp64(nb, src[0],
                                                 &val->ssa->elems[1]->def);
      else if (src[0]->bit_size == 32)
         val->ssa->elems[0]->def = build_frexp32(nb, src[0],
                                                 &val->ssa->elems[1]->def);
      else
         val->ssa->elems[0]->def = build_frexp16(nb, src[0],
                                                 &val->ssa->elems[1]->def);
      return;
   }

   default:
      val->ssa->def =
         nir_build_alu(&b->nb,
                       vtn_nir_alu_op_for_spirv_glsl_opcode(b, entrypoint),
                       src[0], src[1], src[2], NULL);
      return;
   }
}

static void
handle_glsl450_interpolation(struct vtn_builder *b, enum GLSLstd450 opcode,
                             const uint32_t *w, unsigned count)
{
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);

   nir_intrinsic_op op;
   switch (opcode) {
   case GLSLstd450InterpolateAtCentroid:
      op = nir_intrinsic_interp_deref_at_centroid;
      break;
   case GLSLstd450InterpolateAtSample:
      op = nir_intrinsic_interp_deref_at_sample;
      break;
   case GLSLstd450InterpolateAtOffset:
      op = nir_intrinsic_interp_deref_at_offset;
      break;
   default:
      vtn_fail("Invalid opcode");
   }

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);

   struct vtn_pointer *ptr =
      vtn_value(b, w[5], vtn_value_type_pointer)->pointer;
   nir_deref_instr *deref = vtn_pointer_to_deref(b, ptr);

   /* If the value we are interpolating has an index into a vector then
    * interpolate the vector and index the result of that instead. This is
    * necessary because the index will get generated as a series of nir_bcsel
    * instructions so it would no longer be an input variable.
    */
   const bool vec_array_deref = deref->deref_type == nir_deref_type_array &&
      glsl_type_is_vector(nir_deref_instr_parent(deref)->type);

   nir_deref_instr *vec_deref = NULL;
   if (vec_array_deref) {
      vec_deref = deref;
      deref = nir_deref_instr_parent(deref);
   }
   intrin->src[0] = nir_src_for_ssa(&deref->dest.ssa);

   switch (opcode) {
   case GLSLstd450InterpolateAtCentroid:
      break;
   case GLSLstd450InterpolateAtSample:
   case GLSLstd450InterpolateAtOffset:
      intrin->src[1] = nir_src_for_ssa(vtn_ssa_value(b, w[6])->def);
      break;
   default:
      vtn_fail("Invalid opcode");
   }

   intrin->num_components = glsl_get_vector_elements(deref->type);
   nir_ssa_dest_init(&intrin->instr, &intrin->dest,
                     glsl_get_vector_elements(deref->type),
                     glsl_get_bit_size(deref->type), NULL);

   nir_builder_instr_insert(&b->nb, &intrin->instr);

   if (vec_array_deref) {
      assert(vec_deref);
      nir_const_value *const_index = nir_src_as_const_value(vec_deref->arr.index);
      if (const_index) {
         val->ssa->def = vtn_vector_extract(b, &intrin->dest.ssa,
                                            const_index->u32[0]);
      } else {
         val->ssa->def = vtn_vector_extract_dynamic(b, &intrin->dest.ssa,
                                                    vec_deref->arr.index.ssa);
      }
   } else {
      val->ssa->def = &intrin->dest.ssa;
   }
}

bool
vtn_handle_glsl450_instruction(struct vtn_builder *b, SpvOp ext_opcode,
                               const uint32_t *w, unsigned count)
{
   switch ((enum GLSLstd450)ext_opcode) {
   case GLSLstd450Determinant: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = rzalloc(b, struct vtn_ssa_value);
      val->ssa->type = vtn_value(b, w[1], vtn_value_type_type)->type->type;
      val->ssa->def = build_mat_det(b, vtn_ssa_value(b, w[5]));
      break;
   }

   case GLSLstd450MatrixInverse: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = matrix_inverse(b, vtn_ssa_value(b, w[5]));
      break;
   }

   case GLSLstd450InterpolateAtCentroid:
   case GLSLstd450InterpolateAtSample:
   case GLSLstd450InterpolateAtOffset:
      handle_glsl450_interpolation(b, ext_opcode, w, count);
      break;

   default:
      handle_glsl450_alu(b, (enum GLSLstd450)ext_opcode, w, count);
   }

   return true;
}
