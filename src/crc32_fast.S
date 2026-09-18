! CRC-32 of the boot EPROM, hand-written.
!
! This loop is the longest single test in the suite, and it is the one place
! where instruction COUNT translates almost one-for-one into wall time: the
! ROM is linked at P2 (0xA0000000), which the SH-4 never caches, so every
! instruction the loop executes is a fetch from the EPROM across the bus.
! Running it from P1 instead was tried and does not work on real Naomi
! hardware -- black screen, no border colour -- so the only lever left is to
! execute fewer instructions.
!
! Three things the compiler was doing badly, in order of what they cost:
!
!   * the loop it generated for the nibble table straddled a 0x4D0-byte gap,
!     so every one of the eight steps per word paid an unconditional `bra`
!     out and a `bf` back -- ten instructions per nibble, eighty per word;
!   * a nibble table means eight table steps per word where a byte table
!     needs four;
!   * the table was addressed at its link address, which is P2, so all eight
!     lookups per word were uncached EPROM reads. The ROM data was already
!     being read through P1; the table was not.
!
! What is left is 26 instructions per 32-bit word, unrolled four words deep,
! with the table read through P1 so it lives in the operand cache.
!
! Measured under MAME (emulated seconds for the whole 2 MB, EN QUICK build):
!
!   nibble table, C, table at P2      3.807   <- what this replaced
!   byte table, C, table at P2        3.611
!   byte table, this file, table P1   3.579
!   no CRC at all, ROM read only      3.541   <- the floor
!
! So the arithmetic went from 0.266 s to 0.038 s, a factor of seven, and the
! whole test improved 6 % because 93 % of it is streaming 2 MB of EPROM.
! Read those numbers for what they are: MAME charges a flat cost per memory
! access and next to nothing for instruction fetch -- reading the ROM through
! P2 instead of P1 measures the same 3.579 s, which on real silicon it could
! not. On a real board this loop's instruction fetches are uncached EPROM
! cycles and its table reads now hit the operand cache, neither of which MAME
! represents, so the real gain is larger than 6 % by an amount only the board
! can tell us.
!
! u32 crc32_rom_block(const u32 *src, u32 nquads, u32 crc)
!   src    P1 pointer into the ROM, advanced by the routine
!   nquads number of GROUPS OF FOUR words (the caller owns the remainder)
!   crc    running CRC, returned updated -- not inverted, not seeded here
!
! r4 src   r5 nquads   r6 crc   r1 table (P1)   r0/r2/r3 scratch

    .text
    .align  2
    .global _crc32_rom_block
    .type   _crc32_rom_block, @function

! One byte of the CRC: index the table with the low byte, shift the CRC down,
! fold the entry in. r0 is forced because @(r0,rn) is the only indexed load
! the SH-4 offers.
    .macro  CRCBYTE
    mov     r6, r0
    extu.b  r0, r0
    shll2   r0
    mov.l   @(r0,r1), r3
    shlr8   r6
    xor     r3, r6
    .endm

    .macro  CRCWORD
    mov.l   @r4+, r2
    xor     r2, r6
    CRCBYTE
    CRCBYTE
    CRCBYTE
    CRCBYTE
    .endm

_crc32_rom_block:
    mov.l   Lcrctab, r1             ! table at its P2 link address
    mov.l   Lp1mask, r2
    tst     r5, r5
    bt      2f
    and     r2, r1                  ! ... read it through P1 so it caches
1:
    CRCWORD
    CRCWORD
    CRCWORD
    CRCWORD
    dt      r5
    bf      1b
2:
    rts
    mov     r6, r0

    .align  2
Lcrctab:    .long   _crc32_tab8
Lp1mask:    .long   0xDFFFFFFF      ! clear bit 29: P2 -> P1

    .size   _crc32_rom_block, . - _crc32_rom_block
