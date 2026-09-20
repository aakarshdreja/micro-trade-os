/* ================================================================
 * linker.ld: Custom linker script for STM32F405 (Cortex-M4)
 *
 * This file defines:
 *   - Physical memory regions of the microcontroller
 *   - Placement of code and data sections
 *   - Symbols used by startup code for runtime initialization
 *
 * The linker reads this file with the -T flag in the Makefile.
 * ================================================================ */
 
/* ENTRY: tells the linker the name of the first function to execute.
 * This is placed in the ELF header as the entry point address.
 * The hardware doesn't use this. It reads the vector table instead.
 * But debuggers (GDB) and QEMU use it to know where to start. */
ENTRY(Reset_Handler)
 
/* ================================================================
 * MEMORY REGIONS
 * Define the physical address space of the STM32F405.
 * (rx)  = readable + executable  (Flash: code lives here)
 * (rwx) = readable + writable + executable  (RAM: data lives here)
 * LENGTH values must match the actual chip specifications.
 * ================================================================ */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
 
/* ================================================================
 * _estack: Initial Stack Pointer value
 *
 * The Cortex-M4 uses a Full Descending stack:
 *   - Stack pointer starts at the TOP of RAM
 *   - Each PUSH decrements SP before storing
 *
 * Top of RAM = start of RAM + size of RAM
 *            = 0x20000000 + 0x20000 = 0x20020000
 *
 * This symbol is the first word in the vector table (.word _estack).
 * At reset, hardware loads this value into the SP register.
 * ================================================================ */
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* = 0x20020000 */
 
/* ================================================================
 * SECTIONS
 * Define how to map input sections (from .o files) to output
 * sections (in firmware.elf) and which memory region each goes in.
 * ================================================================ */
SECTIONS
{
    /* ============================================================
     * .isr_vector, Interrupt Vector Table
     *
     * MUST be the very first section in Flash (at 0x08000000).
     * The Cortex-M4 hardware reads from this fixed address at reset
     * to find the initial SP and reset vector.
     *
     * KEEP(): prevent --gc-sections from removing this section.
     * Even though no C code "calls" the vector table, it must
     * remain in the binary because the hardware reads it directly.
     * ============================================================ */
    .isr_vector :
    {
        . = ALIGN(4);              /* Align to 4-byte boundary         */
        KEEP(*(.isr_vector))       /* Include .isr_vector from all .o  */
        . = ALIGN(4);              /* Keep end aligned                 */
    } > FLASH                     /* Place this section in Flash      */
 
    /* ============================================================
     * .text, Executable code
     *
     * Contains all compiled machine instructions from your .c and .s
     * files. Runs directly from Flash (no copy needed, Flash is
     * directly addressable by the CPU for instruction fetch).
     * ============================================================ */
    .text :
    {
        . = ALIGN(4);
        *(.text)          /* .text section from all object files      */
        *(.text*)         /* .text.Reset_Handler, .text.main, etc.    */
        . = ALIGN(4);
    } > FLASH
 
    /* ============================================================
     * .rodata, Read-Only Data
     *
     * Contains constants and string literals. For example, the
     * string "Boot OK\r\n" passed to sh_puts() lives here.
     * Like .text, runs directly from Flash, no copy needed.
     * ============================================================ */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)        /* Read-only data from all object files     */
        *(.rodata*)       /* Catches .rodata.str1.4 etc.              */
        . = ALIGN(4);
    } > FLASH
 
    /* ============================================================
     * .data, Initialized Variables
     *
     * Contains global and static variables that have initial values,
     * such as: int initialized = 123;
     *
     * IMPORTANT, Two addresses involved:
     *   LMA (Load Memory Address): where the data IMAGE is stored
     *       in Flash (right after .rodata). This is _sidata.
     *   VMA (Virtual Memory Address): where the data lives at RUNTIME
     *       in RAM. This is _sdata .. _edata.
     *
     * The "AT> FLASH" directive creates this split:
     *   > RAM     → VMA is in RAM (runtime address)
     *   AT> FLASH → LMA is in Flash (storage address)
     *
     * Reset_Handler copies bytes from _sidata (Flash) to _sdata
     * through _edata (RAM) before main() is called.
     * ============================================================ */
    _sidata = LOADADDR(.data);  /* Flash address of .data image. Used by Reset_Handler */
 
    .data :
    {
        . = ALIGN(4);
        _sdata = .;             /* Mark start of .data in RAM (_sdata = VMA start) */
        *(.data)                /* All .data from object files                     */
        *(.data*)               /* Catches .data.rel.ro etc.                       */
        . = ALIGN(4);
        _edata = .;             /* Mark end of .data in RAM (_edata = VMA end)     */
    } > RAM AT> FLASH           /* VMA in RAM, LMA in Flash                       */
 
    /* ============================================================
     * .bss, Zero-Initialized Variables
     *
     * Contains global and static variables with NO explicit initial
     * value: int uninitialized; static int state;
     *
     * The C standard says these must be zero before main() runs.
     * They have NO Flash image (no data to store, they're all zero).
     * Reset_Handler simply writes 0x00 to all bytes in this range.
     *
     * NOLOAD attribute tells the linker: this section is not loaded
     * from the binary (no Flash image), it is only allocated in RAM.
     * ============================================================ */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;              /* Mark start of .bss (_sbss used by Reset_Handler) */
        *(.bss)                 /* Uninitialized globals and statics                */
        *(.bss*)
        *(COMMON)               /* Common symbols (some old C patterns use these)   */
        . = ALIGN(4);
        _ebss = .;              /* Mark end of .bss (_ebss used by Reset_Handler)   */
    } > RAM
}
