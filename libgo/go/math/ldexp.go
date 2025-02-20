// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package math

// Ldexp is the inverse of Frexp.
// It returns frac × 2**exp.
//
// Special cases are:
//	Ldexp(±0, exp) = ±0
//	Ldexp(±Inf, exp) = ±Inf
//	Ldexp(NaN, exp) = NaN
func libc_ldexp(float64, int) float64 __asm__("ldexp")
func Ldexp(frac float64, exp int) float64 {
	return libc_ldexp(frac, exp)
}

func ldexp(frac float64, exp int) float64 {
	// TODO(rsc): Remove manual inlining of IsNaN, IsInf
	// when compiler does it for us
	// special cases
	switch {
	case frac == 0:
		return frac // correctly return -0
	case frac < -MaxFloat64 || frac > MaxFloat64 || frac != frac: // IsInf(frac, 0) || IsNaN(frac):
		return frac
	}
	frac, e := normalize(frac)
	exp += e
	x := Float64bits(frac)
	exp += int(x>>shift)&mask - bias
	if exp < -1074 {
		return Copysign(0, frac) // underflow
	}
	if exp > 1023 { // overflow
		if frac < 0 {
			return Inf(-1)
		}
		return Inf(1)
	}
	var m float64 = 1
	if exp < -1022 { // denormal
		exp += 52
		m = 1.0 / (1 << 52) // 2**-52
	}
	x &^= mask << shift
	x |= uint64(exp+bias) << shift
	return m * Float64frombits(x)
}
