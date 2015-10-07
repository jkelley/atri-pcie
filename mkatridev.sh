#!/bin/sh
# Make the device file for the ATRI PCIe driver.
# Would be nice to move this to udev
#
# J. Kelley <jkelley@icecube.wisc.edu>
#
rm -rf /dev/atri-pcie
mknod /dev/atri-pcie c 241 1
chown root /dev/atri-pcie
chmod 0666 /dev/atri-pcie

