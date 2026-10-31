/*
 * The loader's layout.
 *
 * The loader runs from RAM, chained in front of Android's kernel in boot_a: LK enters it as a
 * Linux arm64 Image at 0x40080000, with the MMU and the caches off. The link address is the
 * place the phone's own preloader put it, so it is the phone's fact, not ours.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __loader_end = .;
}
