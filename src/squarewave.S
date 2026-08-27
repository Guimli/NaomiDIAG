! =============================================================================
! Absolute-minimum sign-of-life ROM: transmits 'U' (0x55) on the SCIF forever.
!
! It performs NO data read of any kind -- no PC-relative literal load, no
! string in ROM, nothing but instruction fetches. Every constant is built
! with immediates and shifts, exactly the way every SEGA Naomi BIOS builds
! 0xFF000000 in its first instructions.
!
! 0x55 = 01010101, so the TXD line becomes a clean square wave at half the
! bit rate: trivially visible on a scope, no decoding needed.
!
! If this shows nothing on TXD while a SEGA BIOS burned on the same EPROM
! boots, then the CPU is not executing our images at all -- and the cause is
! something other than the code itself.
! =============================================================================

    .section .text.start, "ax"
    .global _start
_start:
    mov     #-24, r1                ! 0xFFFFFFE8
    shll16  r1                      ! r1 = 0xFFE80000  (SCIF base, no memory read)

    mov     #0, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = 0
    mov     #6, r0
    mov.w   r0, @(24, r1)           ! SCFCR2 = TFRST|RFRST
    mov     #0, r0
    mov.w   r0, @(0, r1)            ! SCSMR2 = 0  (8N1)
    mov     #12, r0
    mov.b   r0, @(4, r1)            ! SCBRR2 = 12 (115200 @ Pck 50MHz)

    mov     #1, r2                  ! settle delay = 0x10000
    shll16  r2
1:  dt      r2
    bf      1b

    mov     #0, r0
    mov.w   r0, @(24, r1)           ! release FIFO reset
    mov.w   r0, @(16, r1)           ! clear status
    mov     #0x30, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = TE|RE

loop:
    mov     #1, r6                  ! bounded wait for TX FIFO room
    shll16  r6
2:  mov.w   @(16, r1), r0           ! SCFSR2
    tst     #0x20, r0               ! TDFE?
    bf      3f
    dt      r6
    bf      2b
3:  mov     #0x55, r0               ! 'U' -> 0101 0101 -> square wave
    mov.b   r0, @(12, r1)           ! SCFTDR2
    mov     #0, r0
    mov.w   r0, @(16, r1)           ! clear status flags
    bra     loop
    nop
