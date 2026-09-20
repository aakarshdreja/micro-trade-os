# Part 1 : STM32F4 Bare-Metal Boot Sequence

Cortex-M4 startup from reset to `main()`, written from scratch. No vendor HAL,
no CMSIS, no C standard library.

This part is the foundation for the RTOS in Part 2. It establishes the boot
contract that the RTOS assumes on entry: a correct initial stack pointer, a
correct reset vector, initialised memory, and a working interrupt.

Author: Aakarsh D Reja (2023EE11151)

## What it does

1. The CPU resets. It reads the initial stack pointer from `0x08000000`.
2. It reads the `Reset_Handler` address from `0x08000004` and jumps there.
3. `Reset_Handler` copies `.data` from Flash to RAM.
4. It zeroes the `.bss` region in RAM.
5. It branches to `main()`.
6. `main()` starts SysTick and prints a heartbeat from the interrupt handler.

Two variables prove that step 3 and step 4 worked. `initialized` is set to 123
at compile time and lives in `.data`. `uninitialized` lives in `.bss`. If the
copy and the zeroing both worked, the first reads back as 123 and the second as
0. The program checks this and prints the result.

## Memory map

| Region | Start | Size | Contents |
|---|---|---|---|
| Flash | `0x08000000` | 1 MB | Vector table, code, rodata, `.data` image |
| RAM | `0x20000000` | 128 KB | Stack, `.data`, `.bss` |

The initial stack pointer is `0x20020000`, the top of RAM. The linker script
computes it.

## Build and run

```bash
make
```

```bash
make qemu
```

Exit QEMU with `Ctrl-A`, then `X`.

To debug, start a GDB server in one terminal:

```bash
make debug
```

Then connect from a second terminal:

```bash
arm-none-eabi-gdb firmware.elf -ex "target remote :1234"
```

## Evidence

All output below is from an actual run. Reproduce it with the commands shown.

### Build size

```
$ arm-none-eabi-size firmware.elf
   text    data     bss     dec     hex filename
    764       4       8     776     308 firmware.elf
```

`text` is code and read-only data in Flash. `data` is 4 bytes, which is the one
32-bit `initialized` variable. `bss` is 8 bytes, which is `uninitialized` plus
the SysTick counter.

### The ELF is built for the right machine

```
$ arm-none-eabi-readelf -h firmware.elf | grep -E "Class|Machine|Entry"
  Class:                             ELF32
  Machine:                           ARM
  Entry point address:               0x8000175
```

The entry address is odd because the low bit is the Thumb bit. The real address
is `0x8000174`.

### The vector table is laid out correctly

```
$ arm-none-eabi-objdump -s -j .isr_vector firmware.elf
 8000000 00000220 75010008 41000008 45000008
 8000010 49000008 4d000008 51000008 00000000
```

The words are little-endian. The first is `0x20020000`, which is `_estack`. The
second is `0x08000175`, which is `Reset_Handler` with the Thumb bit set. Both
match what the linker reported.

### Linker symbols

```
$ grep -E "_estack|_sdata|_edata|_sbss|_ebss|_sidata" firmware.map
  0x20020000   _estack
  0x080002fc   _sidata
  0x20000000   _sdata
  0x20000004   _edata
  0x20000004   _sbss
  0x2000000c   _ebss
```

`_sidata` is in Flash. `_sdata` is in RAM. `Reset_Handler` copies 4 bytes from
the first to the second, which matches the 4 bytes of `.data` reported by
`size`.

### It runs

```
$ make qemu
=== STM32F4 Bare-Metal Boot OK ===
PASS: initialized == 123 (.data copy worked)
PASS: uninitialized == 0 (.bss zeroing worked)
SysTick initialized. Interrupts firing at ~1kHz
SysTick: 100 ticks elapsed
SysTick: 100 ticks elapsed
SysTick: 100 ticks elapsed
```

Both assertions pass. The repeating tick message shows that the vector table is
wired up and that interrupts are being taken.

## Files

```
Makefile                    build, run, and debug targets
ld/linker.ld                memory map and section placement
startup/startup_stm32f4.s   vector table and Reset_Handler
src/main.c                  memory checks and the SysTick handler
src/semihosting.h           console output through the QEMU semihosting call
```

## Next

Part 2 builds an RTOS kernel on top of this. See
[`../rtos_kernel/`](../rtos_kernel/).
