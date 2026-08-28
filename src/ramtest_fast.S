! =============================================================================
! Hand-written inner loops for the RAM pattern test.
!
! The ROM executes uncached straight from the boot EPROM: every instruction
! fetch is an external bus cycle. Instruction COUNT is therefore very nearly
! proportional to wall-clock time here, which is not true of normal code --
! it is what makes hand-tuning these loops worth doing at all.
!
! What the C version spends per 32-bit word (measured on the generated code):
!   fill   7 instructions, of which exactly one is the store
!   verify 11 instructions
! The overhead is loop bookkeeping and the progress check, both of which can
! be amortised over a block instead of paid per word.
!
! Strategy:
!   fill   unrolled 16x, one base register with displacement addressing, so
!          the pointer advance and loop control are paid once per 16 words
!   verify unrolled 8x, BRANCHLESS: accumulate (read XOR expected) into a
!          register instead of testing each word. A branch per word costs the
!          test, the branch itself and a pipeline bubble; ORing the difference
!          costs one instruction and cannot miss a bad bit. Any nonzero result
!          sends the caller into the slow C loop, which then locates the exact
!          failing addresses -- full diagnostic detail is kept, it is simply
!          only paid for on boards that actually have a fault.
! =============================================================================

    .section .text, "ax"
    .align 2

! -----------------------------------------------------------------------------
! void ram_fill_fast(u32 *base, u32 nblocks16, u32 pattern)
!   r4 = destination, r5 = number of 16-word blocks (>=1), r6 = pattern
! 19 instructions per 16 words = 1.19 per word.
! -----------------------------------------------------------------------------
    .global _ram_fill_fast
_ram_fill_fast:
    tst     r5, r5
    bt      9f
1:  mov.l   r6, @(0, r4)
    mov.l   r6, @(4, r4)
    mov.l   r6, @(8, r4)
    mov.l   r6, @(12, r4)
    mov.l   r6, @(16, r4)
    mov.l   r6, @(20, r4)
    mov.l   r6, @(24, r4)
    mov.l   r6, @(28, r4)
    mov.l   r6, @(32, r4)
    mov.l   r6, @(36, r4)
    mov.l   r6, @(40, r4)
    mov.l   r6, @(44, r4)
    mov.l   r6, @(48, r4)
    mov.l   r6, @(52, r4)
    mov.l   r6, @(56, r4)
    mov.l   r6, @(60, r4)
    dt      r5
    bf/s    1b
    add     #64, r4         ! delay slot: pointer advance is free
9:  rts
    nop

! -----------------------------------------------------------------------------
! u32 ram_verify_fast(u32 *base, u32 nblocks8, u32 pattern)
!   r4 = source, r5 = number of 8-word blocks (>=1), r6 = expected pattern
!   returns in r0 the OR of every (read XOR expected): 0 = whole region good
! 26 instructions per 8 words = 3.25 per word.
! -----------------------------------------------------------------------------
    .global _ram_verify_fast
_ram_verify_fast:
    mov     #0, r7          ! r7 = accumulated difference
    tst     r5, r5
    bt      9f
1:  mov.l   @r4+, r0        ! post-increment: the pointer advance is free
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    mov.l   @r4+, r0
    xor     r6, r0
    or      r0, r7
    dt      r5
    bf      1b
9:  rts
    mov     r7, r0
