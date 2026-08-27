! =============================================================================
! Minimal video + serial sign-of-life ROM for the SEGA Naomi.
!
! Uses NO memory at all -- not one byte of RAM, no stack, no framebuffer.
! The PowerVR sync generator runs from its registers alone, and with the
! framebuffer read disabled the whole screen shows VO_BORDER_COL. So a solid
! colour can be displayed without any working RAM whatsoever.
!
! It alternates the two monitor standards every few seconds, because the
! Naomi BIOS picks between them by reading DIP switch 1 and we cannot know
! which one your cabinet uses:
!
!     RED screen   -> your monitor accepted 31 kHz (VGA)
!     GREEN screen -> your monitor accepted 15 kHz (standard arcade)
!
! Whichever colour appears tells us both that the CPU is executing this
! EPROM and which mode your monitor wants. If neither appears but the
! original BIOS shows a picture, the CPU is not running this ROM.
!
! Serial (115200 8N1) is driven in parallel, so the same ROM doubles as a
! serial test if you find the right pin.
!
! Register values: 31 kHz from the original BIOS; 15 kHz deltas from
! libnaomi (pixel-clock doubling off, SPG 15 kHz + interlace bits, shifted
! start position).
! =============================================================================

    .section .text.start, "ax"
    .global _start
_start:
! ---- SCIF: 115200 8N1 -------------------------------------------------------
    mov.l   Lscif, r1
    mov     #0, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = 0
    mov     #6, r0
    mov.w   r0, @(24, r1)           ! SCFCR2 = reset FIFOs
    mov     #0, r0
    mov.w   r0, @(0, r1)            ! SCSMR2 = 0
    mov     #12, r0
    mov.b   r0, @(4, r1)            ! SCBRR2 = 12
    mov.l   Lsettle, r2
1:  dt      r2
    bf      1b
    mov     #0, r0
    mov.w   r0, @(24, r1)
    mov.w   r0, @(16, r1)
    mov     #0x30, r0
    mov.w   r0, @(8, r1)            ! SCSCR2 = TE|RE

    mov.l   Lhello, r4
    bsr     puts
    nop

loop:
    mov.l   Ltab31, r4              ! 31 kHz VGA, red screen
    bsr     apply
    nop
    mov.l   Lmsg31, r4
    bsr     puts
    nop
    mov.l   Lpause, r2
1:  dt      r2
    bf      1b

    mov.l   Ltab15, r4              ! 15 kHz arcade, green screen
    bsr     apply
    nop
    mov.l   Lmsg15, r4
    bsr     puts
    nop
    mov.l   Lpause, r2
2:  dt      r2
    bf      2b

    bra     loop
    nop

! apply: r4 -> table of (address, value) longword pairs, end marked by 0.
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

! puts: r4 = NUL-terminated string. No stack, no RAM, bounded FIFO wait.
puts:
    mov.l   Lscif, r1
    mov.l   Lkeep, r5
1:  mov.b   @r4+, r3
    tst     r3, r3
    bt      9f
    mov.l   Lspin, r6
2:  mov.w   @(16, r1), r0
    tst     #0x20, r0
    bf      3f
    dt      r6
    bf      2b
3:  mov     r3, r0
    mov.b   r0, @(12, r1)
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
Lpause:  .long 0x00600000
Ltab31:  .long tab31
Ltab15:  .long tab15
Lhello:  .long hello_str
Lmsg31:  .long msg31_str
Lmsg15:  .long msg15_str

    .align 4
tab31:
    .long 0xA05F8008, 0x00000000    ! SOFTRESET: release
    .long 0xA05F80A4, 0x0000001F    ! VRAM arbiter
    .long 0xA05F80A8, 0x15D1C951    ! VRAM config
    .long 0xA05F80A0, 0x00000020    ! VRAM refresh
    .long 0xA05F80E8, 0x00160008    ! VO_CONTROL: blank while we reconfigure
    .long 0xA05F8044, 0x00800000    ! FB_R_CTRL: pixel clock x2 (31k), FB read OFF
    .long 0xA05F80D0, 0x00000100    ! SPG_CONTROL: 31 kHz, non-interlaced
    .long 0xA05F80D8, 0x020C0359    ! SPG_LOAD   (524 x 857)
    .long 0xA05F80D4, 0x007E0345    ! SPG_HBLANK
    .long 0xA05F80DC, 0x00280208    ! SPG_VBLANK
    .long 0xA05F80E0, 0x03F1933F    ! SPG_WIDTH
    .long 0xA05F80EC, 0x000000A6    ! VO_STARTX
    .long 0xA05F80F0, 0x00230023    ! VO_STARTY
    .long 0xA05F8040, 0x00FF0000    ! VO_BORDER_COL: RED
    .long 0xA05F80E8, 0x00160000    ! VO_CONTROL: display on
    .long 0x00000000

tab15:
    .long 0xA05F8008, 0x00000000
    .long 0xA05F80A4, 0x0000001F
    .long 0xA05F80A8, 0x15D1C951
    .long 0xA05F80A0, 0x00000020
    .long 0xA05F80E8, 0x00160008    ! blank while we reconfigure
    .long 0xA05F8044, 0x00000000    ! FB_R_CTRL: NO clock doubling (15k), FB read OFF
    .long 0xA05F80D0, 0x00000150    ! SPG_CONTROL: 15 kHz + interlace
    .long 0xA05F80D8, 0x020C0359
    .long 0xA05F80D4, 0x007E0345
    .long 0xA05F80DC, 0x00280208
    .long 0xA05F80E0, 0x03F1933F
    .long 0xA05F80EC, 0x000000A4    ! VO_STARTX (15 kHz)
    .long 0xA05F80F0, 0x00160016    ! VO_STARTY (15 kHz)
    .long 0xA05F8040, 0x0000FF00    ! VO_BORDER_COL: GREEN
    .long 0xA05F80E8, 0x00160000
    .long 0x00000000

hello_str:
    .asciz "\r\nNAOMI VIDEO TEST -- alternating 31kHz (red) / 15kHz (green)\r\n"
msg31_str:
    .asciz "31kHz VGA  : screen should be RED\r\n"
msg15_str:
    .asciz "15kHz CRT  : screen should be GREEN\r\n"
