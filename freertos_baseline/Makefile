# ================================================================
# Makefile, FreeRTOS BASELINE
#
# Builds the same three experiments as rtos_kernel, on FreeRTOS,
# with the same compiler, the same flags, the same boot layer and the
# same measurement code. The kernel is the only variable.
#
#   make bench-jitter   Experiment 2 on FreeRTOS
#   make bench-ipc      Experiment 3 on FreeRTOS
#   make bench-ctxsw    Experiment 1 on FreeRTOS
# ================================================================

PREFIX   = arm-none-eabi-
CC       = $(PREFIX)gcc
LD       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
SIZE     = $(PREFIX)size

TARGET      = firmware
SRC_DIR     = src
STARTUP_DIR = startup
LD_DIR      = ld
BUILD_DIR   = build
FR          = external/FreeRTOS-Kernel

CPU_MHZ    ?= 8
EXTRA_DEFS ?=
DEFS        = -DRTOS_CPU_MHZ=$(CPU_MHZ) $(EXTRA_DEFS)

INCLUDES = -I$(SRC_DIR) -I$(FR)/include -I$(FR)/portable/GCC/ARM_CM3

# Identical to rtos_kernel's flags. -Wno-unused is NOT used: the
# baseline is held to the same standard as our own kernel.
CFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
         -Wall -Wextra -O2 $(DEFS) $(INCLUDES) -g \
         -ffreestanding -fno-tree-loop-distribute-patterns \
         -ffunction-sections -fdata-sections

ASFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -g

LIBGCC  = $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)
LDFLAGS = -T $(LD_DIR)/linker.ld -Wl,--gc-sections -Wl,-Map=$(TARGET).map \
          -nostdlib -nostartfiles -mcpu=cortex-m4 -mthumb -mfloat-abi=soft

APP_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/stats.c $(SRC_DIR)/gpio.c $(SRC_DIR)/libc_stubs.c
FR_SRCS  = $(FR)/tasks.c $(FR)/queue.c $(FR)/list.c \
           $(FR)/portable/GCC/ARM_CM3/port.c $(FR)/portable/MemMang/heap_4.c

OBJS = $(addprefix $(BUILD_DIR)/app_,$(notdir $(APP_SRCS:.c=.o))) \
       $(addprefix $(BUILD_DIR)/fr_,$(notdir $(FR_SRCS:.c=.o)))  \
       $(BUILD_DIR)/startup_stm32f4.o

QEMU = qemu-system-arm -machine olimex-stm32-h405 -nographic \
       -semihosting-config enable=on,target=native -kernel $(TARGET).elf

.PHONY: all clean qemu bench-jitter bench-ipc bench-ctxsw FORCE

all: $(BUILD_DIR) $(TARGET).elf $(TARGET).bin
	@echo "\n=== FreeRTOS baseline build complete ==="
	$(SIZE) $(TARGET).elf

bench-jitter:
	$(MAKE) clean && $(MAKE) EXTRA_DEFS="-DFRTOS_BENCH_JITTER=1"
bench-ipc:
	$(MAKE) clean && $(MAKE) EXTRA_DEFS="-DFRTOS_BENCH_IPC=1"
bench-ctxsw:
	$(MAKE) clean && $(MAKE) EXTRA_DEFS="-DFRTOS_BENCH_CTXSW=1"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Rebuild when the -D flags change (see rtos_kernel for why).
$(BUILD_DIR)/.buildflags: FORCE | $(BUILD_DIR)
	@echo '$(DEFS)' | cmp -s - $@ || { echo "[FLAGS] $(DEFS)"; echo '$(DEFS)' > $@; }

$(BUILD_DIR)/app_%.o: $(SRC_DIR)/%.c $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fr_%.o: $(FR)/%.c $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/fr_port.o: $(FR)/portable/GCC/ARM_CM3/port.c $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/fr_heap_4.o: $(FR)/portable/MemMang/heap_4.c $(BUILD_DIR)/.buildflags | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/startup_stm32f4.o: $(STARTUP_DIR)/startup_stm32f4.s | $(BUILD_DIR)
	@echo "[AS] $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	@echo "[LD] $@"
	@$(LD) $(LDFLAGS) $(OBJS) $(LIBGCC) -o $@

$(TARGET).bin: $(TARGET).elf
	@$(OBJCOPY) -O binary $< $@

clean:
	@rm -rf $(BUILD_DIR) $(TARGET).elf $(TARGET).bin $(TARGET).map

qemu: $(TARGET).elf
	$(QEMU)
