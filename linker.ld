/* naomi-diag: 2MB BIOS EPROM, executed in place at 0xA0000000 (P2).
 * .bss goes to OC-RAM bank 1 (SH4 operand cache as RAM, 4KB).
 * No .data allowed: the ROM must not depend on any external RAM. */
OUTPUT_FORMAT("elf32-shl")
OUTPUT_ARCH(sh4)
ENTRY(_start)

MEMORY
{
    ROM    (rx) : ORIGIN = 0xA0000000, LENGTH = 2M
    OCRAM1 (rw) : ORIGIN = 0x7E000000, LENGTH = 4K
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
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
