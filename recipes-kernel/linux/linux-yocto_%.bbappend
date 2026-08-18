FILESEXTRAPATHS:prepend := "${THISDIR}/linux-yocto:"

# This recipe inherits kernel-yocto, so the .cfg fragment is merged for us.
SRC_URI:append:beaglebone-yocto = " file://am335x-bonegreen-mpu6050.dts file://mpu6050.cfg"

# The board tree is carried here instead of patching the kernel tree: drop the
# source in with the other TI OMAP boards and list it so Kbuild builds it
# alongside them.
do_configure:append:beaglebone-yocto() {
    dts_dir="${S}/arch/${ARCH}/boot/dts/ti/omap"

    for d in ${UNPACKDIR} ${WORKDIR}/sources-unpack ${WORKDIR}; do
        if [ -f "$d/am335x-bonegreen-mpu6050.dts" ]; then
            install -m 0644 "$d/am335x-bonegreen-mpu6050.dts" "$dts_dir/"
            break
        fi
    done

    if [ ! -f "$dts_dir/am335x-bonegreen-mpu6050.dts" ]; then
        bbfatal "am335x-bonegreen-mpu6050.dts not found in WORKDIR"
    fi

    # This Makefile ends in a plain continuation list with no trailing
    # immediate assignment -- an `always-y := $(dtb-y)` at the end would
    # silently ignore anything appended after it -- so appending is enough
    # and the entry does not have to be inserted mid-file. Checked against 6.12.
    grep -q "am335x-bonegreen-mpu6050.dtb" "$dts_dir/Makefile" || \
        echo 'dtb-y += am335x-bonegreen-mpu6050.dtb' >> "$dts_dir/Makefile"
}
