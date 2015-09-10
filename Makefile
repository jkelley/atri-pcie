#
# Makefile for ATRI PCIe driver
# John Kelley, jkelley@icecube.wisc.edu
#
obj-m := atri-pcie.o
dev_name += atri-pcie
module_home := $(shell pwd)
linux_rev := $(shell uname -r)

all:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) modules

module:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) modules

device:
	rm -rf /dev/$(dev_name)
	mknod /dev/$(dev_name) c 241 1
	chown root /dev/$(dev_name)
	chmod 0666 /dev/$(dev_name)

clean:
	make -C /lib/modules/$(linux_rev)/build M=$(module_home) clean
