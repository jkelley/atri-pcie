# atri-pcie
Linux device driver for use with the ARA ATRI PCIe link
Targeted for kernel v3.4.x.

Intro
---

This driver creates a read-only character device `/dev/atri-pcie` for reading
event data from the ATRI FPGA over the PCIe bus.  The PCI device on the
ATRI side is a bus-master endpoint that can transfer events via DMA to
kernel buffers allocated by the driver.  The driver uses a DMA ring buffer
to buffer transferred events before they are read out by a user.

Currently the device can only be opened by a single process at a time and
is not intended for multiprocess / multithreaded reading.

Building
---

To build the module, test program, and create the device files, execute:

<pre><code>
$ make module
$ make test
$ sudo make device
</code></pre>

or simply `sudo make all`.

Install
---

The driver can be loaded with `sudo insmod atri-pcie.ko`.

Testing
---

The simple program `readtest` will read a certain number of events from the
device file, report how many bytes were transferred from the endpoint, and
spit out the first few bytes of each.  

<pre><code>
$ ./readtest 10
</code></pre>

