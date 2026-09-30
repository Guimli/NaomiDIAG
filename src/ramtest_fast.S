#include "config.h"
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

! ---------------------------------------------------------------------------
! Everything below lives in its own section so the boot code can copy the
! whole block into CPU RAM and run it from there, cached, once that RAM has
! been proven. It is relocatable as written: every branch is PC-relative and
! the one literal (the CRC table address) travels with the code and points
! back at the table in ROM, which never moves.
! ---------------------------------------------------------------------------
    .section .text.reloc, "ax"
    .align 2
    .global _reloc_blk_start
_reloc_blk_start:

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
! u32 ram_verify_fast(u32 *base, u32 nblocks8, u32 pattern, u32 *diff_odd)
!   r4 = source, r5 = number of 8-word blocks (>=1), r6 = expected pattern,
!   r7 = where to store the odd-word mask. Returns the even-word mask in r0.
!
! The two masks are what pins the failing chip. The SDRAM bus is 64 bits of
! four 16-bit chips: a 32-bit word at an even offset is carried by chips 1 and
! 2, one at an odd offset by chips 3 and 4. Accumulating a single mask throws
! that parity away and leaves two candidates for every fault; keeping one mask
! per parity names exactly one chip. The loop is unrolled eight ways, so the
! parity of each word is known at assembly time and the split costs nothing --
! it is the same instruction, on a different register.
! 26 instructions per 8 words = 3.25 per word, unchanged.
! -----------------------------------------------------------------------------
    .global _ram_verify_fast
_ram_verify_fast:
    mov     #0, r1              ! differences on even-address words
    mov     #0, r2              ! differences on odd-address words
    tst     r5, r5
    bt      9f
1:  mov.l   @r4+, r0            ! word 0, even
    xor     r6, r0
    or      r0, r1
    mov.l   @r4+, r0            ! word 1, odd
    xor     r6, r0
    or      r0, r2
    mov.l   @r4+, r0            ! word 2, even
    xor     r6, r0
    or      r0, r1
    mov.l   @r4+, r0            ! word 3, odd
    xor     r6, r0
    or      r0, r2
    mov.l   @r4+, r0            ! word 4, even
    xor     r6, r0
    or      r0, r1
    mov.l   @r4+, r0            ! word 5, odd
    xor     r6, r0
    or      r0, r2
    mov.l   @r4+, r0            ! word 6, even
    xor     r6, r0
    or      r0, r1
    mov.l   @r4+, r0            ! word 7, odd
    xor     r6, r0
    or      r0, r2
    dt      r5
    bf      1b
9:  mov.l   r2, @r7             ! hand back the odd-word mask
    rts
    mov     r1, r0              ! and return the even-word one

! =============================================================================
! Pseudo-random pass, hand-written.
!
! The compiler needed 85 instructions per word for the read loop, which made
! it the single most expensive thing the diagnostic does. Two thirds of that
! was the two CRC accumulations, and most of the waste is one specific thing:
! the indexed addressing mode mov.l @(r0,Rm) forces the index into r0, and GCC
! keeps an extra live copy around it, spending 7 instructions per byte step
! where 5 suffice.
!
! Shared context block (offsets MUST match prng_ctx in ramtest.h):
!     0  x       PRNG state, in and out
!     4  crc_w   CRC of the stream we meant to write
!     8  crc_r   CRC of the stream actually read back
!    12  diff    OR of every (read XOR expected); 0 = region good
!
! Register map inside both loops:
!   r4 data pointer      r5 word count       r6 context block
!   r7 word read         r8 PRNG state       r9 crc_w
!   r10 crc_r            r11 diff            r12 CRC table
!   r13 = 13, r14 = -17  (shld shift amounts; a negative count shifts right)
! =============================================================================

! -----------------------------------------------------------------------------
! void ram_prng_fill_fast(u32 *base, u32 nwords, prng_ctx *c)
! 14 instructions per word (compiler: 19).
! -----------------------------------------------------------------------------
    .global _ram_prng_fill_fast
_ram_prng_fill_fast:
    tst     r5, r5
    bt      9f
    mov.l   r8, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   @r6, r8             ! x
    mov     #13, r13
    mov     #-17, r14
1:  mov     r8, r1              ! ---- xorshift32
    shld    r13, r1
    xor     r1, r8              ! x ^= x << 13
    mov     r8, r1
    shld    r14, r1
    xor     r1, r8              ! x ^= x >> 17
    mov     r8, r1
    shll2   r1
    shll2   r1
    shll    r1
    xor     r1, r8              ! x ^= x << 5
    mov.l   r8, @r4             ! store it
    add     #4, r4
    dt      r5
    bf      1b
    mov.l   r8, @r6             ! hand the PRNG state back
    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r8
9:  rts
    nop

! -----------------------------------------------------------------------------
! void ram_prng_verify_fast(const u32 *base, u32 npairs, prng_ctx *c)
!   r5 counts PAIRS of words, not words.
!
! Unrolled two ways so the parity of each word is known at assembly time: the
! first of every pair sits at an even 32-bit offset and is carried by chips 1
! and 2, the second at an odd offset, by chips 3 and 4. Their differences go
! to separate accumulators, which is what lets a fault name one chip instead
! of two candidates. The unrolling pays for itself -- one loop control for two
! words instead of one each -- so this is 58 instructions per word against the
! 59 of the version that could not tell the halves apart.
! -----------------------------------------------------------------------------
    .global _ram_prng_verify_fast
_ram_prng_verify_fast:
    tst     r5, r5              ! nothing to do: return before saving anything
    bf      1f                  ! (branch on the near side; the body below is
    rts                         !  far past bt's reach)
    nop
1:
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   @r6, r8             ! x
    mov.l   @(4, r6), r9        ! crc_w
    mov.l   @(8, r6), r10       ! crc_r
    mov.l   @(12, r6), r11      ! diff on even-address words
    mov.l   @(16, r6), r3       ! diff on odd-address words
    mov.l   Lcrctab, r12
    mov     #13, r13
    mov     #-17, r14

1:
    mov     r8, r1
    shld    r13, r1
    xor     r1, r8
    mov     r8, r1
    shld    r14, r1
    xor     r1, r8
    mov     r8, r1
    shll2   r1
    shll2   r1
    shll    r1
    xor     r1, r8

    mov.l   @r4+, r7
    mov     r7, r1
    xor     r8, r1
    or      r1, r11

#if CFG_RAM_CRC
    xor     r8, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
#endif

#if CFG_RAM_CRC
    xor     r7, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
#endif

    mov     r8, r1
    shld    r13, r1
    xor     r1, r8
    mov     r8, r1
    shld    r14, r1
    xor     r1, r8
    mov     r8, r1
    shll2   r1
    shll2   r1
    shll    r1
    xor     r1, r8

    mov.l   @r4+, r7
    mov     r7, r1
    xor     r8, r1
    or      r1, r3

#if CFG_RAM_CRC
    xor     r8, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
    extu.b  r9, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r9
    xor     r1, r9
#endif

#if CFG_RAM_CRC
    xor     r7, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
    extu.b  r10, r0
    shll2   r0
    mov.l   @(r0, r12), r1
    shlr8   r10
    xor     r1, r10
#endif

    dt      r5
    bt      2f                  ! the unrolled body is past bf's reach, so
    bra     1b                  ! the loop is written out rather than left
    nop                         ! to the assembler's relaxation
2:
    mov.l   r8, @r6
    mov.l   r9, @(4, r6)
    mov.l   r10, @(8, r6)
    mov.l   r11, @(12, r6)
    mov.l   r3, @(16, r6)
    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    rts
    nop

    .align 4
Lcrctab:    .long _crc32_tab8

! =============================================================================
! Locating the failing words, once a verify pass has seen a difference.
!
! This used to be a C loop from ROM calling note_fail() on every bad word.
! A cut data line makes every word of its half fail -- a million of them in
! 4 MB -- and that loop took three and a half minutes per pass on a real
! Naomi 2. These scan at verify speed and stop only on a word the caller
! still wants to see: any mismatch at all while the first details are being
! collected, then only one that brings a data bit not yet seen on its word
! parity (a bit the ignore masks do not cover). The words they pass over are
! counted, so the error total stays exact.
!
! Context block (offsets MUST match find_ctx in ramtest.h):
!     0  x       expected value (pattern), or PRNG state before the first word
!     4  ign_e   bits to pass over on even-address words
!     8  ign_o   bits to pass over on odd-address words
!    12  skipped mismatches passed over, accumulated
!    16  exp     expected value of the word reported
!    20  got     value read there
! Returns the index of the reported word, or n if none.
! =============================================================================

! u32 ram_find_pat_fast(const u32 *p, u32 n, find_ctx *c)
    .global _ram_find_pat_fast
_ram_find_pat_fast:
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   @(0, r6), r8        ! expected
    mov.l   @(4, r6), r9
    not     r9, r9              ! bits still wanted, even words
    mov.l   @(8, r6), r10
    not     r10, r10            ! bits still wanted, odd words
    mov.l   @(12, r6), r11      ! skipped so far
    mov     #0, r3              ! index
    tst     r5, r5
    bt      8f
1:  mov.l   @r4, r0
    xor     r8, r0
    tst     r0, r0
    bf      3f
2:  add     #4, r4
    add     #1, r3
    cmp/hs  r5, r3
    bf      1b
8:  bra     9f
    mov     r5, r0              ! nothing to report
3:  mov     r4, r1
    mov     #4, r2
    tst     r2, r1              ! T = even-address word
    bt/s    4f
    mov     r9, r2
    mov     r10, r2
4:  tst     r2, r0              ! T = no wanted bit in the difference
    bf      5f
    bra     2b
    add     #1, r11             ! passed over, counted
5:  mov.l   r8, @(16, r6)
    xor     r8, r0              ! difference back to the value read
    mov.l   r0, @(20, r6)
    mov     r3, r0
9:  mov.l   r11, @(12, r6)
    mov.l   r8, @(0, r6)
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    rts
    mov.l   @r15+, r8

! u32 ram_find_prng_fast(const u32 *p, u32 n, find_ctx *c)
! Same, with the expected value regenerated per word (xorshift32 13/17/5,
! as the fill wrote it). On return x is the state of the word reported,
! or of the last word, so the caller resumes on the next one.
    .global _ram_find_prng_fast
_ram_find_prng_fast:
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   @(0, r6), r8        ! state before the first word
    mov.l   @(4, r6), r9
    not     r9, r9
    mov.l   @(8, r6), r10
    not     r10, r10
    mov.l   @(12, r6), r11
    mov     #0, r3
    tst     r5, r5
    bt      8f
1:  mov     r8, r1              ! x ^= x << 13
    shll8   r1
    shll2   r1
    shll2   r1
    shll    r1
    xor     r1, r8
    mov     r8, r1              ! x ^= x >> 17
    shlr16  r1
    shlr    r1
    xor     r1, r8
    mov     r8, r1              ! x ^= x << 5
    shll2   r1
    shll2   r1
    shll    r1
    xor     r1, r8
    mov.l   @r4, r0
    xor     r8, r0
    tst     r0, r0
    bf      3f
2:  add     #4, r4
    add     #1, r3
    cmp/hs  r5, r3
    bf      1b
8:  bra     9f
    mov     r5, r0
3:  mov     r4, r1
    mov     #4, r2
    tst     r2, r1
    bt/s    4f
    mov     r9, r2
    mov     r10, r2
4:  tst     r2, r0
    bf      5f
    bra     2b
    add     #1, r11
5:  mov.l   r8, @(16, r6)
    xor     r8, r0
    mov.l   r0, @(20, r6)
    mov     r3, r0
9:  mov.l   r11, @(12, r6)
    mov.l   r8, @(0, r6)
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    rts
    mov.l   @r15+, r8

! =============================================================================
! SHA-1 over the cartridge, read straight from its PIO port.
!
! void sha1_pio_blocks(u32 h[5], u32 nblocks, u32 w[80])
! void sha1_mem_blocks(u32 h[5], u32 nblocks, u32 w[80], const u32 *src)
!   r4 = the five state words, updated in place
!   r5 = number of 64-byte blocks
!   r6 = an 80-word scratch area (the message schedule)
!   r7 = where the bytes come from: the PIO entry reads CART_ROM_DATA
!        (0xA05F7008), which advances by itself on every 16-bit read; the
!        memory entry reads a buffer the G1 DMA filled with the same bytes
!        in the same order
!
! The C version cost about 68 instructions per byte, from the EPROM: the
! compiler picked the round function with branches, kept the round counter
! in memory and made the 5-bit rotation out of five 1-bit ones. Here the
! rounds are unrolled five at a time with the five variables changing
! registers instead of being copied, the round function is branch-free, and
! rol5 is two dynamic shifts: about 31 instructions per byte, cart read
! included. It lives in the relocated block, so on a board with good CPU RAM
! it runs cached; the cartridge check refuses to run without that.
!
! Registers in the rounds: a..e r8..r12, K r13, schedule walker r14,
! shift counts r2 = 5 and r3 = -27, temporaries r0, r1, r7, group count r5.
! =============================================================================
    .macro  F_ch B, C, D            ! (B & C) | (~B & D) = D ^ (B & (C ^ D))
    mov     \C, r0
    xor     \D, r0
    and     \B, r0
    xor     \D, r0
    .endm
    .macro  F_par B, C, D           ! B ^ C ^ D
    mov     \B, r0
    xor     \C, r0
    xor     \D, r0
    .endm
    .macro  F_maj B, C, D           ! (B & C) | (D & (B | C))
    mov     \B, r0
    or      \C, r0
    and     \D, r0
    mov     \B, r1
    and     \C, r1
    or      r1, r0
    .endm
    .macro  RND f, A, B, C, D, E    ! E += rol5(A) + f(B,C,D) + K + W; B = rol30(B)
    mov.l   @r14+, r0
    add     r13, \E
    add     r0, \E
    mov     \A, r1
    shld    r2, r1
    mov     \A, r7
    shld    r3, r7
    or      r7, r1
    add     r1, \E
    F_\f    \B, \C, \D
    add     r0, \E
    rotr    \B
    rotr    \B
    .endm
    .macro  RND5 f                  ! five rounds bring the registers back
    RND     \f, r8, r9, r10, r11, r12
    RND     \f, r12, r8, r9, r10, r11
    RND     \f, r11, r12, r8, r9, r10
    RND     \f, r10, r11, r12, r8, r9
    RND     \f, r9, r10, r11, r12, r8
    .endm

    .global _sha1_pio_blocks
    .global _sha1_mem_blocks
_sha1_pio_blocks:
    mov     #0, r7                  ! no buffer: the port
_sha1_mem_blocks:
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   r4, @-r15               ! @(8,r15): h
    mov.l   r5, @-r15               ! @(4,r15): blocks left
    mov.l   r7, @-r15               ! @(0,r15): source buffer, 0 = port
    tst     r5, r5
    bf      .Lsblock
    bra     .Lsdone
    nop

.Lsblock:
    mov     r6, r3
    mov     #16, r2
    mov.l   @r15, r1
    tst     r1, r1
    bt      .Lsport_rd
    ! 16 message words from memory: little-endian there, b3 b2 b1 b0 once
    ! loaded, turned big-endian
0:  mov.l   @r1+, r0
    swap.b  r0, r0
    swap.w  r0, r0
    swap.b  r0, r0
    mov.l   r0, @r3
    dt      r2
    bf/s    0b
    add     #4, r3
    bra     .Lsched
    mov.l   r1, @r15                ! where the next block starts

.Lsport_rd:
    ! 16 message words, big-endian: each is two port reads, b0 b1 then b2 b3
    mov.l   .Lsport, r1
1:  mov.w   @r1, r0
    extu.w  r0, r0
    swap.b  r0, r0                  ! b0 << 8 | b1
    shll16  r0
    mov.w   @r1, r7
    extu.w  r7, r7
    swap.b  r7, r7                  ! b2 << 8 | b3
    or      r7, r0
    mov.l   r0, @r3
    dt      r2
    bf/s    1b
    add     #4, r3

    ! schedule: w[i] = rol1(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]), i = 16..79
.Lsched:
    mov     r6, r7                  ! r7 = &w[i-16], r3 = &w[i]
    mov     #64, r2
2:  mov.l   @r7, r0
    mov.l   @(8,r7), r1
    xor     r1, r0
    mov.l   @(32,r7), r1
    xor     r1, r0
    mov.l   @(52,r7), r1
    xor     r1, r0
    rotl    r0
    mov.l   r0, @r3
    add     #4, r3
    dt      r2
    bf/s    2b
    add     #4, r7

    mov.l   @(8,r15), r0
    mov.l   @r0, r8
    mov.l   @(4,r0), r9
    mov.l   @(8,r0), r10
    mov.l   @(12,r0), r11
    mov.l   @(16,r0), r12
    mov     r6, r14
    mov     #5, r2
    mov     #-27, r3

    mov.l   .Lsk1, r13
    mov     #4, r5
3:  RND5    ch
    dt      r5
    bf      3b
    mov.l   .Lsk2, r13
    mov     #4, r5
4:  RND5    par
    dt      r5
    bf      4b
    mov.l   .Lsk3, r13
    mov     #4, r5
5:  RND5    maj
    dt      r5
    bf      5b
    mov.l   .Lsk4, r13
    mov     #4, r5
6:  RND5    par
    dt      r5
    bf      6b

    mov.l   @(8,r15), r0            ! h += a..e
    mov.l   @r0, r1
    add     r1, r8
    mov.l   r8, @r0
    mov.l   @(4,r0), r1
    add     r1, r9
    mov.l   r9, @(4,r0)
    mov.l   @(8,r0), r1
    add     r1, r10
    mov.l   r10, @(8,r0)
    mov.l   @(12,r0), r1
    add     r1, r11
    mov.l   r11, @(12,r0)
    mov.l   @(16,r0), r1
    add     r1, r12
    mov.l   r12, @(16,r0)

    mov.l   @(4,r15), r0
    dt      r0
    mov.l   r0, @(4,r15)
    bt      .Lsdone
    bra     .Lsblock
    nop

.Lsdone:
    add     #12, r15
    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    rts
    mov.l   @r15+, r8

    .align 2
.Lsport:    .long   0xA05F7008      ! CART_ROM_DATA, P2
.Lsk1:      .long   0x5A827999
.Lsk2:      .long   0x6ED9EBA1
.Lsk3:      .long   0x8F1BBCDC
.Lsk4:      .long   0xCA62C1D6

    .global _reloc_blk_end
_reloc_blk_end:
