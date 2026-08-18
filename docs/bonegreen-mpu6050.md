# MPU6050 on a BeagleBone Green

> ⚠️ **Written without the hardware in hand. Not validated on a board yet.**
> Everything here was derived from the mainline device tree and the AM335x
> header pin tables, and checked as far as a host can check it — the tree
> compiles, and differs from the upstream board tree by exactly one node. The
> [validation checklist](#validation-checklist) is
> what turns that into evidence, and it is not done.

`beaglebone-yocto` is the machine that ships inside `poky` (`meta-yocto-bsp`),
so this needs no BSP layer beyond the reference distribution. The board is also
the one the Yocto Project documentation uses throughout its examples, so the
machine, the device tree and the U-Boot target all exist upstream: the work here
is the sensor and the integration, not a bring-up.

## What the layer adds

```
recipes-kernel/linux/
├── linux-yocto_%.bbappend                       installs the board tree
└── linux-yocto/
    ├── am335x-bonegreen-mpu6050.dts             the board tree plus the sensor
    └── mpu6050.cfg                              IIO and MPU6050 drivers, built in
```

`imulog` needs no changes: it looks the device up by its IIO `name` attribute
rather than assuming a device number, so the same binary and the same recipe
serve any board that exposes an `mpu6050`.

## The bus, and why there is nothing to set up

The sensor goes on **I2C2 — P9_19 (SCL) and P9_20 (SDA)**. That bus is already
enabled, with its pinmux, in `am335x-bone-common.dtsi`:

```
&i2c2 {
	pinctrl-0 = <&i2c2_pins>;
	status = "okay";
	clock-frequency = <100000>;
	cape_eeprom0: cape_eeprom0@54 { ... }
```

It is the cape bus, carrying the cape EEPROMs at 0x54–0x57. So the board tree
adds one node and nothing else: no pinmux, no bus to enable, and no address
conflict with the sensor at 0x68.

⚠️ **The bus is declared at 100 kHz**, and that is a real ceiling. A sample is
a handful of two-byte register reads, so it costs a couple of milliseconds of
bus time — fine for a health log at 1 Hz, useless for vibration capture. Which
is the argument for the interrupt line, below, not for raising the clock.

## Wiring

| MPU6050 | BeagleBone header |
| ------- | ----------------- |
| VCC     | P9_3 (3.3 V)      |
| GND     | P9_1              |
| SDA     | P9_20             |
| SCL     | P9_19             |
| INT     | *not wired yet* — P9_12 |

**P9_12 is `gpio1_28`** (pad `GPMC_BEN1`), and no upstream node on this board
maps it. Wiring `INT` there and declaring the interrupt is what gets the IIO
trigger and the buffered path, with kernel timestamps instead of userspace
polling. The device tree carries the exact two properties needed, commented,
so it is a two-line change once the wire is in place.

Doing it is the difference between a sensor you poll and a sensor that drives
acquisition — and it is the reason this board is worth the header space: it has
GPIOs to spare, which is not true of every small target.

## Build configuration

```
MACHINE ?= "beaglebone-yocto"
KERNEL_DEVICETREE:append:beaglebone-yocto = " ti/omap/am335x-bonegreen-mpu6050.dtb"
IMAGE_INSTALL:append = " imulog"
```

⚠️ `KERNEL_DEVICETREE` belongs in `local.conf`, not in the kernel bbappend. The
image recipe evaluates it from a different datastore than the kernel recipe, so
setting it in the bbappend builds and deploys the `.dtb` and then never gets it
into the boot path — a failure that looks like a missing sensor rather than a
missing file. It also needs `:append`: a plain `+=` can land on the wrong side
of the parse order and replace the machine's whole device tree list.

## Validation checklist

Nothing below has been run. In order, because each one is cheap and the first
failure explains the rest:

**1. ⚠️ The open question: which device tree U-Boot actually loads.** On this
board U-Boot picks `fdtfile` by reading the board EEPROM, so it will ask for
`am335x-bonegreen.dtb` — not ours. It has to be resolved one of three ways: the
U-Boot environment (`printenv fdtfile` on the console, then `setenv`/`saveenv`),
an `extlinux.conf` entry if the image is built with `UBOOT_EXTLINUX`, or naming
the tree so that the detected name matches. **Determine
which mechanism this image uses before assuming the sensor is missing** — a
kernel that boots without the sensor most likely booted the upstream tree.

```sh
bitbake-getvar -r core-image-minimal UBOOT_EXTLINUX
ls /boot /boot/extlinux 2>/dev/null      # on the target
fw_printenv fdtfile 2>/dev/null          # or printenv at the U-Boot prompt
```

**2. The tree is the one that booted.**

```sh
cat /proc/device-tree/model      # expect "...BeagleBone Green with MPU6050"
```

**3. The driver bound.**

```sh
dmesg | grep -i inv-mpu6050      # expect "inv-mpu6050-i2c 2-0068"
i2cdetect -y -r 2                # 0x68 present, plus the cape EEPROMs
```

Note the bus number: `2-0068`, not `0-0068` — this board's cape bus is i2c2,
and the sensor is not on the bus the PMIC uses.

**4. The channels read, and the numbers are real.**

```sh
imulog -c 3 -r 10
imulog -R -c 1
```

With the board static, the magnitude of the three accelerometer axes should
land near 9.8 m/s², and `temp_c` near ambient. If the magnitude sits a few
percent low the suspect is per-axis zero-g offset rather than scaling —
`docs/imulog.md` explains why, and the six-position test that separates the two.

**5. The service.**

```sh
/etc/init.d/imulog status        # started at boot, pid points at imulog
head -n 3 /var/log/imulog.csv
/etc/init.d/imulog stop
```

## What was checked on the host

Not hardware evidence, but not nothing either:

- **The board tree compiles**, against the same kernel version the machine
  builds, with no warning attributable to it — the warnings dtc emits come from
  `am33xx.dtsi` and `am33xx-l4.dtsi` and appear identically for the upstream
  board tree.
- **It is the upstream tree plus one node.** Compiling both and diffing the
  decompiled result gives exactly two differences: the `model` string and the
  `imu@68` node inside `i2c2`.
- **The DTB Makefile takes an appended entry.** `ti/omap/Makefile` ends in a
  plain continuation list, with no trailing immediate assignment of the
  `always-y := $(dtb-y)` kind — which would silently ignore anything appended
  after it, and would force the entry to be inserted mid-file instead.
- **0x68 is free** on that bus in the upstream tree, and **P9_12 / `gpio1_28`
  is unmapped**, so neither choice collides with the board's own nodes.
