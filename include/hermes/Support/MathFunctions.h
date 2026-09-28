/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SUPPORT_MATHFUNCTIONS_H
#define HERMES_SUPPORT_MATHFUNCTIONS_H

#include <float.h>
#include <math.h>
#include <stdint.h>

/// JavaScript rounding is toward positive infinity at a tie. Unlike round(),
/// it preserves negative zero in [-0.5, 0). Avoid adding 0.5 where that addition
/// would round a value below 0.5 to 1 or an already integral value upward.
static inline double hermesMathRound(double x) {
  const double integerThreshold = UINT64_C(1) << (DBL_MANT_DIG - 1);
  double magnitude = fabs(x);
  if (magnitude >= integerThreshold)
    return x;
  if (magnitude < 0.5)
    return copysign(0, x);
  return copysign(floor(x + 0.5), x);
}

/// Unlike fmin/fmax, JavaScript propagates either NaN. The sign comparison
/// orders -0 before +0 as well as values on opposite sides of zero.
static inline double hermesMathMin(double x, double y) {
  if (isnan(x) || isnan(y))
    return NAN;
  return x < y || signbit(x) > signbit(y) ? x : y;
}

static inline double hermesMathMax(double x, double y) {
  if (isnan(x) || isnan(y))
    return NAN;
  return x > y || signbit(x) < signbit(y) ? x : y;
}

#endif
