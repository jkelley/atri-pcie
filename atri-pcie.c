/*
 * ATRI PCIe Linux Device Driver
 * based loosely on Xilinx XAPP1052 sample bus master driver
 *
 * John Kelley
 * jkelley@icecube.wisc.edu
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <asm/uaccess.h>

#include "evt_queue.h"
#include "atri-pcie.h"

int              gDrvrMajor = 241;           // Major number not dynamic.
unsigned int     gStatFlags = 0x00;          // Status flags used for cleanup.
unsigned long    gBaseHdwr;                  // Base register address (Hardware address)
unsigned long    gBaseLen;                   // Base register address Length
void            *gBaseVirt = NULL;           // Base register address (Virtual address, for I/O).
char             gDrvrName[]= "atri-pcie";   // Name of driver in proc.
struct pci_dev  *gDev = NULL;                // PCI device structure.
int              gIrq;                       // IRQ assigned by PCI system.

// Device semaphores
struct semaphore gSemOpen;
struct semaphore gSemDMA;
struct semaphore gSemRead;
DEFINE_SEMAPHORE(gSemOpen);
DEFINE_SEMAPHORE(gSemRead);
DEFINE_SEMAPHORE(gSemDMA);

// Queue of DMA buffers for event transfer
evtq           *gEvtQ = NULL;

//-----------------------------------------------------------------------------
// Prototypes
//-----------------------------------------------------------------------------

irq_handler_t xpcie_irq_handler(int irq, void *dev_id, struct pt_regs *regs);
void xpcie_dump_regs(void);
u32 xpcie_read_reg(u32 dw_offset);
void xpcie_write_reg(u32 dw_offset, u32 val);
void xpcie_init_card(void);
void xpcie_initiator_reset(void);
void xpcie_remove(struct pci_dev *dev);
int xpcie_probe(struct pci_dev *dev, const struct pci_device_id *id);
void dma_setup(struct work_struct *work);

// Work queue for DMA setup
static struct workqueue_struct *dma_setup_wq;
static DECLARE_WORK(dma_work, dma_setup);

//-----------------------------------------------------------------------------
// Driver struct
static struct pci_device_id ids[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_XILINX_PCIE) },
    { 0, }
};

MODULE_DEVICE_TABLE(pci, ids);

static struct pci_driver pci_driver = {
    .name = "atri-pcie",
    .id_table = ids,
    .probe = xpcie_probe,
    .remove = xpcie_remove
};

//-----------------------------------------------------------------------------
// Called with device is opened
int xpcie_open(struct inode *inode, struct file *filp) {

    // Limit to one reader at a time
    if (down_trylock(&gSemOpen))
        return -EINVAL;

    gEvtQ = new_evtq(gDev);
    if (gEvtQ == NULL) {
        printk(KERN_ALERT"%s: Open: couldn't create event queue\n",gDrvrName);
        up(&gSemOpen);
        return -ENOMEM;
    }

    // Set up the first DMA transfer
    queue_work(dma_setup_wq, &dma_work);

    // Hold the semaphore    
    printk(KERN_INFO"%s: Open: module opened\n",gDrvrName);    
    return SUCCESS;
}

// Called when device is released
int xpcie_release(struct inode *inode, struct file *filp) {
    // Reset the endpoint to stop any DMA activity
    xpcie_dump_regs();
    xpcie_initiator_reset();
    delete_evtq(gEvtQ);
    up(&gSemOpen);
    printk(KERN_INFO"%s: Release: module released\n",gDrvrName);    
    return SUCCESS;
}

ssize_t xpcie_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {

    evtbuf *eb;
    printk(KERN_INFO"%s: reading %d bytes\n", gDrvrName, (int)count);
        
    if (down_interruptible(&gSemRead))
        return -ERESTARTSYS;

    // Check if event queue is empty 
    while (evtq_isempty(gEvtQ)) {
        up(&gSemRead); 

        // If we're non blocking, return
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        // Otherwise, wait until there is something there
        if (wait_event_interruptible(gEvtQ->rd_waitq, !evtq_isempty(gEvtQ)))
            return -ERESTARTSYS; /* signal caught */

        /* Loop, but first reacquire the lock */
        if (down_interruptible(&gSemRead))
            return -ERESTARTSYS;
    }

    eb = evtq_getevent(gEvtQ, gEvtQ->rd_idx);

    // Make sure buffer is large enough    
    if (eb->len > count) {
        up(&gSemRead);
        return -ENOMEM; // Is this OK?
    }
    if (copy_to_user(buf, eb->buf, eb->len)) {
        up(&gSemRead);
        return -EFAULT;
    }
    
    // Once event has been read, increment the read pointer.
    // Wake up any sleeping write preparation.
    gEvtQ->rd_idx++;
    up(&gSemRead);
    wake_up_interruptible(&gEvtQ->wr_waitq);
    
    printk(KERN_INFO"%s: xpcie_Read: %d bytes have been read...\n", gDrvrName, (int)eb->len);
    return eb->len;
}

// Aliasing write, read, ioctl, etc...
struct file_operations xpcie_intf = {
    read:           xpcie_read,
    // write:          xpcie_Write_Orig,
    // unlocked_ioctl: xpcie_Ioctl,
    open:           xpcie_open,
    release:        xpcie_release,
};

static int __init xpcie_init(void) {
    return pci_register_driver(&pci_driver);
}

static void __exit xpcie_exit(void) {
    pci_unregister_driver(&pci_driver);
}

int xpcie_probe(struct pci_dev *dev, const struct pci_device_id *id) {

    int irqFlags = 0;
    u8 rb;

    // Find the Xilinx PCIE device
    /*
    gDev = pci_get_device(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_XILINX_PCIE, gDev);
    if (NULL == gDev) {
        printk(KERN_WARNING"%s: Init: Hardware not found.\n", gDrvrName);
        return (CRIT_ERR);
    }
    else
        pci_dev_put(gDev);
    */
    gDev = dev;
    
    // Get Base Address of registers from pci structure. Should come from pci_dev
    // structure, but that element seems to be missing on the development system.
    gBaseHdwr = pci_resource_start(gDev, 0);
    
    if (0 > gBaseHdwr) {
        printk(KERN_WARNING"%s: Init: Base Address not set.\n", gDrvrName);
        return (CRIT_ERR);
    } 
    printk(KERN_INFO"%s: Init: Base hw val %lx\n", gDrvrName, (unsigned long)gBaseHdwr);
    
    // Get the Base Address Length
    gBaseLen = pci_resource_len (gDev, 0);
    printk(KERN_INFO"%s: Init: Base hw len %d\n", gDrvrName, (unsigned int)gBaseLen);
    
    // Remap the I/O register block so that it can be safely accessed.
    // I/O register block starts at gBaseHdwr and is 32 bytes long.
    gBaseVirt = ioremap(gBaseHdwr, gBaseLen);
    if (!gBaseVirt) {
        printk(KERN_WARNING"%s: Init: Could not remap memory.\n", gDrvrName);
        return (CRIT_ERR);
    } 
    printk(KERN_INFO"%s: Init: Virt HW address %lX\n", gDrvrName, (unsigned long)gBaseVirt);
    
    // Get IRQ from pci_dev structure. It may have been remapped by the kernel,
    // and this value will be the correct one.
    gIrq = gDev->irq;
    printk(KERN_INFO"%s: Init: Device IRQ: %d\n",gDrvrName, gIrq);
    
    //---START: Initialize Hardware
    
    // Check the memory region to see if it is in use
    if (0 > check_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE)) {
        printk(KERN_WARNING"%s: Init: Memory in use.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    // Try to gain exclusive control of memory for demo hardware.
    request_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE, "3GIO_Demo_Drv");
    // Update flags
    gStatFlags = gStatFlags | HAVE_REGION;
    
    printk(KERN_INFO"%s: Init: Initialize Hardware Done..\n",gDrvrName);
    
    printk(KERN_INFO"%s: IRQ Setup..\n", gDrvrName);
    // Request IRQ from OS
    // Try to get an MSI interrupt
    if (PCI_USE_MSI) {
        if (pci_enable_msi(gDev) < 0) {
            printk(KERN_WARNING"%s: Init: Unable to enable MSI",gDrvrName);    
            return (CRIT_ERR);
        }
    }
    else {
        irqFlags |= IRQF_SHARED;
        // FIX ME this is probably unneccessary
        // but interrupts are currently broken on endpoint
        if (pci_read_config_byte(gDev, PCI_INTERRUPT_LINE, &rb) != 0) {
            printk(KERN_WARNING"%s: could not read IRQ number from configuration\n",gDrvrName);
            return (CRIT_ERR);
        }
        //gIrq = rb;
        printk(KERN_INFO"%s: configuration space says IRQ number is %d\n",gDrvrName,(int)rb);
    }
    
    if (0 > request_irq(gIrq, (irq_handler_t) xpcie_irq_handler, irqFlags, gDrvrName, gDev)) {
        printk(KERN_WARNING"%s: Init: Unable to allocate IRQ",gDrvrName);
        return (CRIT_ERR);
    }
    // Update flags stating IRQ was successfully obtained
    gStatFlags = gStatFlags | HAVE_IRQ;
    
    // Bus Master Enable
    if (0 > pci_enable_device(gDev)) {
        printk(KERN_WARNING"%s: Init: Device not enabled.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    // Set address range for DMA transfers
    if (pci_set_dma_mask(gDev, PCI_HW_DMA_MASK) < 0) {
        printk(KERN_WARNING"%s: Init: DMA mask could not be set.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    //--- END: Initialize Hardware
    
    //--- START: Register Driver
    
    // Register with the kernel as a character device.
    if (0 > register_chrdev(gDrvrMajor, gDrvrName, &xpcie_intf)) {
        printk(KERN_WARNING"%s: Init: will not register\n", gDrvrName);
        return (CRIT_ERR);
    }
    printk(KERN_INFO"%s: Init: module registered\n", gDrvrName);
    gStatFlags = gStatFlags | HAVE_KREG;
    
    //--- END: Register Driver
    
    // Create DMA workqueue
    dma_setup_wq = create_singlethread_workqueue("atri-pcie-dma-work");
    if (dma_setup_wq == NULL) {
        printk(KERN_WARNING"%s: Init: couldn't create DMA workqueue\n", gDrvrName);
        return (CRIT_ERR);
    }
    gStatFlags = gStatFlags | HAVE_WQ;
    
    printk(KERN_ALERT"%s driver is loaded\n", gDrvrName);
    
    // Initializing card registers
    xpcie_init_card();
    
    return 0;
}

//--- xpcie_initiator_reset(): Resets the Xilinx reference design
void xpcie_initiator_reset() {
  // Reset device and then make it active
  xpcie_write_reg(REG_DCSR, DCSR_RESET);
  xpcie_write_reg(REG_DCSR, DCSR_ACTIVE);
}

//--- xpcie_init_card(): Initializes XBMD descriptor registers to default values
void xpcie_init_card() {
  xpcie_initiator_reset();
}

// Performs any cleanup required before releasing the device
void xpcie_remove(struct pci_dev *dev) {

    // Flush the DMA workqueue and destroy it
    printk(KERN_INFO"%s: destroy workqueue\n", gDrvrName);
    if (gStatFlags & HAVE_WQ) {
        flush_workqueue(dma_setup_wq);
        destroy_workqueue(dma_setup_wq);
    }
    
    printk(KERN_INFO"%s: Release memory\n",gDrvrName);
    // Check if we have a memory region and free it
    if (gStatFlags & HAVE_REGION)
        (void) release_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE);
    
    // Check if we have an IRQ and free it
    printk(KERN_INFO"%s: Free IRQ\n",gDrvrName);  
    pci_disable_msi(gDev);
    if (gStatFlags & HAVE_IRQ) {
        (void) free_irq(gIrq, gDev);
    }
    
    // Free up memory pointed to by virtual address
    printk(KERN_INFO"%s: unmap memory\n",gDrvrName);  
    if (gBaseVirt != NULL)
        iounmap(gBaseVirt);    
    gBaseVirt = NULL;
    
    // Unregister Device Driver
    printk(KERN_DEBUG"%s: unregister driver\n",gDrvrName);    
    if (gStatFlags & HAVE_KREG) {
        unregister_chrdev(gDrvrMajor, gDrvrName);
    }  
    gStatFlags = 0;
    
    printk(KERN_ALERT"%s driver is unloaded\n", gDrvrName);
}

irq_handler_t xpcie_irq_handler(int irq, void *dev_id, struct pt_regs *regs) {

    // Check that the DMA transfer is done
    // Otherwise this is probably not for us
    //  u32 reg;
    //reg = xpcie_read_reg(REG_DDMACR);    
    //if (!(reg & DDMACR_WR_DONE))

    // Check interrupt pending bit in PCI config space
    if (!(PCI_USE_MSI) && (!pci_check_and_mask_intx(gDev)))
        return IRQ_NONE;

    printk(KERN_INFO"%s: Interrupt Handler Start ..",gDrvrName);
    
    // Reset the initiator.  This also clears the DONE bit.
    xpcie_initiator_reset();
    
    // Data is now ready for processer. Increment the write pointer
    // and wake up and waiting reads
    gEvtQ->wr_idx++;
    wake_up_interruptible(&gEvtQ->rd_waitq);
   
    // Put the setup for the next write into a workqueue.
    // It can sleep so cannot be done here
    queue_work(dma_setup_wq, &dma_work);
    
    printk(KERN_INFO"%s Interrupt Handler End ..\n", gDrvrName);
    return (irq_handler_t) IRQ_HANDLED;
}

void dma_setup(struct work_struct *work) {
    evtbuf *eb;
    printk(KERN_INFO"%s: DMA write setup\n", gDrvrName);

    if (down_interruptible(&gSemDMA))
        return;

    // If the queue is full, wait until it is not    
    while (evtq_isfull(gEvtQ)) {
        if (wait_event_interruptible(gEvtQ->wr_waitq, !evtq_isfull(gEvtQ)))
            continue;
    }

    eb = evtq_getevent(gEvtQ, gEvtQ->wr_idx);        
    printk(KERN_INFO"%s: DMA setup address: %lx\n", gDrvrName, (unsigned long)eb->physaddr);
    // Write the PCIe write DMA address to the device
    xpcie_write_reg(REG_WDMATLPA, eb->physaddr);

    // TEMP FIX ME: this is for the sample firmware only
    // Write: Write DMA Expected Data Pattern with default value (feedbeef)    
    xpcie_write_reg(REG_WDMATLPP, 0xfeedbeef);
    // Write: Write DMA TLP Size register (32dwords)    
    xpcie_write_reg(REG_WDMATLPS, 0x20);
    // Write: Write DMA TLP Count register
    xpcie_write_reg(REG_WDMATLPC, 0x0001);
    
    // Enable interrupts
    pci_intx(gDev, 1);

    // Tell the device to start DMA
    xpcie_write_reg(REG_DDMACR, DDMACR_WR_START);
    up(&gSemDMA);    
}

void xpcie_dump_regs(void) {
    u32 i, regx;
    for (i = 0; i < 13; i++) {
        regx = xpcie_read_reg(i);
        printk(KERN_WARNING"%s : REG<%d> : 0x%X\n", gDrvrName, i, regx);
    }    
}

u32 xpcie_read_reg(u32 dw_offset) {
    u32 ret = 0;
    printk(KERN_INFO"%s Read Register %d\n", gDrvrName, dw_offset);
    ret = readl(gBaseVirt + (4 * dw_offset));
    return ret; 
}

void xpcie_write_reg(u32 dw_offset, u32 val) {
	printk(KERN_INFO"%s Write Register %d Value %x\n", gDrvrName,
	       dw_offset, val);  
    writel(val, (gBaseVirt + (4 * dw_offset)));
}

module_init(xpcie_init);
module_exit(xpcie_exit);

MODULE_LICENSE("Dual BSD/GPL");

