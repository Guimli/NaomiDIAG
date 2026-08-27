! =============================================================================
! Square-wave sign-of-life ROM, following the KallistiOS SCIF sequence exactly.
!
! KallistiOS drives dcload-serial on real Dreamcast hardware with the same
! SH7091. Its scif_init() does three things ours never did, and starts from a
! different SR. None of them should matter according to the documentation --
! but they are free, and this burns the whole class of doubt in one test:
!
!   SR       = 0x500000F0   (register bank 0, like KOS; we used 0x700000F0)
!   SCFCR2   = 0x40         at the end (KOS value; we wrote 0)
!   SCSPTR2  = 0            "disable manual pin control" -- the register that
!                           can nail TXD2 to a fixed level (docs say it resets
!                           to 0, but this makes it certain)
!   SCFSR2   = 0x60         clear status without clearing TDFE/TEND
!   SCLSR2   = 0            clear the overrun latch
!   settle delay ~6x longer than ours
!
! Still emits 'U' (0x55) forever: a clean square wave on TXD.
! =============================================================================

    .section .text.start, "ax"
    .global _start
_start:
    ! SR = 0x500000F0, built without any memory access
    mov     #5, r0
    shll16  r0                      ! 0x00050000
    shll8   r0                      ! 0x05000000
    shll2   r0
    shll2   r0                      ! 0x50000000
    mov     #15, r1
    shll2   r1
    shll2   r1                      ! 0x000000F0
    or      r1, r0                  ! 0x500000F0
    ldc     r0, sr

    mov     #-24, r1
    shll16  r1                      ! r1 = 0xFFE80000 (SCIF base)

    mov     #0, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = 0     (TX/RX off, internal clock)
    mov     #6, r0
    mov.w   r0, @(24, r1)           ! SCFCR2 = 0x06  (reset both FIFOs)
    mov     #0, r0
    mov.w   r0, @(0, r1)            ! SCSMR2 = 0     (8N1)
    mov     #12, r0
    mov.b   r0, @(4, r1)            ! SCBRR2 = 12    (115200 @ Pck 50 MHz)

    mov     #4, r2                  ! long settle: 0x00040000 iterations
    shll16  r2
1:  dt      r2
    bf      1b

    mov     #0x40, r0
    mov.w   r0, @(24, r1)           ! SCFCR2 = 0x40  (KOS value)

    mov     r1, r2
    add     #32, r2                 ! r2 = 0xFFE80020 = SCSPTR2
    mov     #0, r0
    mov.w   r0, @r2                 ! SCSPTR2 = 0 -> pins under SCIF control

    mov.w   @(16, r1), r0           ! read SCFSR2 before clearing
    mov     #0x60, r0
    mov.w   r0, @(16, r1)           ! clear status, keep TDFE/TEND

    add     #4, r2                  ! r2 = 0xFFE80024 = SCLSR2
    mov.w   @r2, r0                 ! read before clearing
    mov     #0, r0
    mov.w   r0, @r2                 ! SCLSR2 = 0 (clear overrun latch)

    mov     #0x30, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = TE|RE

loop:
    mov     #1, r6
    shll16  r6
2:  mov.w   @(16, r1), r0
    tst     #0x20, r0               ! room in TX FIFO?
    bf      3f
    dt      r6
    bf      2b
3:  mov     #0x55, r0               ! 'U' -> square wave
    mov.b   r0, @(12, r1)
    mov     #0x60, r0
    mov.w   r0, @(16, r1)           ! clear status the KOS way
    bra     loop
    nop
