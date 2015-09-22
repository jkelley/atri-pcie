# atri-pcie
Linux device driver for use with the ARA ATRI PCIe link

Targeted for kernel v3.4.x.

This driver creates a read-only character device /dev/atri-pcie for reading
event data from the ATRI FPGA over the PCIe bus.  The PCI device on the
ATRI side is a bus-master endpoint that can transfer events via DMA to
kernel buffers allocated by the driver.  The driver uses a DMA ring buffer
to buffer transferred events before they are read out by a user.

Currently the device can only be opened by a single process at a time and
is not intended for multithreaded reading.

