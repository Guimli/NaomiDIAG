@ =============================================================================
@ Self-test and sound RAM test run BY the AICA's ARM7 (IC33).
@
@ The SH-4 reaches the sound RAM only across G2, one FIFO-paced access at a
@ time -- eight minutes for the 8 MB. The ARM7 sits on that RAM's own bus.
@ So, as with the CPU RAM: the SH-4 proves a small window first (0-16 KB),
@ copies this program into it and releases the ARM7 from reset. The ARM
@ then tests itself using that window only -- registers, its banked
@ registers, the ALU, byte and word access to the window -- and, if all of
@ that holds, tests the rest of the sound RAM with the patterns the SH-4
@ would have used, reporting through the mailbox below. The SH-4 only reads
@ the mailbox and keeps a timeout.
@
@ ARM7DI = ARMv3: no Thumb, no halfword loads, no long multiply.
@ Everything lives below 0x4000; nothing above it is touched before the
@ self-test has passed.
@ =============================================================================

        .equ    MB,        0x3F00       @ mailbox (see aica_arm.h)
        .equ    MB_SIG,    0x00         @ 'ARM7' once running
        .equ    MB_STEP,   0x04         @ step under way
        .equ    MB_STATUS, 0x08         @ 0 run, 0xDEAD00nn fail, DONE
        .equ    MB_PROG,   0x0C         @ address reached (full test)
        .equ    MB_ERRS,   0x10
        .equ    MB_BAD,    0x14
        .equ    MB_NFAIL,  0x18
        .equ    MB_FAILS,  0x1C         @ 8 x {addr, exp, got}
        .equ    MB_START,  0x80         @ from the SH-4: first address
        .equ    MB_END,    0x84         @ from the SH-4: end address
        .equ    MB_SEED,   0x88         @ from the SH-4: PRNG seed
        .equ    MB_DETAIL, 0x8C         @ self-test: which check failed
        .equ    MB_TICKS,  0x90         @ loop counter for the speed check
        .equ    STACK,     0x3E00
        .equ    SCR_LO,    0x2000       @ own-RAM scratch, inside the window
        .equ    SCR_HI,    0x3E00
        .equ    SIG,       0x41524D37   @ 'ARM7'
        .equ    DONE,      0x600D600D

        .arm
        .text
        .global _arm_start
_arm_start:
        b       reset                   @ 0x00 reset
        b       exc_und                 @ 0x04 undefined instruction
        b       exc_swi                 @ 0x08 SWI
        b       exc_pabt                @ 0x0C prefetch abort
        b       exc_dabt                @ 0x10 data abort
        b       exc_res                 @ 0x14 reserved (address exception)
        b       exc_irq                 @ 0x18 IRQ
        b       exc_fiq                 @ 0x1C FIQ

exc_und:  mov   r0, #0xE1
          b     exc
exc_swi:  mov   r0, #0xE2
          b     exc
exc_pabt: mov   r0, #0xE3
          b     exc
exc_dabt: mov   r0, #0xE4
          b     exc
exc_res:  mov   r0, #0xE5
          b     exc
exc_irq:  mov   r0, #0xE6
          b     exc
exc_fiq:  mov   r0, #0xE7
exc:      mov   r12, #MB
          ldr   r1, =0xDEAD0000
          orr   r1, r1, r0
          str   r1, [r12, #MB_STATUS]
1:        b     1b

reset:
        mov     r12, #MB
        ldr     r0, =SIG
        str     r0, [r12, #MB_SIG]
        mov     sp, #STACK

@ ---- step 1: registers, then the banked ones of FIQ, IRQ and SVC -------------
        mov     r0, #1
        str     r0, [r12, #MB_STEP]
        ldr     r0, =0x55555555
        bl      regs_check
        ldr     r0, =0xAAAAAAAA
        bl      regs_check
        ldr     r0, =0x0F0F0F0F
        bl      regs_check
        @ banked r13/r14 (and r8-r12 in FIQ): a value in each mode must
        @ survive a visit to the others
        mrs     r4, cpsr
        bic     r5, r4, #0x1F
        orr     r6, r5, #0x11           @ FIQ
        msr     cpsr_c, r6
        ldr     r8, =0x8F18F18F
        ldr     r13, =0x13F13F13
        orr     r6, r5, #0x12           @ IRQ
        msr     cpsr_c, r6
        ldr     r13, =0x13113113
        ldr     r14, =0x14114114
        msr     cpsr_c, r4              @ back to SVC
        orr     r6, r5, #0x11
        msr     cpsr_c, r6
        ldr     r0, =0x8F18F18F
        cmp     r8, r0
        ldreq   r0, =0x13F13F13
        cmpeq   r13, r0
        orr     r6, r5, #0x12
        msr     cpsr_c, r6
        ldreq   r0, =0x13113113
        cmpeq   r13, r0
        ldreq   r0, =0x14114114
        cmpeq   r14, r0
        msr     cpsr_c, r4
        mov     r12, #MB
        mov     sp, #STACK
        movne   r0, #0x12
        bne     selftest_fail

@ ---- step 2: ALU and multiplier, against results worked out beforehand -------
        mov     r0, #2
        str     r0, [r12, #MB_STEP]
        mov     r9, #0
        ldr     r1, =0x7FFFFFFF
        adds    r2, r1, #1              @ 0x80000000, N and V set, C clear
        bvc     alu_bad
        add     r9, r9, #1
        bcs     alu_bad
        add     r9, r9, #1
        bpl     alu_bad
        add     r9, r9, #1
        ldr     r3, =0x80000000
        cmp     r2, r3
        bne     alu_bad
        add     r9, r9, #1
        ldr     r1, =0xFFFFFFFF
        adds    r2, r1, #1              @ 0, Z and C set
        bne     alu_bad
        add     r9, r9, #1
        bcc     alu_bad
        add     r9, r9, #1
        mov     r1, #0x81
        mov     r2, r1, lsl #24         @ 0x81000000
        mov     r3, r2, asr #4          @ 0xF8100000
        ldr     r4, =0xF8100000
        cmp     r3, r4
        bne     alu_bad
        add     r9, r9, #1
        mov     r3, r2, lsr #4          @ 0x08100000
        cmp     r3, #0x08100000
        bne     alu_bad
        add     r9, r9, #1
        mov     r3, r2, ror #28         @ 0x10000008
        ldr     r4, =0x10000008
        cmp     r3, r4
        bne     alu_bad
        add     r9, r9, #1
        ldr     r1, =0x12345678
        ldr     r2, =0x9ABCDEF1
        mul     r3, r1, r2              @ low 32 bits
        ldr     r4, =0x366176F8
        cmp     r3, r4
        bne     alu_bad
        add     r9, r9, #1
        mla     r3, r1, r2, r1          @ + 0x12345678
        ldr     r4, =0x4895CD70
        cmp     r3, r4
        bne     alu_bad
        add     r9, r9, #1
        ldr     r1, =0xC3C3C3C3
        eor     r2, r1, r1, ror #8
        ldr     r4, =0x00000000
        cmp     r2, r4
        bne     alu_bad
        add     r9, r9, #1
        ldr     r1, =0x0F0F0F0F
        bic     r2, r1, #0x0F
        ldr     r4, =0x0F0F0F00
        cmp     r2, r4
        bne     alu_bad
        add     r9, r9, #1
        ldr     r1, =0x00000064
        subs    r2, r1, #0x65           @ -1, N set, C clear
        bpl     alu_bad
        add     r9, r9, #1
        bcs     alu_bad
        add     r9, r9, #1
        b       alu_ok
alu_bad:
        add     r0, r9, #0x20           @ 0x20 + index of the failed branch
        b       selftest_fail
alu_ok:

@ ---- step 3: the window, through the ARM's own bus: bytes and words ----------
        mov     r0, #3
        str     r0, [r12, #MB_STEP]
        ldr     r1, =SCR_LO
        ldr     r2, =SCR_HI
        mov     r3, #0
1:      and     r4, r3, #0xFF           @ byte i = i & 0xFF, ^ 0xA5
        eor     r4, r4, #0xA5
        strb    r4, [r1, r3]
        add     r3, r3, #1
        add     r5, r1, r3
        cmp     r5, r2
        blo     1b
        mov     r3, #0                  @ read back as words
2:      ldr     r6, [r1, r3]
        and     r4, r3, #0xFF
        eor     r7, r4, #0xA5
        add     r4, r3, #1
        and     r4, r4, #0xFF
        eor     r4, r4, #0xA5
        orr     r7, r7, r4, lsl #8
        add     r4, r3, #2
        and     r4, r4, #0xFF
        eor     r4, r4, #0xA5
        orr     r7, r7, r4, lsl #16
        add     r4, r3, #3
        and     r4, r4, #0xFF
        eor     r4, r4, #0xA5
        orr     r7, r7, r4, lsl #24
        cmp     r6, r7
        movne   r0, #0x31
        bne     selftest_fail
        add     r3, r3, #4
        add     r5, r1, r3
        cmp     r5, r2
        blo     2b
        ldr     r6, =0x5AC3F00F          @ words written, bytes read
        str     r6, [r1]
        ldrb    r7, [r1]
        cmp     r7, #0x0F
        ldreqb  r7, [r1, #1]
        cmpeq   r7, #0xF0
        ldreqb  r7, [r1, #2]
        cmpeq   r7, #0xC3
        ldreqb  r7, [r1, #3]
        cmpeq   r7, #0x5A
        movne   r0, #0x32
        bne     selftest_fail

@ ---- step 4: speed, for the SH-4 to time: a fixed count of loop turns -------
        mov     r0, #4
        str     r0, [r12, #MB_STEP]
        ldr     r1, =100000
        mov     r2, #0
3:      add     r2, r2, #1
        subs    r1, r1, #1
        bne     3b
        str     r2, [r12, #MB_TICKS]

@ ---- step 5..7: the rest of the sound RAM -----------------------------------
        mov     r0, #5
        str     r0, [r12, #MB_STEP]
        ldr     r8, =0x55555555
        bl      pattern_pass
        mov     r0, #6
        str     r0, [r12, #MB_STEP]
        ldr     r8, =0xAAAAAAAA
        bl      pattern_pass
        mov     r0, #7
        str     r0, [r12, #MB_STEP]
        bl      prng_pass

        ldr     r0, =DONE
        str     r0, [r12, #MB_STATUS]
9:      b       9b

selftest_fail:
        str     r0, [r12, #MB_DETAIL]
        ldr     r1, =0xDEAD0000
        ldr     r2, [r12, #MB_STEP]
        orr     r1, r1, r2
        str     r1, [r12, #MB_STATUS]
8:      b       8b

@ r0 = pattern. Every general register r1-r11 (and r0 itself) gets a value
@ derived from it; the sum of what comes back must be the expected one.
regs_check:
        mov     r1, r0, ror #1
        mov     r2, r0, ror #2
        mov     r3, r0, ror #3
        mov     r4, r0, ror #4
        mov     r5, r0, ror #5
        mov     r6, r0, ror #6
        mov     r7, r0, ror #7
        mov     r8, r0, ror #8
        mov     r9, r0, ror #9
        mov     r10, r0, ror #10
        mov     r11, r0, ror #11
        eor     r1, r1, r0, ror #1      @ each must cancel to zero
        eor     r2, r2, r0, ror #2
        eor     r3, r3, r0, ror #3
        eor     r4, r4, r0, ror #4
        eor     r5, r5, r0, ror #5
        eor     r6, r6, r0, ror #6
        eor     r7, r7, r0, ror #7
        eor     r8, r8, r0, ror #8
        eor     r9, r9, r0, ror #9
        eor     r10, r10, r0, ror #10
        eor     r11, r11, r0, ror #11
        orr     r1, r1, r2
        orr     r1, r1, r3
        orr     r1, r1, r4
        orr     r1, r1, r5
        orr     r1, r1, r6
        orr     r1, r1, r7
        orr     r1, r1, r8
        orr     r1, r1, r9
        orr     r1, r1, r10
        orrs    r1, r1, r11
        movne   r0, #0x11
        bne     selftest_fail
        mov     pc, lr

@ ---- a failure: r9 = address, r6 = expected, r7 = read --------------------
record:
        stmfd   sp!, {r0-r3, lr}
        ldr     r0, [r12, #MB_ERRS]
        add     r0, r0, #1
        str     r0, [r12, #MB_ERRS]
        ldr     r0, [r12, #MB_BAD]
        eor     r1, r6, r7
        orr     r0, r0, r1
        str     r0, [r12, #MB_BAD]
        ldr     r0, [r12, #MB_NFAIL]
        cmp     r0, #8
        bhs     1f
        add     r1, r0, r0, lsl #1      @ * 12
        mov     r1, r1, lsl #2
        add     r1, r1, #MB_FAILS
        add     r1, r1, r12
        stmia   r1, {r6, r7, r9}        @ stored as exp, got, addr
        add     r0, r0, #1
        str     r0, [r12, #MB_NFAIL]
1:      ldmfd   sp!, {r0-r3, pc}

@ ---- r8 = pattern over [MB_START, MB_END), 32 bytes at a time -------------
pattern_pass:
        stmfd   sp!, {lr}
        mov     r0, r8
        mov     r1, r8
        mov     r2, r8
        mov     r3, r8
        mov     r4, r8
        mov     r5, r8
        mov     r6, r8
        mov     r7, r8
        ldr     r10, [r12, #MB_START]
        ldr     r11, [r12, #MB_END]
1:      stmia   r10!, {r0-r7}
        cmp     r10, r11
        blo     1b
        ldr     r10, [r12, #MB_START]
2:      tst     r10, #0xFF00            @ progress every 64 KB
        tsteq   r10, #0x00FF
        streq   r10, [r12, #MB_PROG]
        ldmia   r10!, {r0-r7}
        cmp     r0, r8
        cmpeq   r1, r8
        cmpeq   r2, r8
        cmpeq   r3, r8
        cmpeq   r4, r8
        cmpeq   r5, r8
        cmpeq   r6, r8
        cmpeq   r7, r8
        blne    slow32
        cmp     r10, r11
        blo     2b
        ldmfd   sp!, {pc}

@ the block just read held a mismatch: word by word, from memory again
slow32:
        stmfd   sp!, {r0-r7, lr}
        sub     r9, r10, #32
        mov     r6, r8
3:      ldr     r7, [r9]
        cmp     r7, r6
        blne    record
        add     r9, r9, #4
        cmp     r9, r10
        blo     3b
        ldmfd   sp!, {r0-r7, pc}

@ ---- xorshift32 over [MB_START, MB_END), seed from the SH-4 ---------------
prng_pass:
        stmfd   sp!, {lr}
        ldr     r0, [r12, #MB_SEED]
        cmp     r0, #0
        moveq   r0, #1
        mov     r5, r0                  @ keep the seed for the read
        ldr     r10, [r12, #MB_START]
        ldr     r11, [r12, #MB_END]
1:      eor     r0, r0, r0, lsl #13
        eor     r0, r0, r0, lsr #17
        eor     r0, r0, r0, lsl #5
        str     r0, [r10], #4
        cmp     r10, r11
        blo     1b
        mov     r0, r5
        ldr     r10, [r12, #MB_START]
2:      tst     r10, #0xFF00
        tsteq   r10, #0x00FF
        streq   r10, [r12, #MB_PROG]
        eor     r0, r0, r0, lsl #13
        eor     r0, r0, r0, lsr #17
        eor     r0, r0, r0, lsl #5
        ldr     r7, [r10], #4
        cmp     r7, r0
        beq     3f
        sub     r9, r10, #4
        mov     r6, r0
        bl      record
3:      cmp     r10, r11
        blo     2b
        ldmfd   sp!, {pc}

        .ltorg
        .global _arm_end
_arm_end:
