/* Minimal stand-in for compiler-rt's x86 cpu_model.c, which zig does not
 * ship for windows-gnu targets.  clang lowers __builtin_cpu_init() /
 * __builtin_cpu_supports() and target("...") function multiversioning
 * (simd-checksum-x86_64.cpp) onto these two symbols.
 *
 * Only the feature bits are filled in; vendor/type/subtype stay 0, which
 * __builtin_cpu_is() would treat as "unknown CPU".  Bit numbers follow
 * compiler-rt's enum ProcessorFeatures. */

#include <cpuid.h>

struct __processor_model {
	unsigned int __cpu_vendor;
	unsigned int __cpu_type;
	unsigned int __cpu_subtype;
	unsigned int __cpu_features[1];
} __cpu_model;

unsigned int __cpu_features2[4];

enum {
	FEATURE_CMOV = 0, FEATURE_MMX, FEATURE_POPCNT, FEATURE_SSE, FEATURE_SSE2,
	FEATURE_SSE3, FEATURE_SSSE3, FEATURE_SSE4_1, FEATURE_SSE4_2, FEATURE_AVX,
	FEATURE_AVX2, FEATURE_SSE4_A, FEATURE_FMA4, FEATURE_XOP, FEATURE_FMA,
	FEATURE_AVX512F, FEATURE_BMI, FEATURE_BMI2
};

static unsigned long long xgetbv0(void)
{
	unsigned int lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((unsigned long long)hi << 32) | lo;
}

int __cpu_indicator_init(void)
{
	unsigned int a, b, c, d, max, f = 0;
	int os_avx = 0;

	if (__cpu_model.__cpu_features[0])
		return 0;
	if (!__get_cpuid(0, &max, &b, &c, &d))
		return -1;
	__cpuid(1, a, b, c, d);
	if (d & (1u << 15)) f |= 1u << FEATURE_CMOV;
	if (d & (1u << 23)) f |= 1u << FEATURE_MMX;
	if (d & (1u << 25)) f |= 1u << FEATURE_SSE;
	if (d & (1u << 26)) f |= 1u << FEATURE_SSE2;
	if (c & (1u << 0))  f |= 1u << FEATURE_SSE3;
	if (c & (1u << 9))  f |= 1u << FEATURE_SSSE3;
	if (c & (1u << 19)) f |= 1u << FEATURE_SSE4_1;
	if (c & (1u << 20)) f |= 1u << FEATURE_SSE4_2;
	if (c & (1u << 23)) f |= 1u << FEATURE_POPCNT;
	/* AVX needs the CPU bit *and* the OS saving YMM state (OSXSAVE+XCR0). */
	if ((c & (1u << 27)) && (c & (1u << 28)) && (xgetbv0() & 6) == 6) {
		os_avx = 1;
		f |= 1u << FEATURE_AVX;
		if (c & (1u << 12)) f |= 1u << FEATURE_FMA;
	}
	if (max >= 7) {
		__cpuid_count(7, 0, a, b, c, d);
		if (os_avx && (b & (1u << 5))) f |= 1u << FEATURE_AVX2;
		if (b & (1u << 3)) f |= 1u << FEATURE_BMI;
		if (b & (1u << 8)) f |= 1u << FEATURE_BMI2;
	}
	__cpu_model.__cpu_features[0] = f;
	return 0;
}

/* compiler-rt runs this as a constructor too. */
__attribute__((constructor(101))) static void cpu_model_ctor(void)
{
	__cpu_indicator_init();
}
