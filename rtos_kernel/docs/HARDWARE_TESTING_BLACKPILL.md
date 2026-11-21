# Hardware testing on the WeAct Black Pill (STM32F411CEU6)

Step-by-step procedure for running the kernel and the FreeRTOS baseline on a
WeAct Black Pill with an ST-Link V2 programmer. Follow the steps in order: each
one checks something the next one depends on.

**Board facts used by the code:** STM32F411CEU6, Cortex-M4F, 25 MHz crystal,
100 MHz core clock, 512 KB Flash, 128 KB SRAM, MPU and DWT cycle counter
present. The jitter probe pins are **PB12** (signal) and **PB13** (window),
because the F411 package has no port D.

---

## 0. What you need

| Item | Needed for |
|---|---|
| WeAct Black Pill STM32F411CEU6 | everything |
| ST-Link V2 programmer (USB stick type) | flashing, debugging, printing output |
| 4 female-to-female jumper wires | ST-Link to Black Pill |
| Laptop with macOS or Linux (Windows: see 1c) | building and running |

Leave the Black Pill's USB-C port unplugged: the board is powered from the
ST-Link.

---

## 1. Install the tools (once per laptop)

### 1a. macOS

```bash
brew install --cask gcc-arm-embedded
brew install open-ocd stlink qemu
```

`arm-none-eabi-gdb` comes with the Arm toolchain. The Makefile uses
`gdb-multiarch` if it exists, otherwise `arm-none-eabi-gdb`.

### 1b. Ubuntu / Debian

```bash
sudo apt update
sudo apt install gcc-arm-none-eabi gdb-multiarch openocd stlink-tools qemu-system-arm make git
```

If OpenOCD later says `LIBUSB_ERROR_ACCESS`, unplug and replug the ST-Link,
or run the command with `sudo` once to confirm it is a permissions problem.

### 1c. Windows (WSL)

WSL cannot see USB devices by default. Install
[usbipd-win](https://github.com/dorssel/usbipd-win), then in an
**administrator** PowerShell, with the ST-Link plugged in:

```powershell
usbipd list
usbipd bind --busid <BUSID of "STM32 STLink">
usbipd attach --wsl --busid <BUSID>
```

Repeat `usbipd attach` each time you replug the ST-Link. Then follow 1b inside
WSL.

### 1d. Check the install

```bash
arm-none-eabi-gcc --version
openocd --version
st-info --version
```

All three must print a version.

---

## 2. Get the code

```bash
git clone https://github.com/aakarshdreja/micro-trade-os
cd micro-trade-os/rtos_kernel
```

(If you already have it: `git pull`.) The Black Pill support is selected with
`HW=1 BOARD=blackpill` on every hardware build. If `make hw BOARD=blackpill`
says `Unknown BOARD` or builds at 168 MHz, your copy predates the Black Pill
support: pull again.

Confirm QEMU still works before touching hardware:

```bash
make clean && make && make qemu      # Ctrl+A then X to quit
```

The boot banner should say `Core clock: not configured (QEMU model clock)`.

---

## 3. Wire the ST-Link to the Black Pill

**Unplug the ST-Link from USB while wiring.**

The Black Pill has a 4-pin debug header at the end opposite the USB-C port,
labelled `3V3 / DIO / SCK / GND` (some batches print `SWDIO`, `SWCLK`).
The ST-Link V2 has a 10-pin header with the pin names printed on its case.

| ST-Link V2 pin (printed on case) | Black Pill debug header |
|---|---|
| **3.3V** | **3V3** |
| **SWDIO** | **DIO** (PA13) |
| **SWCLK** | **SCK** (PA14) |
| **GND** | **GND** |

Rules:
- Read the labels on **your** ST-Link case. Clone pinouts differ between batches.
- **Never connect the ST-Link's 5V pin** to anything.
- Leave the Black Pill's USB-C unplugged while the 3.3V wire is connected.

Plug the ST-Link into the laptop. The Black Pill's red power LED should light.

---

## 4. Check that the laptop can see the chip

```bash
st-info --probe
```

Expected (numbers may differ slightly):

```
Found 1 stlink programmers
  ...
  flash:   524288 (pagesize: 16384)
  sram:    131072
  chipid:  0x431
  dev-type: STM32F411xC_xE
```

`chipid 0x431` and `flash 524288` (512 KB) confirm a real STM32F411.

Then check OpenOCD:

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg
```

Expected lines include `Cortex-M4 r0p1 processor detected` and
`target has 6 breakpoints, 4 watchpoints`, then
`Listening on port 3333 for gdb connections`. Press **Ctrl+C** to stop it.

If either fails, go to **Troubleshooting** at the end before continuing.

---

## 5. Test A: the pipeline demo (kernel boots, clock and DWT work)

```bash
cd rtos_kernel
make hw BOARD=blackpill
make run-hw 2>&1 | tee ../results/results_blackpill_demo.txt
```

`make run-hw` flashes the board with OpenOCD, turns semihosting on and starts
the program. **The firmware's output appears in this terminal.** Stop with
**Ctrl+C**.

Check these lines in the output:

```
Board: WeAct Black Pill STM32F411CEU6
Core clock: 100 MHz (PLL from 25 MHz HSE crystal)
DWT cycle counter: available (hardware, cycle-accurate)
```

| If you see | Meaning |
|---|---|
| `PLL from 25 MHz HSE crystal` | Correct. |
| `PLL from 16 MHz HSI: crystal did not start` | Still 100 MHz, but from the internal oscillator. Fine for testing; note it in the results. |
| `DWT ... unavailable` | Wrong build. Re-run `make hw BOARD=blackpill`. |
| No output at all | See Troubleshooting. |

The rest of the output (schedulability analysis, latency distributions, the
priority-inversion test) should look like the QEMU run, but with real cycle
counts.

Printing is slow on hardware: every line stops the CPU while the laptop
prints it. That is normal, and the benchmarks keep printing outside the timed
sections.

### Confirm the clock registers (optional, 2 minutes)

In terminal 1:

```bash
make ocd
```

In terminal 2:

```bash
make gdb
```

At the `(gdb)` prompt:

```
continue
```

Wait 2 seconds, press **Ctrl+C**, then:

```
x/wx 0x40023804
x/wx 0x40023808
```

Expected: `0x04403219` (PLL: /25, ×200, /2, from HSE) and `0x0000100a`
(running from the PLL, APB1 /2). Type `quit` and press Ctrl+C in terminal 1.
If HSE failed, the first value is `0x04003210` instead.

---

## 6. Test B: Experiment 1, context-switch cost

```bash
make bench-ctxsw HW=1 BOARD=blackpill
make run-hw 2>&1 | tee ../results/results_blackpill_ctxsw.txt
```

Wait for `switches collected = ... / 1000000` to finish and the report to print.
Check:

```
clock source: DWT CYCCNT (cycle-accurate, hardware)
ctx-switch: n=... min=... mean=... p50=... p99=... p99.9=... max=... cyc
```

`min` must now be **non-zero** (QEMU showed `min=0`). Also record
`max IRQ-disabled window`, which QEMU could not measure.

---

## 7. Test C: Experiment 2, execution jitter

```bash
make bench-jitter HW=1 BOARD=blackpill
make run-hw 2>&1 | tee ../results/results_blackpill_jitter.txt
```

The evidence is the on-chip histogram of block times from the DWT counter.
Phase A (tick-driven) should show a few blocks of about 1 ms; Phase B (poll
mode) should put every block within a cycle or two of the others.

---

## 8. Test D: Experiment 3, 64-byte IPC latency

```bash
make bench-ipc HW=1 BOARD=blackpill
make run-hw 2>&1 | tee ../results/results_blackpill_ipc.txt
```

Record the three subjects (mutex queue, lock-free SPSC, cache-aligned SPSC).

---

## 9. Test E: the FreeRTOS baseline, same three experiments

```bash
cd ../freertos_baseline
make bench-ctxsw  HW=1 BOARD=blackpill && make run-hw 2>&1 | tee ../results/results_blackpill_frtos_ctxsw.txt
make bench-jitter HW=1 BOARD=blackpill && make run-hw 2>&1 | tee ../results/results_blackpill_frtos_jitter.txt
make bench-ipc    HW=1 BOARD=blackpill && make run-hw 2>&1 | tee ../results/results_blackpill_frtos_ipc.txt
```

Press Ctrl+C after each report prints before starting the next one.

Note for the report: on hardware our kernel times with the DWT counter, while
the baseline still uses the SysTick-derived counter. Both count CPU cycles at
100 MHz, but say which clock produced each number.

---

## 10. Running without the debugger

```bash
make flash
```

This writes `firmware.bin` with st-flash and resets the board. The program
runs on its own, but **prints nothing**: output needs OpenOCD attached. Use
it to check that the board runs standalone; for measurements use `make run-hw`.

---

## 11. What to record for the report

For every run, keep the `tee` text file and note:
- board, core clock line, and clock source line from the output
- `n`, `min`, `p50`, `p99`, `p99.9`, `max` in cycles, and the µs values

Cycle counts depend on the Cortex-M4 core and compare directly with the
Discovery board's. **µs values are at 100 MHz**, so they are larger than the
168 MHz figures in the proposal. Divide cycles by 100 for µs on this board.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `st-info --probe` finds 0 programmers | USB cable or port. On WSL, re-run `usbipd attach`. On Linux, try once with `sudo`. |
| `Found 1 stlink programmers` but `chipid 0x000` / `unknown device` | Wiring. Check SWDIO/SWCLK are not swapped and GND is connected. |
| OpenOCD: `init mode failed (unable to connect to the target)` | Wiring first. If wiring is right: hold the Black Pill's **NRST** button, start the OpenOCD command, release NRST about half a second later. |
| OpenOCD: `UNEXPECTED idcode` / `Could not find MEM-AP` | Clone chip. Add `-c "set CPUTAPID 0"` before `-f target/stm32f4x.cfg`. |
| OpenOCD warns the ST-Link firmware is old | Works anyway. Updating via STMicroelectronics' ST-LINK upgrade tool is optional. |
| `make run-hw` flashes, then no output | Run `make ocd` and `make gdb`, then `continue` in GDB; output appears in the OpenOCD terminal. If it still prints nothing, press Ctrl+C in GDB and run `bt` to see where it stopped. |
| Output stops after `Core clock:` never appears | The PLL did not lock. Run `x/wx 0x40023800` in GDB and send us the value. |
| `HardFault_Handler` in GDB `bt` | Note the last printed line and send the full output. |
| Board was flashed with other firmware and won't connect | Hold **BOOT0**, press and release **NRST**, release BOOT0 (enters the ROM bootloader), then connect again. |
