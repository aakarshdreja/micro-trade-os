# ================================================================
# Makefile, Build system for STM32F4 bare-metal project
#
# Usage:
#   make          → Build firmware.elf and firmware.bin
#   make qemu     → Build and run in QEMU (semihosting output visible)
#   make debug    → Build and start QEMU in GDB server mode (port 1234)
#   make clean    → Remove all generated files
#
# Toolchain: GNU ARM Embedded (arm-none-eabi-*)
# Target:    STM32F405 emulated by QEMU olimex-stm32-h405
# ================================================================
 
# ---- Toolchain ------------------------------------------------
# Cross-compiler prefix for ARM bare-metal targets.
# All tools share this prefix: gcc, as, ld, objcopy, size, nm.
PREFIX   = arm-none-eabi-
CC       = $(PREFIX)gcc       # C compiler
AS       = $(PREFIX)gcc       # Use GCC as assembler (handles .s files with C preprocessor)
LD       = $(PREFIX)gcc       # Use GCC as linker front-end (simpler than calling ld directly)
OBJCOPY  = $(PREFIX)objcopy   # ELF → binary/hex converter
SIZE     = $(PREFIX)size      # Reports section sizes
 
# GDB: use gdb-multiarch on Ubuntu/Debian, arm-none-eabi-gdb on Windows/macOS
# Uncomment the line for your system:
GDB      = gdb-multiarch      # Ubuntu/Debian
#GDB     = arm-none-eabi-gdb  # Windows / macOS / Arch Linux
 
# ---- Target name -----------------------------------------------
# All generated files use this base name: firmware.elf, firmware.bin, firmware.map
TARGET   = firmware
 
# ---- Directory layout ------------------------------------------
SRC_DIR     = src
STARTUP_DIR = startup
LD_DIR      = ld
BUILD_DIR   = build
 
# ---- Source files ----------------------------------------------
C_SRCS      = $(SRC_DIR)/main.c
ASM_SRCS    = $(STARTUP_DIR)/startup_stm32f4.s
 
# ---- Object files (compiled output, placed in build/) ----------
C_OBJS      = $(BUILD_DIR)/main.o
ASM_OBJS    = $(BUILD_DIR)/startup_stm32f4.o
ALL_OBJS    = $(C_OBJS) $(ASM_OBJS)
 
# ---- Linker script ---------------------------------------------
LDSCRIPT    = $(LD_DIR)/linker.ld
 
# ---- Compiler flags --------------------------------------------
# -mcpu=cortex-m4   : Generate instructions for Cortex-M4 core
# -mthumb           : Use Thumb2 instruction set (required for Cortex-M)
# -mfloat-abi=soft  : Software floating-point (QEMU default; no FPU emulation needed)
# -Wall             : Enable all compiler warnings
# -Wextra           : Enable extra warnings
# -O0               : No optimization. Keeps code faithful to source for debugging
# -g                : Generate DWARF debug symbols (needed for GDB source-level debug)
# -ffunction-sections: Put each function in its own .text.funcname section
# -fdata-sections   : Put each variable in its own .data.varname section
#                     These two flags enable --gc-sections to remove dead code
CFLAGS = -mcpu=cortex-m4    \
          -mthumb            \
          -mfloat-abi=soft   \
          -Wall              \
          -Wextra            \
          -O0                \
          -g                 \
          -ffunction-sections \
          -fdata-sections
 
# ---- Assembler flags -------------------------------------------
# Same CPU flags as CFLAGS so the assembler targets the same architecture.
# -g: include debug info in .o so GDB can show assembly source lines.
ASFLAGS = -mcpu=cortex-m4  \
           -mthumb          \
           -mfloat-abi=soft \
           -g
 
# ---- Linker flags ----------------------------------------------
# -T $(LDSCRIPT)         : Use our custom linker script
# -Wl,--gc-sections      : Remove unused sections (reduces binary size)
#                          Works with -ffunction-sections -fdata-sections above
# -Wl,-Map=$(TARGET).map : Write a linker map file (required deliverable)
# -nostdlib              : Do not link standard C library (no printf, malloc etc.)
#                          We are bare-metal. No OS, no C runtime library
# -nostartfiles          : Do not use GCC's default crt0.o startup file
#                          We wrote our own startup in startup_stm32f4.s
# CPU flags repeated for linker because GCC passes them to the linker backend
LDFLAGS = -T $(LDSCRIPT)          \
           -Wl,--gc-sections       \
           -Wl,-Map=$(TARGET).map  \
           -nostdlib               \
           -nostartfiles           \
           -mcpu=cortex-m4         \
           -mthumb                 \
           -mfloat-abi=soft
 
# ---- QEMU command ----------------------------------------------
# -machine olimex-stm32-h405 : Emulate Olimex STM32-H405 board (STM32F405 chip)
# -nographic                 : Run without GUI window; use terminal for I/O
# -semihosting               : Intercept BKPT 0xAB and service semihosting calls
#                              This enables sh_puts() to print to your terminal
# -kernel firmware.elf       : Load our ELF file (QEMU parses ELF directly)
QEMU = qemu-system-arm                   \
       -machine olimex-stm32-h405        \
       -nographic                        \
       -semihosting                      \
       -kernel $(TARGET).elf
 
# ================================================================
# BUILD RULES
# ================================================================
 
# Default target: build everything, then print size report
.PHONY: all clean qemu debug
 
all: $(BUILD_DIR) $(TARGET).elf $(TARGET).bin
	@echo "\n=== Build complete ==="
	$(SIZE) $(TARGET).elf
 
# ---- Create build directory if it doesn't exist ----------------
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
 
# ---- Compile C sources -----------------------------------------
# $< = the source file (prerequisite)
# $@ = the target file (output)
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c $(SRC_DIR)/semihosting.h | $(BUILD_DIR)
	@echo "[CC] Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@
 
# ---- Assemble startup file -------------------------------------
$(BUILD_DIR)/startup_stm32f4.o: $(STARTUP_DIR)/startup_stm32f4.s | $(BUILD_DIR)
	@echo "[AS] Assembling $<"
	$(AS) $(ASFLAGS) -c $< -o $@
 
# ---- Link all objects into ELF ---------------------------------
$(TARGET).elf: $(ALL_OBJS)
	@echo "[LD] Linking $@"
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@
 
# ---- Convert ELF to raw binary ---------------------------------
# -O binary: output format is raw binary (no ELF headers)
# Raw binary is used when flashing to real hardware via ST-Link/DFU
$(TARGET).bin: $(TARGET).elf
	@echo "[OC] Generating $@"
	$(OBJCOPY) -O binary $< $@
 
# ---- Clean all generated files ---------------------------------
clean:
	@echo "[CLEAN] Removing generated files"
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map
 
# ---- Run firmware in QEMU (normal mode) ------------------------
# Output from sh_puts() appears directly in your terminal.
# Press Ctrl+A then X to exit QEMU.
qemu: $(TARGET).elf
	@echo "[QEMU] Running firmware (Ctrl+A then X to quit)..."
	$(QEMU)
 
# ---- Start QEMU in GDB server mode ----------------------------
# -S: Freeze CPU at start (don't execute until GDB says 'continue')
# -s: Start GDB server on port 1234 (equivalent to -gdb tcp::1234)
# Open a second terminal and run: gdb-multiarch firmware.elf
# Then in GDB: target remote :1234
debug: $(TARGET).elf
	@echo "[DEBUG] Starting QEMU GDB server on port 1234..."
	@echo "[DEBUG] In another terminal run: gdb-multiarch firmware.elf"
	@echo "[DEBUG] Then in GDB: target remote :1234"
	$(QEMU) -S -s
