# CC, CFLAGS and LDFLAGS come from the environment when this is built by
# OpenEmbedded; the defaults below are only for a plain host build.
CFLAGS ?= -O2 -Wall -Wextra
prefix ?= /usr
bindir ?= $(prefix)/bin

all: imulog

# LDFLAGS has to reach the link line, or the OE QA check for GNU_HASH and
# for the distro's hardening flags fails on the resulting binary.
imulog: imulog.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: imulog
	install -d $(DESTDIR)$(bindir)
	install -m 0755 imulog $(DESTDIR)$(bindir)/imulog

clean:
	rm -f imulog

.PHONY: all install clean
