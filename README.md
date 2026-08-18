# meta-biasresistor

A Yocto/OpenEmbedded layer for getting measurements off small ARM Linux boards:
bring a sensor up in the device tree, get calibrated samples out of it, and ship
the reader as a recipe and a service instead of a script run by hand.

Board support, kernel fragments and my own recipes live here rather than in
`poky` or the BSP layers, so upstream stays untouched and updatable. Each piece
has a note in `docs/` saying what it needed, what was not obvious, and how far
it has been verified.

## What is in it now

- **`imulog`** — reads an MPU6050 through the kernel's IIO interface and logs
  calibrated samples as CSV. Scale and offset are read from the device rather
  than hardcoded from a full-scale range. Shipped as a recipe with a defaults
  file, a systemd unit and a sysvinit script, each init integration guarded by
  its `DISTRO_FEATURE`.
- **BeagleBone Green** — a board device tree that is the mainline tree plus the
  sensor node on the cape I2C bus, and the kernel fragment that builds the IIO
  drivers in. Verified on the host: it compiles, and differs from the upstream
  board tree by exactly one node.

The sensor sits on I2C at 100 kHz and is read through sysfs, which makes it a
slow health log — a sample every second, not vibration capture. Worth stating so
the numbers are not mistaken for something they are not.

## Layout

```
conf/layer.conf         layer metadata: priority, dependencies, release compat
recipes-kernel/         board device trees and kernel config fragments
recipes-imu/            imulog
docs/                   notes per piece
```

## Using it

Clone next to `poky`, then add it to the build:

```sh
bitbake-layers add-layer ../meta-biasresistor
bitbake-layers show-layers        # confirm it is picked up
```

> **Note:** `LAYERSERIES_COMPAT_biasresistor` in `conf/layer.conf` must list the
> Yocto release you actually build against. If bitbake refuses the layer, that
> line is almost always why — add your release name to it.

## License

MIT — see `LICENSE`.
