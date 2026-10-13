/* Win64 ABI + correctness check for rsync's hand-written asm, as shimmed by
 * win32\patches.ps1:
 *   get_checksum1_avx2_asm (simd-checksum-avx2.S, --enable-roll-asm)
 *   md5_process_asm        (lib/md5-asm-x86_64.S, --enable-md5-asm)
 * Every call goes through abi_call5 (abi_call.S), which detects any
 * callee-saved register the asm fails to preserve.  Built and run by
 * build-win.ps1 when asm is enabled; exit status 0 = all good. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef signed char schar;

long long abi_call5(void *fn, void *a1, void *a2, void *a3, void *a4, void *a5, int *ret);
int get_checksum1_avx2_asm(schar *buf, int len, int i, uint32_t *ps1, uint32_t *ps2);
void md5_process_asm(void *ctx, const void *data, size_t nblocks);

static const char *reg_names[] = {
	"rbx", "rbp", "rdi", "rsi", "r12", "r13", "r14", "r15",
	"xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
};

static int report(const char *what, long long mask)
{
	int i;
	if (!mask)
		return 0;
	printf("FAIL %s clobbers:", what);
	for (i = 0; i < 18; i++) {
		if (mask & (1LL << i))
			printf(" %s", reg_names[i]);
	}
	printf("\n");
	return 1;
}

/* rsync's rolling checksum, plain C (CHAR_OFFSET 0), from offset i on. */
static void sums_c(const schar *buf, int len, int i, uint32_t *s1, uint32_t *s2)
{
	for (; i < len; i++) {
		*s1 += buf[i];
		*s2 += *s1;
	}
}

static int have_avx2(void)
{
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2");
}

/* Plain-C MD5 compression of one 64-byte block (RFC 1321), for comparison. */
#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define STEP(f, a, b, c, d, x, t, s) (a) += f((b), (c), (d)) + (x) + (t); (a) = ROL((a), (s)) + (b)
static void md5_block_c(uint32_t st[4], const unsigned char *p)
{
	static const uint32_t T[64] = {
		0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
		0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
		0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
		0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
		0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
		0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
		0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
		0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
	static const int S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
		5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20, 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
		6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
	uint32_t x[16], a = st[0], b = st[1], c = st[2], d = st[3], t;
	int i;
	for (i = 0; i < 16; i++)
		x[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
	for (i = 0; i < 64; i++) {
		uint32_t f; int g;
		if (i < 16) { f = F(b, c, d); g = i; }
		else if (i < 32) { f = G(b, c, d); g = (5*i + 1) & 15; }
		else if (i < 48) { f = H(b, c, d); g = (3*i + 5) & 15; }
		else { f = I(b, c, d); g = (7*i) & 15; }
		t = d; d = c; c = b;
		b = b + ROL(a + f + T[i] + x[g], S[i]);
		a = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

int main(void)
{
	static const int sizes[] = { 0, 1, 63, 64, 127, 128, 129, 191, 192, 255, 256, 700, 1024, 4096, 65536, 1 << 20 };
	int fails = 0, n, off, k;
	schar *mem = malloc((1 << 20) + 128);

	srand(12345);
	for (k = 0; k < (1 << 20) + 128; k++)
		mem[k] = (schar)rand();

	if (!have_avx2())
		printf("SKIP checksum: CPU has no AVX2\n");
	else {
		for (off = 0; off < 3; off++) {
			for (n = 0; n < (int)(sizeof sizes / sizeof sizes[0]); n++) {
				schar *buf = mem + off * 7;
				int len = sizes[n], i = 0;
				uint32_t s1 = 0, s2 = 0, r1 = 0, r2 = 0;
				long long m = abi_call5((void *)get_checksum1_avx2_asm, buf, (void *)(intptr_t)len,
							(void *)(intptr_t)0, &s1, &s2, &i);
				fails += report("get_checksum1_avx2_asm", m);
				sums_c(buf, len, i, &s1, &s2);
				sums_c(buf, len, 0, &r1, &r2);
				/* rsync only uses (s1 & 0xffff) + (s2 << 16): the high
				 * halves are scratch, and the asm leaves other values there. */
				if (((s1 & 0xffff) + (s2 << 16)) != ((r1 & 0xffff) + (r2 << 16))) {
					printf("FAIL checksum len=%d off=%d: asm %08x c %08x\n", len, off * 7,
					       (s1 & 0xffff) + (s2 << 16), (r1 & 0xffff) + (r2 << 16));
					fails++;
				}
			}
		}
		if (!fails)
			printf("ok   get_checksum1_avx2_asm: ABI-clean, matches C for %d cases\n",
			       3 * (int)(sizeof sizes / sizeof sizes[0]));
	}

	{
		int before = fails;
		for (n = 0; n < 64; n++) {
			uint32_t st_asm[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
			uint32_t st_c[4];
			size_t blocks = (size_t)(n % 9);
			unsigned char *data = (unsigned char *)mem + n * 13;
			memcpy(st_c, st_asm, sizeof st_c);
			fails += report("md5_process_asm",
					abi_call5((void *)md5_process_asm, st_asm, data, (void *)blocks, NULL, NULL, NULL));
			for (k = 0; k < (int)blocks; k++)
				md5_block_c(st_c, data + k * 64);
			if (memcmp(st_asm, st_c, sizeof st_c) != 0) {
				printf("FAIL md5 blocks=%d\n", (int)blocks);
				fails++;
			}
		}
		if (fails == before)
			printf("ok   md5_process_asm: ABI-clean, matches C for 64 cases\n");
	}

	printf(fails ? "%d failure(s)\n" : "all asm ABI checks passed\n", fails);
	free(mem);
	return fails != 0;
}
