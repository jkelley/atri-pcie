#
# Makefile for ATRI PCIe driver
# John Kelley, jkelley@icecube.wisc.edu
#

# Set to y to enable debug printks
DEBUG = n

# Add to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DATRI_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif
ccflags-y := $(DEBFLAGS)

obj-m := atri-pcie.o

dev_name += atri-pcie
module_home := $(shell pwd)
linux_rev := $(shell uname -r)
etc_modules_check := $(shell grep -c $(dev_name) /etc/modules)
rclocal_done_check := $(shell grep -c atri /etc/rc.local)

all: module test device

test: readtest.c
	gcc -g -o readtest readtest.c

module:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) modules

install: module
	cp $(dev_name).ko /lib/modules/$(linux_rev)/
	depmod -a
	if [ $(etc_modules_check) -eq 0 ]; then echo $(dev_name) >> /etc/modules; fi
	if [ $(rclocal_done_check) -eq 0 ]; \
	then if [ -f /etc/rc.local ]; then \
		echo "Modifying rc.local"; \
		head -n -1 /etc/rc.local > /tmp/rc.local.new; \
		tail -n +2 mkatridev.sh >> /tmp/rc.local.new; \
		tail -n 1 /etc/rc.local >> /tmp/rc.local.new; \
		mv /tmp/rc.local.new /etc/rc.local; \
	else \
		cp mkatridev.sh /etc/rc.local; \
		fi; \
	fi

device:
	rm -rf /dev/$(dev_name)
	mknod /dev/$(dev_name) c 241 1
	chown root /dev/$(dev_name)
	chmod 0666 /dev/$(dev_name)

clean:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) clean
	rm -f readtest

