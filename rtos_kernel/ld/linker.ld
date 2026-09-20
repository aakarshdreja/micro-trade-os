/* ================================================================
 * linker.ld: Custom linker script for STM32F407 (Cortex-M4)
 *
 * Adopted from the ELL365 bare-metal project. Defines the memory
 * map (1 MB Flash @ 0x08000000, 128 KB SRAM @ 0x20000000), places
 * the vector table first, and exports the symbols the startup code
 * and the RTOS kernel need.
 * ================================================================ */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* = 0x20020000 */

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ARM stack-unwind / exception tables (keeps GCC happy) */
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* End of statically allocated RAM. Exported for stack-usage
     * analysis; task stacks are caller-provided static arrays. */
    . = ALIGN(8);
    _end = .;
    _heap_start = .;
    PROVIDE(end = .);
}
