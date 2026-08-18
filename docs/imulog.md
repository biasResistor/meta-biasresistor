# imulog: reading an MPU6050 over IIO

First application recipe in this layer. Everything before it was platform work —
device trees, kernel fragments, machine support — with no program of the
project's own on the target. `imulog` closes that gap: a small C program that
reads an MPU6050 through IIO, packaged as a recipe and started as a service.

It is deliberately board-agnostic: the device is found by its IIO `name`
attribute, so the same binary and the same recipe serve every machine in this
layer without a line of conditional code.

```
recipes-imu/imulog/
├── imulog_1.0.bb                recipe: build, install, init integration
└── imulog/
    ├── imulog.c                 the program
    ├── Makefile
    ├── imulog.default           /etc/default/imulog, arguments for the service
    ├── imulog.service           systemd unit
    └── imulog.init              sysvinit script
```

## Why polling and not the IIO buffer

Where the sensor's `INT` line is not wired, the device tree node declares only
`compatible` and `reg` — no `interrupts` property. That single fact decides the
shape of the program. From
`drivers/iio/imu/inv_mpu6050/inv_mpu_core.c`:

```c
	if (irq > 0) {
		/*
		 * The driver currently only supports buffered capture with its
		 * own trigger. So no IRQ, no trigger, no buffer
		 */
		result = devm_iio_triggered_buffer_setup(...);
```

With no IRQ the driver still registers the IIO device and every channel is
readable, but no trigger is created and `/dev/iio:device0` yields nothing. The
data path that remains is the per-channel sysfs attributes, read one at a time,
paced by userspace. That is fine for a slow health log and useless for vibration
capture at kilohertz rates — which is the argument for wiring `INT` to a GPIO
before the analysis stage, not a limitation of the driver. On the BeagleBone
Green that pin is available and the device tree already carries the two
properties it needs, commented; see `docs/bonegreen-mpu6050.md`.

## Units

The IIO ABI is `processed = (raw + offset) * scale`, and the program reads
`scale` and `offset` once at startup rather than hardcoding a full-scale range
that `in_accel_scale_available` lets the user change at runtime.

| Channel | Scale attribute | Result unit |
| --- | --- | --- |
| `in_accel_{x,y,z}_raw` | `in_accel_scale` | m/s² |
| `in_anglvel_{x,y,z}_raw` | `in_anglvel_scale` | rad/s |
| `in_temp_raw` | `in_temp_scale`, `in_temp_offset` | **millidegrees** C |

Temperature is the odd one: accelerometer and gyroscope scales give final units,
temperature gives millidegrees, so the program divides that channel by 1000 and
nothing else. For this part the kernel carries `TEMP_SCALE 2941176` (µ-units,
so 2.941176) and `TEMP_OFFSET 12420`, which reproduces the datasheet's
`degC = raw / 340 + 36.53`.

## Using it

```
IMAGE_INSTALL:append = " imulog"
```

On the target:

```sh
imulog -c 5 -r 10          # five samples at 10 Hz
imulog -R -c 1             # raw counts, no scaling
imulog -r 50 > capture.csv # until SIGINT
```

Output is CSV with a `#` comment header, so it loads straight into the offline
analyzer with `comment='#'`. The shape of it:

```
# t_s,ax_ms2,ay_ms2,az_ms2,gx_rads,gy_rads,gz_rads,temp_c
0.000,0.8217,-0.0574,10.0344,-0.00413,0.00160,-0.00067,24.61
```

`t_s` is monotonic seconds since the first sample, so it survives a wall-clock
step — a board with no RTC gets its time late, if at all.

The device is found by matching the IIO `name` attribute against `mpu6050`
rather than assuming `iio:device0`: numbering follows probe order, and this
board will get a second IIO device as soon as anything else lands on the bus.

## The service

Arguments live in `/etc/default/imulog` and are read by both init paths:

```sh
IMULOG_ARGS="-r 1"
IMULOG_LOG="/var/log/imulog.csv"
IMULOG_ERR="/var/log/imulog.err"
```

Under **systemd**, `imulog` runs in the foreground and its stdout is captured by
the journal (`journalctl -u imulog`). Under **sysvinit**, which is what a stock
poky image uses, the init script appends samples to `$IMULOG_LOG` and sends
stderr to `$IMULOG_ERR` — folding the two together would drop the startup line
and any read error into the middle of the samples.

⚠️ `/var/log` is a tmpfs on a stock poky image, and this board has 64 MiB of
RAM. At the default 1 Hz a line costs about 60 bytes, i.e. ~5 MB/day. Raising
the rate for a long capture means pointing `IMULOG_LOG` at real storage first.

## Four things that were not obvious

**`S = "${WORKDIR}"` no longer works for local files.** Recipes whose `SRC_URI`
is only `file://` used to build in `WORKDIR`. Since Yocto 5.0 the fetcher
unpacks into `UNPACKDIR`, whose default moved during the releases this layer
claims compatibility with. Referring to `${UNPACKDIR}` by name, rather than
picking `${WORKDIR}` or `${WORKDIR}/sources-unpack`, is what keeps the recipe
building on all of them.

**`LDFLAGS` has to be on the link line.** A one-line Makefile that calls
`$(CC) $(CFLAGS) -o` builds fine and then fails QA: OE injects its hardening
and `-Wl,--hash-style=gnu` flags through `LDFLAGS`, and a binary linked without
them trips the `ldflags` / GNU_HASH checks. The compile is not the problem, the
missing variable in the link is.

**`start-stop-daemon` cannot redirect.** It has no option to send a daemon's
stdout to a file, so the init script starts `/bin/sh -c "exec imulog … >> log"`.
The `exec` matters: without it the pid written by `--make-pidfile` belongs to
the wrapper shell, and `stop` kills the shell while `imulog` keeps running.

**Installing both init integrations unconditionally is harmless but wrong.**
`update-rc.d.bbclass` and `systemd.bbclass` each disable themselves when their
`DISTRO_FEATURE` is missing, so the unused file would just be dead weight in the
rootfs. The `bb.utils.contains` guards in `do_install` keep the package to the
integration that the image will actually run.

## Validation

> ⚠️ **Not yet run on hardware.** The program and its packaging are complete;
> what is missing is a target with the sensor on it. The checks below are the
> ones that close that gap, and they are the same on any board that exposes an
> `mpu6050` as an IIO device.

The checks, in order:

```sh
imulog -c 3 -r 10                 # scaled samples
imulog -R -c 1                    # raw counts, to compare against the scaling
/etc/init.d/imulog status         # started at boot, pid points at imulog
cat /var/log/imulog.err           # startup banner only: the CSV stays clean
head -n 3 /var/log/imulog.csv     # note: busybox head needs -n
/etc/init.d/imulog stop           # and no process left behind
```

Two things make the numbers checkable without any reference instrument:

**Temperature against the room.** The conversion is fixed for this part —
`degC = raw / 340 + 36.53` — so a raw count and a thermometer are enough to
prove the scaling is applied the right way round.

**One g of gravity.** With the board static, the vector magnitude of the three
accelerometer axes should land near 9.8 m/s². If it sits a few percent low, the
suspect is per-axis zero-g offset rather than scaling: this part is specified at
up to ±80 mg per axis, and on a tilt that loads all three axes those offsets
stop cancelling and project onto the magnitude. The six-position test — each
axis pointing down and up in turn — separates offset from gain error, and the
driver exposes `in_accel_calibbias` for the correction once it is known.

For the vibration work an offset is a footnote: order analysis lives in the AC
content, and a constant per-axis offset is what the first high-pass removes. It
matters only if the same sensor is ever asked for a tilt angle.
