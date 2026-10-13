# Zephyr Training Environment

Welcome to the Zephyr RTOS training! This repository includes a ready-to-use
development environment based on Zephyr **4.4.0**, which you can set up in one of
three ways:

---

## Manual Zephyr Setup

Follow the following guide:
- [Getting Started Guide](https://docs.zephyrproject.org/4.4.0/develop/getting_started/index.html#).

Make sure to select appropriate OS and to perform all steps till
[Build the Blinky Sample](https://docs.zephyrproject.org/4.4.0/develop/getting_started/index.html#build-the-blinky-sample).

## Response-Time Experiment (ESP32-S3)

The requirement is: each timer release must start control work within 10,000 us.
The measurement runs from the cycle timestamp in the timer callback to the first
control code after receiving the message. It includes queueing and dispatch, but
is not a completion-time deadline or a bound on interrupt latency before the
timer callback. A response greater than 10,000 us is a miss; a dropped release
also counts as a miss because it never starts. Sequence numbers restart at 1
for each run. Cycle subtraction tolerates a single wrap; delays must remain
shorter than a full 32-bit cycle-counter period.

Each run releases 20 events, 250 ms apart, starting after 500 ms. Counters are
updated for every event even when repeated warning messages are rate-limited.
`measure stats` reports released, processed, dropped, misses, maximum observed
response, and remaining samples. Wait for `remaining=0` before recording results
or starting another run. A completed, loss-free run has released=processed=20
and dropped=0. Maximum response covers received events, not dropped ones.

### Build and Inspect

From the workspace root in PowerShell:

```powershell
.\zephyrproject\.venv\Scripts\west.exe build -b esp32s3_devkitc/esp32s3/procpu -S espressif-flash-16M Zephyr_intermediate/app -d build -p always
.\zephyrproject\.venv\Scripts\west.exe flash -d build
```

Open the board's serial shell at the configured console baud rate. Run:

```text
kernel thread list
kernel thread stacks
measure baseline
measure stats
```

Repeat `measure stats` until remaining is zero. Save the sequence logs and final
counters. The kernel thread list includes names, priorities, states, and stack
usage. Outside a release, control normally waits on its message queue and
maintenance waits on its semaphore. A shell snapshot need not catch the brief
runnable state; use CTF scheduling events for that evidence. Run shell inspection
outside the timed window to avoid changing the measurement with inspection work.

On this single-core build (`CONFIG_SMP=n`), smaller nonnegative numbers are higher
preemptible priorities. Baseline control is priority 7 and maintenance is priority
4. Both are awakened in the same timer interrupt, so the 45,000 us maintenance
busy wait is expected to delay control and violate its 10,000 us deadline.

### Capture RAM CTF

CTF uses RAM, not the console UART. Extraction requires a working ESP32-S3 JTAG
debugger connection, the matching ELF with debug symbols, and its GDB/OpenOCD
setup. Babeltrace 2 can run in Linux or WSL; it is not bundled with this project.
On Debian/Ubuntu, install it with `sudo apt install babeltrace2` in your terminal.

Create the trace directories and copy this checkout's matching metadata:

```powershell
New-Item -ItemType Directory -Force traces/baseline, traces/corrected
Copy-Item deps/zephyr/subsys/tracing/ctf/tsdl/metadata traces/baseline/metadata
Copy-Item deps/zephyr/subsys/tracing/ctf/tsdl/metadata traces/corrected/metadata
.\zephyrproject\.venv\Scripts\west.exe debug -d build
```

Before each run, halt at an idle point with `remaining=0`. Clear the RAM backend
position and full flag in GDB, then continue and issue the corresponding serial
shell measurement command. Do not halt, break, or step during a timed window:

```gdb
set variable 'tracing_backend_ram.c'::pos = 0
set variable 'tracing_backend_ram.c'::buffer_full = 0
continue
```

After `measure stats` reports remaining=0, interrupt GDB and capture the baseline:

```gdb
print 'tracing_backend_ram.c'::buffer_full
print 'tracing_backend_ram.c'::pos
dump binary memory traces/baseline/channel0_0 (char *)ram_tracing (char *)ram_tracing + 'tracing_backend_ram.c'::pos
```

These relative paths assume GDB runs from the workspace root. Dump only `pos`
bytes, not the zero-padded full array. A true `buffer_full` means capture was
truncated: shorten the window or increase the buffer within the board's available
RAM and rebuild. Idle time and unrelated kernel events also consume buffer space.

### Correct and Repeat

With the target halted between runs, clear the backend again using the two GDB
assignments above, then continue. In the serial shell run:

```text
measure corrected
measure stats
```

This applies one correction: control priority becomes 3, above maintenance at 4.
The workload, event period, deadline, sample count, and instrumentation do not
change. Wait for remaining=0 and save the final counters. Inspect
`kernel thread list` afterwards to verify the new priority. Interrupt GDB and
repeat the dump using `traces/corrected/channel0_0`; check the full flag again.

Decode both captures from the workspace root in WSL:

```sh
babeltrace2 traces/baseline > traces/baseline/decoded.txt
babeltrace2 traces/corrected > traces/corrected/decoded.txt
grep -E 'named_event|thread_switched' traces/baseline/decoded.txt
grep -E 'named_event|thread_switched' traces/corrected/decoded.txt
```

Named events carry `name`, `arg0`, and `arg1`. `measure_begin` identifies the
priority and sample count; `control_release` carries sequence and deadline;
`control_start` carries sequence and measured response in us; `control_done`
carries sequence and cumulative misses; `control_drop` identifies a rejected
sequence and the queue error. Correlate release/start/done by sequence within
each capture and confirm all 20 sequences are present. Scheduling events should
show maintenance ahead of control in baseline and control ahead of maintenance
after the correction.

Record both runs' maximum response, miss count, drop count, and trace completeness.
The corrected run is expected to have fewer misses, but actual target measurements
must establish the result. Zero misses in 20 samples is an observed result, not
a proof of a hard real-time guarantee under every possible interrupt and workload.
