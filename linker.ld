/* naomi-diag: 2MB BIOS EPROM, executed in place from the ROM_BASE window.
 *
 * Default 0x80000000 = P1, the CACHED alias of area 0. The SH-4 boots at
 * 0xA0000000 (P2), which the hardware never caches -- so every instruction
 * fetch is an EPROM access. crt0 therefore jumps to the P1 alias of itself
 * as soon as CCR is programmed, and everything is linked for P1. Measured
 * consequence on real hardware: the 2 MB ROM checksum went from minutes to
 * seconds. Build with ROM_BASE=0xA0000000 for an uncached fallback image.
 * .bss goes to OC-RAM bank 1 (SH4 operand cache as RAM, 4KB).
 * No .data allowed: the ROM must not depend on any external RAM. */
OUTPUT_FORMAT("elf32-shl")
OUTPUT_ARCH(sh4)
ENTRY(_start)

MEMORY
{
    ROM    (rx) : ORIGIN = @ROM_BASE@, LENGTH = 2M
    OCRAM1 (rw) : ORIGIN = 0x7E000000, LENGTH = 4K
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text.reloc)
        *(.text*)
    } > ROM

    .rodata : {
        *(.rodata*)
    } > ROM

    .data : {
        *(.data*)
    } > ROM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > OCRAM1

    /DISCARD/ : { *(.comment) *(.note*) }
}

ASSERT(SIZEOF(.data) == 0, "no writable .data allowed (ROM runs without RAM)")
