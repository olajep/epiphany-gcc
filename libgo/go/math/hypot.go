// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package math

/*
	Hypot -- sqrt(p*p + q*q), but overflows only if the result does.
*/

// Hypot computes Sqrt(p*p + q*q), taking care to avoid
// unnecessary overflow and underflow.
//
// Special cases are:
//	Hypot(p, q) = +Inf if p or q is infinite
//	Hypot(p, q) = NaN if p or q is NaN
func Hypot(p, q float64) float64 {
	return hypot(p, q)
}

func hypot(p, q float64) float64 {
	// TODO(rsc): Remove manual inlining of IsNaN, IsInf
	// when compiler does it for us
	// special cases
	switch {
	case p < -MaxFloat64 || p > MaxFloat64 || q < -MaxFloat64 || q > MaxFloat64: // IsInf(p, 0) || IsInf(q, 0):
		return Inf(1)
	case p != p || q != q: // IsNaN(p) || IsNaN(q):
		return NaN()
	}
	if p < 0 {
		p = -p
	}
	if q < 0 {
		q = -q
	}
	if p < q {
		p, q = q, p
	}
	if p == 0 {
		return 0
	}
	q = q / p
	return p * Sqrt(1+q*q)
}
