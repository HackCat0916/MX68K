// ---------------------------------------------------------------------------
//	misc.h
//	Copyright (C) cisc 1998, 1999.
// ---------------------------------------------------------------------------
//	$fmgen-Id: misc.h,v 1.5 2002/05/31 09:45:20 cisc Exp $

#ifndef MISC_H
#define MISC_H

inline int32_t Max(int32_t x, int32_t y) { return (x > y) ? x : y; }
inline int32_t Min(int32_t x, int32_t y) { return (x < y) ? x : y; }
inline int32_t Abs(int32_t x) { return x >= 0 ? x : -x; }

inline int32_t Limit(int32_t v, int32_t max, int32_t min)
{ 
	return v > max ? max : (v < min ? min : v); 
}

inline uint32_t BSwap(uint32_t a)
{
	return (a >> 24) | ((a >> 8) & 0xff00) | ((a << 8) & 0xff0000) | (a << 24);
}

inline uint32_t NtoBCD(uint32_t a)
{
	return ((a / 10) << 4) + (a % 10);
}

inline uint32_t BCDtoN(uint32_t v)
{
	return (v >> 4) * 10 + (v & 15);
}


template<class T>
inline T gcd(T x, T y)
{
	T t;
	while (y)
	{
		t = x % y;
		x = y;
		y = t;
	}
	return x;
}


template<class T>
T bessel0(T x)
{
	T p, r, s;

	r = 1.0;
	s = 1.0;
	p = (x / 2.0) / s;

	while (p > 1.0E-10)
	{
		r += p * p;
		s += 1.0;
		p *= (x / 2.0) / s;
	}
	return r;
}


#endif // MISC_H

