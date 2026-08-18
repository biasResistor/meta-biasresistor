SUMMARY = "MPU6050 sample logger over the IIO sysfs interface"
DESCRIPTION = "Polls the accelerometer, gyroscope and temperature channels of \
an InvenSense MPU6050 exposed as an IIO device and writes one CSV line per \
sample. The target board wires no interrupt line to the sensor, so the IIO \
buffer and trigger path is unavailable and the sysfs attributes are the only \
data path."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://imulog.c \
           file://Makefile \
           file://imulog.default \
           file://imulog.service \
           file://imulog.init \
           "

# Everything is carried in the layer, so the source directory is whatever the
# fetcher unpacked into. Referring to UNPACKDIR instead of a literal path keeps
# this working across the releases in LAYERSERIES_COMPAT, which moved it.
S = "${UNPACKDIR}"

inherit update-rc.d systemd

SYSTEMD_SERVICE:${PN} = "imulog.service"
SYSTEMD_AUTO_ENABLE = "enable"

INITSCRIPT_NAME = "imulog"
INITSCRIPT_PARAMS = "defaults 90 10"

do_compile() {
    oe_runmake
}

do_install() {
    oe_runmake install DESTDIR=${D} bindir=${bindir}

    install -d ${D}${sysconfdir}/default
    install -m 0644 ${S}/imulog.default ${D}${sysconfdir}/default/imulog

    # Both init classes no-op themselves when their DISTRO_FEATURE is absent,
    # so only the integration that will actually run gets installed.
    if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${S}/imulog.service ${D}${systemd_system_unitdir}/imulog.service
        sed -i -e 's,@BINDIR@,${bindir},g' \
               -e 's,@SYSCONFDIR@,${sysconfdir},g' \
               ${D}${systemd_system_unitdir}/imulog.service
    fi

    if ${@bb.utils.contains('DISTRO_FEATURES', 'sysvinit', 'true', 'false', d)}; then
        install -d ${D}${sysconfdir}/init.d
        install -m 0755 ${S}/imulog.init ${D}${sysconfdir}/init.d/imulog
        sed -i -e 's,@BINDIR@,${bindir},g' \
               -e 's,@SYSCONFDIR@,${sysconfdir},g' \
               ${D}${sysconfdir}/init.d/imulog
    fi
}

FILES:${PN} += "${systemd_system_unitdir}/imulog.service"

CONFFILES:${PN} = "${sysconfdir}/default/imulog"
