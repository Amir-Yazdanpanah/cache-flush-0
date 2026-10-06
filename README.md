# cache_flush measurement driver

`cache_flush` is a small Linux misc-device driver for running a timed kernel
workload while exposing an external GPIO measurement gate.

The device interface is intentionally small:

- `write()` uploads one complete program blob (maximum 1 MiB).
- `ioctl(CACHE_FLUSH_IOC_RUN, &request)` runs the timed workload with the
  selected reset profile.
- `read()` returns the result of the most recent completed run.

`cache_flush_ioctl.h` contains the userspace ABI shared with the driver. The
uploaded blob is executed in supervisor mode at `kernel_buffer + entry_offset`.
It must use the kernel's RISC-V calling convention, return with `ret`, and be
self-contained unless it deliberately calls stable addresses known to it.

At module initialization the driver walks the buffer mappings through
PGD/P4D/PUD/PMD/PTE, adds read, write, and execute permission to every buffer
page, flushes that kernel TLB range on every online hart, and finally flushes
the RISC-V instruction cache. The 1 MiB buffer is page-aligned and page-sized,
so unrelated module data does not share the pages changed to RWX.

## Building

The normal Meson build includes the module when a configured RISC-V kernel
header/build directory is supplied:

```sh
meson setup build-linux \
  --cross-file linux/cross-riscv64-linux-gnu.txt \
  -Dtarget=linux \
  -Dkernel_build_dir=/usr/src/linux-headers-6.12.0-riscv64
meson compile -C build-linux
```

The module can also be built directly with Kbuild:

```sh
make -C /path/to/riscv/kernel/build \
  M="$PWD/kernelmodule" ARCH=riscv \
  CROSS_COMPILE=riscv64-linux-gnu- modules
```

The Debian packaging helper installs `cache_flush.ko` below the matching
`/lib/modules/<kernel-release>/updates/dutizer/` directory and runs `depmod`
from its post-install script.

## Module parameters

The GPIO is mandatory. A CPU can optionally be selected for the workload:

```text
trigger_gpio=<n>   GPIO used as the external measurement gate
run_cpu=<n>        CPU used for calibration and measurement
settle_cycles=<n>  Approximate loop iterations before measurement (default 256)
```

For example:

```sh
sudo insmod cache_flush.ko trigger_gpio=23 run_cpu=2 settle_cycles=256
```

`run_cpu=-1` disables explicit repinning and keeps the caller's existing CPU
affinity. If a CPU is specified, it must be online and already contained in the
caller's allowed CPU mask.

The driver requires the RISC-V Zicbom extension and a valid kernel-discovered
`riscv,cbom-block-size`. Opening `/dev/cache_flush` requests the GPIO
exclusively and configures it as an output initially LOW; closing the device
drives it LOW and releases it. This lets the Linux runtime hand the same line
between kernel and userspace execution dynamically. GPIO controllers that
require sleeping operations are rejected.

## Reset profiles

The run request carries the same runtime profile IDs as the DUTizer protocol:
none, all methods, each individual method, and all-except-one variants. The
kernel dispatches the selected fixed profile through a function pointer.

The Linux cache method aligns the uploaded program range to the
hardware-reported cache-block size and issues `cbo.flush` inline for every
block. The inline assembly enables Zicbom locally with `.option arch,+zicbom`,
so it does not depend on the kernel's internal alternatives macros. It does not
use the userspace reset-buffer-thrashing fallback. Other methods train the
branch predictor, thrash the BTB, drain the pipeline, or execute a local
`fence.i`.

## Measurement sequence

For every RUN ioctl the driver:

1. saves the caller's CPU affinity;
2. temporarily pins the caller to `run_cpu`, if configured;
3. disables task preemption;
4. calibrates the approximate duration of reset and workload;
5. performs one preparatory reset without executing the workload;
6. waits `settle_cycles` once, immediately before the measurement;
7. drives the trigger GPIO HIGH;
8. records the start timestamp and, for every iteration, performs exactly one
   reset and executes the workload;
9. records the final timestamp and drives the GPIO LOW;
10. re-enables task preemption; and
11. restores the caller's original CPU affinity.

Hardware interrupts remain enabled during the measurement.

Disabling preemption is appropriate for short controlled experiments, but a
very large `target_ns` will keep the selected CPU from scheduling another task
for the duration of the run. Do not use multi-second target times with this
configuration without reconsidering the measurement strategy.

## Userspace interface

A typical userspace sequence is:

```c
int fd = open("/dev/cache_flush", O_RDWR);

/* One write replaces the complete uploaded program. */
write(fd, program, program_size);

struct cache_flush_run request = {
    .target_ns = 100000, /* 100 us */
    .reset_profile = MICRORESET_PROFILE_ALL,
    .entry_offset = program_entry_offset,
};
ioctl(fd, CACHE_FLUSH_IOC_RUN, &request);

struct cache_flush_results result;
read(fd, &result, sizeof(result));
```

The result contains:

```c
struct cache_flush_results {
    __u64 start;
    __u64 end;
    __u64 actual_time;
    __u64 last_chunk;
    __u64 chunks;
    __u64 total_iterations;
};
```

## Reducing measurement noise

Pinning and `preempt_disable()` prevent the workload from being migrated or
scheduled out during the measured region. They do not eliminate all Linux
activity on that CPU. For a dedicated measurement CPU, the kernel can also be
booted with CPU isolation and full-dynticks options.

For example, to dedicate CPU 2:

```text
isolcpus=2 nohz_full=2 rcu_nocbs=2
```

These are kernel command-line arguments and therefore normally require changing
the bootloader configuration and rebooting. Keep at least one other CPU as a
normal housekeeping CPU.

- `isolcpus=2` keeps normal scheduler load balancing away from CPU 2.
- `nohz_full=2` suppresses the periodic scheduler tick on CPU 2 when the kernel
  can run it in full-dynticks mode.
- `rcu_nocbs=2` offloads RCU callback processing from CPU 2.

After reboot, verify the active kernel command line with:

```sh
cat /proc/cmdline
```

The module can then explicitly use the same isolated CPU:

```sh
sudo insmod cache_flush.ko trigger_gpio=23 run_cpu=2
```

### Interrupts

CPU isolation does not automatically guarantee that every hardware IRQ is kept
away from the measurement CPU. For low-noise measurements, also inspect IRQ
affinity and move unrelated device interrupts to housekeeping CPUs where
appropriate.

Do **not** simply disable hardware interrupts around an arbitrarily long
measurement window. That can cause severe system latency and device failures.

## Permissions

The misc device is created with mode `0600`. Running a non-preemptible timed
kernel workload is intentionally restricted to privileged users by default.
