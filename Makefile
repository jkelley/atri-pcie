#
# Makefile for ATRI PCIe driver
# John Kelley, jkelley@icecube.wisc.edu
#
obj-m := atri-pcie.o

dev_name += atri-pcie
module_home := $(shell pwd)
linux_rev := $(shell uname -r)
etc_modules_check := $(shell grep -c $(dev_name) /etc/modules)

all: module test device

test: readtest.c
	gcc -g -o readtest readtest.c

module:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) modules

install: module
	cp $(dev_name).ko /lib/modules/$(linux_rev)/
	depmod -a
	if [ $(etc_modules_check) -eq 0 ]; then echo $(dev_name) >> /etc/modules; fi
	cp $(dev_name).rules /etc/udev/rules.d/10_$(dev_name).rules

device:
	rm -rf /dev/$(dev_name)
	mknod /dev/$(dev_name) c 241 1
	chown root /dev/$(dev_name)
	chmod 0666 /dev/$(dev_name)

clean:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) clean
	rm -f readtest

