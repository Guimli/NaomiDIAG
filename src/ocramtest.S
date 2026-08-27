! =============================================================================
! Does the SH-4 operand cache really work as RAM on this board?
!
! NaomiDIAG puts its stack and .bss in the operand cache configured as RAM
! (CCR.ORA|OIX). No other Naomi boot code does this: the original BIOS runs
! with CCR = 0, JinGasa keeps its stack in main RAM, KallistiOS never
! configures it. It is validated only in MAME -- and MAME models OC-RAM as
! two plain 4 KB blocks, which may be more forgiving than real silicon.
!
! This ROM performs exactly the crt0 sequence (SR, MMUCR, CCR = 0x9A9), runs
! the same OC-RAM self-test, additionally exercises the whole 4 KB stack
! bank, and then paints the screen:
!
!     RED   -> OC-RAM works. The stack of NaomiDIAG is sound; look elsewhere.
!     GREEN -> OC-RAM is NOT usable on this board. That is the bug: every C
!              function call in NaomiDIAG runs on a broken stack.
!
! Uses no external RAM whatsoever, and the video path is the one you already
! proved works (31 kHz).
! =============================================================================

    .section .text.start, "ax"
    .global _start
_start:
    mov.l   Lsr_init, r0
    ldc     r0, sr                  ! same SR as NaomiDIAG
    mov.l   Lmmucr, r1
    mov     #0, r0
    mov.l   r0, @r1                 ! MMUCR = 0
    mov.l   Lccr, r1
    mov.l   Lccr_val, r0
    mov.l   r0, @r1                 ! CCR = ICE|ICI|OCE|OCI|ORA|OIX
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop

    mov     #0, r10                 ! r10 = 0 -> OK so far

! ---- the exact self-test crt0 performs: 4 cells, 2 banks, distinct values --
    mov.l   Locram0, r2             ! 0x7C000000
    mov.l   Locram0e, r3            ! 0x7C000FFC
    mov.l   Locram1, r6             ! 0x7E000000
    mov.l   Locram1e, r7            ! 0x7E000FFC
    mov.l   Lpat1, r0
    mov.l   Lpat2, r1
    mov.l   r0, @r2
    mov.l   r1, @r3
    not     r0, r4
    not     r1, r5
    mov.l   r4, @r6
    mov.l   r5, @r7
    mov.l   @r2, r8
    cmp/eq  r0, r8
    bt      1f
    mov     #1, r10                 ! mismatch -> failure
1:  mov.l   @r3, r8
    cmp/eq  r1, r8
    bt      2f
    mov     #1, r10
2:  mov.l   @r6, r8
    cmp/eq  r4, r8
    bt      3f
    mov     #1, r10
3:  mov.l   @r7, r8
    cmp/eq  r5, r8
    bt      4f
    mov     #1, r10

! ---- harder: fill the whole 4 KB stack bank with a walking value ---------
4:  mov.l   Locram0, r2             ! 0x7C000000
    mov.l   Lbanklen, r3            ! 4096 bytes
    mov     r2, r4
    mov     r3, r5
    mov     #0, r0
5:  mov.l   r0, @r4                 ! write index at each word
    add     #4, r4
    add     #1, r0
    mov     r0, r8
    shll2   r8
    cmp/hs  r5, r8
    bf      5b

    mov     r2, r4                  ! read it all back
    mov     #0, r0
6:  mov.l   @r4, r8
    cmp/eq  r0, r8
    bt      7f
    mov     #1, r10                 ! stack bank does not hold data
7:  add     #4, r4
    add     #1, r0
    mov     r0, r8
    shll2   r8
    cmp/hs  r5, r8
    bf      6b

! ---- report by colour, forever ------------------------------------------
    tst     r10, r10
    bt      8f
    mov.l   Ltabko, r4              ! GREEN: OC-RAM unusable
    bra     9f
    nop
8:  mov.l   Ltabok, r4              ! RED: OC-RAM fine
9:  bsr     apply
    nop
loop:
    bra     loop
    nop

apply:
1:  mov.l   @r4+, r0
    tst     r0, r0
    bt      9f
    mov.l   @r4+, r1
    mov.l   r1, @r0
    bra     1b
    nop
9:  rts
    nop

    .align 4
Lsr_init:  .long 0x700000F0
Lmmucr:    .long 0xFF000010
Lccr:      .long 0xFF00001C
Lccr_val:  .long 0x000009A9
Locram0:   .long 0x7C000000
Locram0e:  .long 0x7C000FFC
Locram1:   .long 0x7E000000
Locram1e:  .long 0x7E000FFC
Lpat1:     .long 0x5AA51234
Lpat2:     .long 0xC33C9876
Lbanklen:  .long 4096
Ltabok:    .long tab_red
Ltabko:    .long tab_green

    .align 4
tab_red:
    .long 0xA05F8008, 0x00000000
    .long 0xA05F80A4, 0x0000001F
    .long 0xA05F80A8, 0x15D1C951
    .long 0xA05F80A0, 0x00000020
    .long 0xA05F80E8, 0x00160008
    .long 0xA05F8044, 0x00800000
    .long 0xA05F80D0, 0x00000100
    .long 0xA05F80D8, 0x020C0359
    .long 0xA05F80D4, 0x007E0345
    .long 0xA05F80DC, 0x00280208
    .long 0xA05F80E0, 0x03F1933F
    .long 0xA05F80EC, 0x000000A6
    .long 0xA05F80F0, 0x00230023
    .long 0xA05F8040, 0x00FF0000    ! RED
    .long 0xA05F80E8, 0x00160000
    .long 0x00000000
tab_green:
    .long 0xA05F8008, 0x00000000
    .long 0xA05F80A4, 0x0000001F
    .long 0xA05F80A8, 0x15D1C951
    .long 0xA05F80A0, 0x00000020
    .long 0xA05F80E8, 0x00160008
    .long 0xA05F8044, 0x00800000
    .long 0xA05F80D0, 0x00000100
    .long 0xA05F80D8, 0x020C0359
    .long 0xA05F80D4, 0x007E0345
    .long 0xA05F80DC, 0x00280208
    .long 0xA05F80E0, 0x03F1933F
    .long 0xA05F80EC, 0x000000A6
    .long 0xA05F80F0, 0x00230023
    .long 0xA05F8040, 0x0000FF00    ! GREEN
    .long 0xA05F80E8, 0x00160000
    .long 0x00000000
