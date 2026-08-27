! =============================================================================
! Minimal serial bring-up ROM for the SEGA Naomi.
!
! Does NOTHING except initialise the SH-4 SCIF and transmit a message in an
! endless loop: no cache setup, no stack, no memory of any kind, no tests.
! Roughly a hundred bytes of code, all inside the first instructions after
! reset.
!
! Purpose: separate "the CPU is not executing my EPROM at all" from "the
! diagnostic ROM stops somewhere". If a scope on the real TXD pin (CN1 pin
! B48, or CN8 pin 2 only when JP9-JP13 are set to 2-3) shows no transitions
! with THIS ROM burned, the fault is the burn, the chip or the socket -- not
! the software.
!
! The message repeats forever, so triggering the scope is easy: any sweep
! catches a burst. 115200 8N1.
! =============================================================================

    .section .text.start, "ax"
    .global _start
_start:
    mov.l   Lscif, r1           ! r1 = 0xFFE80000
    mov     #0, r0
    mov.w   r0, @(8, r1)        ! SCSCR2 = 0  (TX/RX off, internal clock)
    mov     #6, r0
    mov.w   r0, @(24, r1)       ! SCFCR2 = TFRST|RFRST (reset both FIFOs)
    mov     #0, r0
    mov.w   r0, @(0, r1)        ! SCSMR2 = 0  (8N1, Pck/1)
    mov     #12, r0
    mov.b   r0, @(4, r1)        ! SCBRR2 = 12 (Pck 50MHz -> 115200 baud)
    mov.l   Lsettle, r2         ! settle > 1 bit time
1:  dt      r2
    bf      1b
    mov     #0, r0
    mov.w   r0, @(24, r1)       ! SCFCR2 = 0  (release FIFO reset)
    mov.w   r0, @(16, r1)       ! SCFSR2 = 0  (clear status)
    mov     #0x30, r0
    mov.w   r0, @(8, r1)        ! SCSCR2 = TE|RE

loop:
    mov.l   Lmsg, r4
    bsr     puts
    nop
    mov.l   Lpause, r2          ! short gap between messages
2:  dt      r2
    bf      2b
    bra     loop
    nop

! puts: r4 = NUL-terminated string. No stack, no RAM. Bounded FIFO wait.
puts:
    mov.l   Lscif, r1
    mov.l   Lkeep, r5
1:  mov.b   @r4+, r3
    tst     r3, r3
    bt      9f
    mov.l   Lspin, r6
2:  mov.w   @(16, r1), r0       ! SCFSR2
    tst     #0x20, r0           ! room in TX FIFO?
    bf      3f
    dt      r6
    bf      2b
3:  mov     r3, r0
    mov.b   r0, @(12, r1)       ! SCFTDR2
    mov.w   @(16, r1), r0
    and     r5, r0
    mov.w   r0, @(16, r1)
    bra     1b
    nop
9:  rts
    nop

    .align 4
Lscif:   .long 0xFFE80000
Lsettle: .long 0x00010000
Lspin:   .long 0x00020000
Lkeep:   .long 0x0000FF9F
Lpause:  .long 0x00400000
Lmsg:    .long msg
msg:
    .asciz "NAOMI SERIAL TEST -- SH4 SCIF alive, 115200 8N1\r\n"
