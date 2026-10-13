/* long long abi_call5(fn, a1, a2, a3, a4, a5, int *ret)
 *
 * Calls fn(a1..a5) under the Windows x64 ABI with a sentinel value in every
 * callee-saved register (rbx rbp rdi rsi r12-r15, xmm6-xmm15 both halves),
 * stores fn's eax in *ret, and returns a bitmask of the registers fn failed
 * to preserve (0 = ABI-clean):
 *   bit 0..7  = rbx rbp rdi rsi r12 r13 r14 r15
 *   bit 8..17 = xmm6 .. xmm15
 * Test helper for the asm-roll / asm-MD5 Win64 shims (win32\patches.ps1). */

.intel_syntax noprefix
.text
	.globl abi_call5
abi_call5:
	push	rbx
	push	rbp
	push	rdi
	push	rsi
	push	r12
	push	r13
	push	r14
	push	r15
	/* entry rsp = 8 mod 16; +64 pushed = 8 mod 16; 0xD8 brings it to 0.
	 * [rsp+0x00] shadow space, [rsp+0x20] 5th arg for fn,
	 * [rsp+0x30 + 16*k] saved xmm(6+k). Our own args: entry+0x28.. ->
	 * rsp + 0xD8 + 0x40 + 0x28 = rsp+0x140 (a4), 0x148 (a5), 0x150 (ret). */
	sub	rsp, 0xD8
	movdqu	[rsp+0x30], xmm6
	movdqu	[rsp+0x40], xmm7
	movdqu	[rsp+0x50], xmm8
	movdqu	[rsp+0x60], xmm9
	movdqu	[rsp+0x70], xmm10
	movdqu	[rsp+0x80], xmm11
	movdqu	[rsp+0x90], xmm12
	movdqu	[rsp+0xA0], xmm13
	movdqu	[rsp+0xB0], xmm14
	movdqu	[rsp+0xC0], xmm15

	mov	r10, rcx            /* fn */
	mov	rcx, rdx            /* a1 */
	mov	rdx, r8             /* a2 */
	mov	r8, r9              /* a3 */
	mov	r9, [rsp+0x140]     /* a4 */
	mov	rax, [rsp+0x148]    /* a5 */
	mov	[rsp+0x20], rax

#define SENT(n) (0x0101010101010101 * (n))
	mov	rbx, SENT(1)
	mov	rbp, SENT(2)
	mov	rdi, SENT(3)
	mov	rsi, SENT(4)
	mov	r12, SENT(5)
	mov	r13, SENT(6)
	mov	r14, SENT(7)
	mov	r15, SENT(8)
#define SETX(x, n) mov rax, SENT(n); movq x, rax; mov rax, SENT(n + 0x10); pinsrq x, rax, 1
	SETX(xmm6, 9)
	SETX(xmm7, 10)
	SETX(xmm8, 11)
	SETX(xmm9, 12)
	SETX(xmm10, 13)
	SETX(xmm11, 14)
	SETX(xmm12, 15)
	SETX(xmm13, 16)
	SETX(xmm14, 17)
	SETX(xmm15, 18)

	call	r10
	mov	r11d, eax           /* fn's return value */
	xor	r10, r10            /* corruption mask */

#define CHKR(r, n, bit) mov rax, SENT(n); cmp r, rax; je 1f; or r10, (1 << bit); 1:
	CHKR(rbx, 1, 0)
	CHKR(rbp, 2, 1)
	CHKR(rdi, 3, 2)
	CHKR(rsi, 4, 3)
	CHKR(r12, 5, 4)
	CHKR(r13, 6, 5)
	CHKR(r14, 7, 6)
	CHKR(r15, 8, 7)
#define CHKX(x, n, bit) movq rax, x; mov rcx, SENT(n); cmp rax, rcx; jne 2f; pextrq rax, x, 1; mov rcx, SENT(n + 0x10); cmp rax, rcx; je 3f; 2: or r10, (1 << bit); 3:
	CHKX(xmm6, 9, 8)
	CHKX(xmm7, 10, 9)
	CHKX(xmm8, 11, 10)
	CHKX(xmm9, 12, 11)
	CHKX(xmm10, 13, 12)
	CHKX(xmm11, 14, 13)
	CHKX(xmm12, 15, 14)
	CHKX(xmm13, 16, 15)
	CHKX(xmm14, 17, 16)
	CHKX(xmm15, 18, 17)

	mov	rax, [rsp+0x150]
	test	rax, rax
	jz	4f
	mov	[rax], r11d
4:
	movdqu	xmm6,  [rsp+0x30]
	movdqu	xmm7,  [rsp+0x40]
	movdqu	xmm8,  [rsp+0x50]
	movdqu	xmm9,  [rsp+0x60]
	movdqu	xmm10, [rsp+0x70]
	movdqu	xmm11, [rsp+0x80]
	movdqu	xmm12, [rsp+0x90]
	movdqu	xmm13, [rsp+0xA0]
	movdqu	xmm14, [rsp+0xB0]
	movdqu	xmm15, [rsp+0xC0]
	add	rsp, 0xD8
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rsi
	pop	rdi
	pop	rbp
	pop	rbx
	mov	rax, r10
	ret
