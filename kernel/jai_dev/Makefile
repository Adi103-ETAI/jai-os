obj-m += jai_dev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

CCACHE := $(shell command -v ccache >/dev/null 2>&1 && echo ccache)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(if $(CCACHE),CC="$(CCACHE) gcc") modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Helper targets — loading requires root
load: all
	sudo insmod jai_dev.ko
	@echo "loaded — try: cat /dev/jai"

unload:
	sudo rmmod jai_dev

test: load
	@echo "--- read test:"; cat /dev/jai
	@echo "--- write test:"; echo "hello from userland" | tee /dev/jai >/dev/null
	@echo "--- kernel log:"; dmesg | tail -3

.PHONY: all clean load unload test
