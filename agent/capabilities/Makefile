# jai-capabilities - measure the machine at boot (roadmap 2b)
# Same recipe as the other JAI C tools (jai-mcp, jai-eventd):
# gcc -O2 -Wall -Wextra, json-c as the only library.
#
# NOTE (the phase2 5m lesson): for image bakes this is built INSIDE the
# chroot against the rootfs glibc - build-host != target. Live-VM install
# without a re-bake: scripts/vm-capabilities.sh.
#
# JAI_ETC_DIR is a compile-time override for TESTS only (default /etc/jai).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -ljson-c

all: jai-capabilities

jai-capabilities: jai-capabilities.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: jai-capabilities
	install -D -m 0755 jai-capabilities $(DESTDIR)/usr/local/bin/jai-capabilities

clean:
	rm -f jai-capabilities

.PHONY: all install clean
