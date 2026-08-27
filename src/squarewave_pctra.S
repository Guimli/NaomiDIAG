! =============================================================================
! Square-wave sign-of-life ROM, WITH the GPIO port A configuration: transmits 'U' (0x55) on the SCIF forever.
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
    ! -----------------------------------------------------------------
    ! Port control register A = 0x000FEF30, the value JinGasa writes as the
    ! very first thing it does -- and JinGasa exists solely to talk over the
    ! serial port. The original BIOS programs PCTRA too. Neither our ROMs
    ! nor MAME ever touch it: MAME's SCIF transmits regardless of pin
    ! configuration, so if the real chip needs the pin enabled here, the
    ! difference is invisible in emulation.
    ! Built with immediates and shifts only -- still no data read at all.
    ! -----------------------------------------------------------------
    mov     #-128, r2               ! 0xFFFFFF80
    shll16  r2                      ! 0xFF800000
    add     #0x2C, r2               ! 0xFF80002C = PCTRA
    mov     #-17, r0                ! 0xFFFFFFEF
    shll8   r0                      ! 0xFFFFEF00
    add     #0x30, r0               ! 0xFFFFEF30
    shll16  r0
    shlr16  r0                      ! 0x0000EF30
    mov     #15, r3
    shll16  r3                      ! 0x000F0000
    or      r3, r0                  ! 0x000FEF30
    mov.l   r0, @r2                 ! PCTRA = 0x000FEF30

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
