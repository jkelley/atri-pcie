# atri-pcie
Linux device driver for use with the ARA (Askaryan Radio Array) ATRI PCIe
link.  Targeted for 3.4 kernel; compiled but not tested on 3.10 kernel.

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

After a reboot the device files have to recreated with `sudo make device`.
To see all of the driver debug messages in `/var/log/kern.log`, make sure
the kernel print level is set to at least 6 (KERN_INFO), using the
following:

<pre><code>
$ sudo bash
# echo 6 > /proc/sys/kernel/printk
</code></pre>

Finally, the driver can be loaded with `sudo insmod atri-pcie.ko`.
Messages in `/var/log/kern.log` will indicate if this is successful.

Testing
---

The simple program `readtest` will read a certain number of events from the
device file, report how many bytes were transferred from the endpoint, and
spit out the first few bytes of each.  

<pre><code>
$ ./readtest 10
</code></pre>

Once these events are read, the driver will continue issuing DMA requests
to the endpoint until the driver's internal ring buffer is full.  Then, it
will wait for the reader to empty some space so it can continue.


