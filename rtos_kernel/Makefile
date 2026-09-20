# ================================================================
# Makefile: Low-latency Cortex-M4 RTOS kernel (STM32F407 / QEMU)
#
#   make        -> build firmware.elf and firmware.bin
#   make qemu   -> run in QEMU (semihosting output in terminal)
#   make debug  -> start QEMU as a GDB server on port 1234
#   make clean  -> remove build artifacts
#
# Toolchain: GNU ARM Embedded (arm-none-eabi-*)
# Target:    STM32F407 emulated by QEMU olimex-stm32-h405
# ================================================================

PREFIX   = arm-none-eabi-
CC       = $(PREFIX)gcc
AS       = $(PREFIX)gcc
LD       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
SIZE     = $(PREFIX)size

# GDB: use gdb-multiarch on Ubuntu/Debian, arm-none-eabi-gdb elsewhere
GDB      = gdb-multiarch

TARGET   = firmware

SRC_DIR     = src
STARTUP_DIR = startup
LD_DIR      = ld
BUILD_DIR   = build

# ---- Sources ---------------------------------------------------
C_SRCS   = $(SRC_DIR)/main.c           \
           $(SRC_DIR)/rtos.c           \
           $(SRC_DIR)/poll.c           \
           $(SRC_DIR)/sync.c           \
           $(SRC_DIR)/spsc.c           \
           $(SRC_DIR)/queue_baseline.c \
           $(SRC_DIR)/stats.c          \
           $(SRC_DIR)/gpio.c           \
           $(SRC_DIR)/app_demo.c       \
           $(SRC_DIR)/bench.c          \
           $(SRC_DIR)/libc_stubs.c
ASM_SRCS = $(STARTUP_DIR)/startup_stm32f4.s \
           $(SRC_DIR)/context.s

C_OBJS   = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
ASM_OBJS = $(BUILD_DIR)/startup_stm32f4.o $(BUILD_DIR)/context.o
ALL_OBJS = $(C_OBJS) $(ASM_OBJS)

LDSCRIPT = $(LD_DIR)/linker.ld

# ---- Configurable build defines --------------------------------
# CPU_MHZ    : core clock (8 for QEMU olimex model, 168 for hardware)
# EXTRA_DEFS : extra -D flags (e.g. -DRTOS_ENABLE_MPU, -DRTOS_BENCH)
CPU_MHZ  ?= 8
DEFS      = -DRTOS_CPU_MHZ=$(CPU_MHZ) $(EXTRA_DEFS)

# ---- Flags -----------------------------------------------------
CFLAGS = -mcpu=cortex-m4    \
         -mthumb            \
         -mfloat-abi=soft   \
         -Wall              \
         -Wextra            \
         -O2                \
         $(DEFS)            \
         -g                 \
         -ffreestanding     \
         -fno-tree-loop-distribute-patterns \
         -ffunction-sections \
         -fdata-sections

ASFLAGS = -mcpu=cortex-m4  \
          -mthumb          \
          -mfloat-abi=soft \
          -g

# libgcc provides compiler runtime helpers (e.g. 64-bit division,
# __aeabi_uldivmod) that GCC may emit. We stay -nostdlib (no libc)
# but link the compiler runtime explicitly.
LIBGCC = $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)

LDFLAGS = -T $(LDSCRIPT)          \
          -Wl,--gc-sections       \
          -Wl,-Map=$(TARGET).map  \
          -nostdlib               \
          -nostartfiles           \
          -mcpu=cortex-m4         \
          -mthumb                 \
          -mfloat-abi=soft

QEMU = qemu-system-arm                              \
       -machine olimex-stm32-h405                   \
       -nographic                                   \
       -semihosting-config enable=on,target=native  \
       -kernel $(TARGET).elf

.PHONY: all clean qemu debug hw flash \
        bench bench-ctxsw bench-jitter bench-ipc bench-all

all: $(BUILD_DIR) $(TARGET).elf $(TARGET).bin
	@echo "\n=== Build complete ==="
	$(SIZE) $(TARGET).elf

# ---- Hardware build (STM32F4 Discovery, 168 MHz, MPU guard on) --
hw:
	$(MAKE) clean
	$(MAKE) CPU_MHZ=168 EXTRA_DEFS="-DRTOS_ENABLE_MPU=1"
	@echo "Hardware image built (168 MHz, MPU stack guard enabled)."

# ================================================================
# Benchmarking testbed. Three experiments, three separate images.
#
# Each benchmark is built as its OWN firmware image rather than being
# selected at run time. A measurement of the scheduler must not share
# the CPU with the demo's tasks, or it stops being a measurement of
# the scheduler. Building them separately also keeps each image small
# enough that code layout cannot drift between runs.
#
# Append HW=1 to any of these to build the 168 MHz hardware image
# with the MPU stack guard on, e.g.:  make bench-jitter HW=1
# ================================================================
ifdef HW
BENCH_CPU_MHZ = 168
BENCH_HW_DEFS = -DRTOS_ENABLE_MPU=1
else
BENCH_CPU_MHZ = $(CPU_MHZ)
BENCH_HW_DEFS =
endif

# Experiment 1. Context-switch cost, >= 10^6 samples
bench-ctxsw:
	$(MAKE) clean
	$(MAKE) CPU_MHZ=$(BENCH_CPU_MHZ) EXTRA_DEFS="-DRTOS_BENCH_CTXSW=1 $(BENCH_HW_DEFS)"
	@echo "Experiment 1 (context-switch cost) built."

# Experiment 2. Execution jitter: tick-driven vs poll-mode
bench-jitter:
	$(MAKE) clean
	$(MAKE) CPU_MHZ=$(BENCH_CPU_MHZ) EXTRA_DEFS="-DRTOS_BENCH_JITTER=1 $(BENCH_HW_DEFS)"
	@echo "Experiment 2 (execution jitter) built. Probe PD12 (signal), PD13 (window)."

# Experiment 3, 64-byte IPC latency: mutex queue vs lock-free vs cache-aligned
bench-ipc:
	$(MAKE) clean
	$(MAKE) CPU_MHZ=$(BENCH_CPU_MHZ) EXTRA_DEFS="-DRTOS_BENCH_IPC=1 $(BENCH_HW_DEFS)"
	@echo "Experiment 3 (64-byte IPC latency) built."

# Backwards-compatible alias for the original context-switch target.
bench: bench-ctxsw

# Build and run all three under QEMU, one after another.
bench-all:
	$(MAKE) bench-ctxsw  && $(MAKE) qemu-run
	$(MAKE) bench-jitter && $(MAKE) qemu-run
	$(MAKE) bench-ipc    && $(MAKE) qemu-run

# ---- Flash to STM32F4 Discovery --------------------------------
# Uses st-flash (stlink). Alternative: OpenOCD (see README).
flash: $(TARGET).bin
	st-flash write $(TARGET).bin 0x08000000

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ---- Rebuild when the -D flags change --------------------------
# Each experiment is a different set of -D defines over the SAME
# sources. Without this, running `make` after `make bench-ctxsw`
# finds every .o newer than its .c, rebuilds nothing, and silently
# relinks the benchmark objects into what you believe is the demo
# image. The stamp file records the flags the objects were built
# with; when it changes, everything that depends on it rebuilds.
.PHONY: FORCE
$(BUILD_DIR)/.buildflags: FORCE | $(BUILD_DIR)
	@echo '$(DEFS)' | cmp -s - $@ || { \
	    echo "[FLAGS] $(DEFS)"; echo '$(DEFS)' > $@; }

# ---- Compile C -------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Assemble --------------------------------------------------
$(BUILD_DIR)/startup_stm32f4.o: $(STARTUP_DIR)/startup_stm32f4.s $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[AS] $<"
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/context.o: $(SRC_DIR)/context.s $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[AS] $<"
	$(AS) $(ASFLAGS) -c $< -o $@

# ---- Link ------------------------------------------------------
$(TARGET).elf: $(ALL_OBJS)
	@echo "[LD] $@"
	$(LD) $(LDFLAGS) $(ALL_OBJS) $(LIBGCC) -o $@

$(TARGET).bin: $(TARGET).elf
	@echo "[OC] $@"
	$(OBJCOPY) -O binary $< $@

clean:
	@echo "[CLEAN]"
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map

qemu: $(TARGET).elf
	@echo "[QEMU] Running (Ctrl+A then X to quit)..."
	$(QEMU)

# Time-boxed run used by 'bench-all': the benchmark images finish on
# their own and then idle forever, so an unattended run needs a stop.
QEMU_RUN_SECS ?= 60
qemu-run: $(TARGET).elf
	@echo "[QEMU] Timed run ($(QEMU_RUN_SECS)s)..."
	-timeout $(QEMU_RUN_SECS) $(QEMU)

debug: $(TARGET).elf
	@echo "[DEBUG] QEMU GDB server on :1234. Connect with '$(GDB) firmware.elf'"
	$(QEMU) -S -s
