! =============================================================================
! naomi-diag crt0.S — reset entry for the SEGA Naomi diagnostic BIOS ROM
!
! Constraint: NO external RAM may be touched until it has been tested.
! Execution starts at 0xA0000000 (P2, uncached, maps ROM offset 0).
! Bring-up order:
!   1. SR: privileged, exceptions blocked (an exception restarts the ROM,
!      which re-prints the banner — itself a useful diagnostic signal)
!   2. MMU off, operand cache configured as 8KB on-chip RAM (ORA=1, OIX=1)
!      -> 4KB at 0x7C000000 (stack), 4KB at 0x7E000000 (.bss)
!   3. SCIF init + banner: the SCIF is fully internal to the SH4, it is the
!      only output channel that requires no working external memory at all
!   4. OC-RAM self-test (registers only); failure is reported and halts
!   5. Stack up, .bss cleared, jump to C (cmain)
! =============================================================================

#include "version.inc"
#include "strings.h"

    .section .text.start, "ax"
    .global _start
_start:
    mov.l   Lsr_init, r0
    ldc     r0, sr              ! MD=1 RB=1 BL=1 IMASK=15
    mov.l   Lmmucr, r1
    mov     #0, r0
    mov.l   r0, @r1             ! MMUCR = 0 (MMU off)
    mov.l   Lccr, r1
    mov.l   Lccr_val, r0
    mov.l   r0, @r1             ! CCR = ICE|ICI|OCE|OCI|ORA|OIX
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop

! ---------------------------------------------------------------------------
! SCIF init: 115200 8N1, FIFOs on. Registers only, no memory involved.
! ---------------------------------------------------------------------------
scif_setup:
    mov.l   Lscif, r1           ! r1 = 0xFFE80000
    mov     #0, r0
    mov.w   r0, @(8, r1)        ! SCSCR2  = 0      (TX/RX off, internal clock)
    mov     #6, r0
    mov.w   r0, @(24, r1)       ! SCFCR2  = TFRST|RFRST (reset both FIFOs)
    mov     #0, r0
    mov.w   r0, @(0, r1)        ! SCSMR2  = 0      (8N1, Pck/1)
    mov     #12, r0
    mov.b   r0, @(4, r1)        ! SCBRR2  = 12     (Pck 50MHz -> 115200 baud)
    mov.l   Ldelay, r2          ! wait > 1 bit time
1:  dt      r2
    bf      1b
    mov     #0, r0
    mov.w   r0, @(24, r1)       ! SCFCR2  = 0      (release FIFO reset)
    mov.w   r0, @(16, r1)       ! SCFSR2  = 0      (clear status flags)
    mov     #0x30, r0
    mov.w   r0, @(8, r1)        ! SCSCR2  = TE|RE

    mov.l   Lbanner, r4
    bsr     asm_puts
    nop

! ---------------------------------------------------------------------------
! OC-RAM self-test: both 4KB banks, first and last word, two patterns.
! Uses only registers. If the CPU cache is bad, the CPU itself is unusable:
! report on SCIF and halt.
! ---------------------------------------------------------------------------
ocram_test:
    mov.l   Locram0, r2         ! 0x7C000000  (bank 0 start)
    mov.l   Locram0e, r3        ! 0x7C000FFC  (bank 0 end)
    mov.l   Locram1, r6         ! 0x7E000000  (bank 1 start)
    mov.l   Locram1e, r7        ! 0x7E000FFC  (bank 1 end)
    mov.l   Lpat1, r0
    mov.l   Lpat2, r1
    mov.l   r0, @r2             ! distinct values in each cell to catch
    mov.l   r1, @r3             ! aliasing between the two banks
    not     r0, r4
    not     r1, r5
    mov.l   r4, @r6
    mov.l   r5, @r7
    mov.l   @r2, r8
    cmp/eq  r0, r8
    bf      ocram_fail
    mov.l   @r3, r8
    cmp/eq  r1, r8
    bf      ocram_fail
    mov.l   @r6, r8
    cmp/eq  r4, r8
    bf      ocram_fail
    mov.l   @r7, r8
    cmp/eq  r5, r8
    bf      ocram_fail

    mov.l   Locram_ok, r4
    bsr     asm_puts
    nop

! ---------------------------------------------------------------------------
! Stack in OC-RAM bank 0, .bss in bank 1, then C.
! ---------------------------------------------------------------------------
    mov.l   Lstack, r15         ! SP = 0x7C001000 (top of bank 0)
    mov.l   Lbss_s, r2
    mov.l   Lbss_e, r3
    mov     #0, r0
1:  cmp/hs  r3, r2              ! T = (r2 >= r3)
    bt      2f
    mov.l   r0, @r2
    bra     1b
    add     #4, r2
2:  mov.l   Lcmain, r0
    jsr     @r0
    nop
halt:
    bra     halt
    nop

ocram_fail:
    mov.l   Locram_ko, r4
    bsr     asm_puts
    nop
    bra     halt
    nop

! ---------------------------------------------------------------------------
! asm_puts: send NUL-terminated string at r4 to the SCIF.
! Clobbers r0,r1,r3,r5,r6. No stack, no RAM.
! The TDFE wait is BOUNDED: exits early when the FIFO frees up (real HW),
! or times out and writes anyway (diag must never hang on its own console;
! also works around emulators with partial SCIF models).
! ---------------------------------------------------------------------------
    .global asm_puts
asm_puts:
    mov.l   Lscif, r1
    mov.l   Lfsr_keep, r5       ! ~ (TDFE|TEND) mask
1:  mov.b   @r4+, r3
    tst     r3, r3
    bt      9f
    mov.l   Ltxspin, r6         ! bounded wait for TX FIFO room
2:  mov.w   @(16, r1), r0       ! SCFSR2
    tst     #0x20, r0           ! TDFE set (room in TX FIFO)?
    bf      3f
    dt      r6
    bf      2b
3:  mov     r3, r0
    mov.b   r0, @(12, r1)       ! SCFTDR2 = char
    mov.w   @(16, r1), r0
    and     r5, r0
    mov.w   r0, @(16, r1)       ! clear TDFE|TEND
    bra     1b
    nop
9:  rts
    nop

    .align 4
Lsr_init:   .long 0x700000F0
Lmmucr:     .long 0xFF000010
Lccr:       .long 0xFF00001C
Lccr_val:   .long 0x000009A9    ! ICI|ICE|ORA|OIX|OCI|OCE
Lscif:      .long 0xFFE80000
Ldelay:     .long 0x00010000
Ltxspin:    .long 0x00020000
Lfsr_keep:  .long 0x0000FF9F
Locram0:    .long 0x7C000000
Locram0e:   .long 0x7C000FFC
Locram1:    .long 0x7E000000
Locram1e:   .long 0x7E000FFC
Lpat1:      .long 0x5AA51234
Lpat2:      .long 0xC33C9876
Lstack:     .long 0x7C001000
Lbss_s:     .long __bss_start
Lbss_e:     .long __bss_end
Lcmain:     .long _cmain
Lbanner:    .long banner_str
Locram_ok:  .long ocram_ok_str
Locram_ko:  .long ocram_ko_str

banner_str:
    .ascii S_BANNER_A
    .ascii DIAG_VERSION
    .asciz S_BANNER_B
ocram_ok_str:
    .asciz S_BOOT_OCRAM_OK
ocram_ko_str:
    .asciz S_BOOT_OCRAM_KO
