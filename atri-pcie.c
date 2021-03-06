
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
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/random.h>
#include <linux/timer.h>
#include <asm/uaccess.h>

#include "atri-pcie.h"
#include "evt_queue.h"

int              gDrvrMajor = 241;           // Major number not dynamic.
unsigned int     gStatFlags = 0x00;          // Status flags used for cleanup.
unsigned long    gBaseHdwr;                  // Base register address (Hardware address)
unsigned long    gBaseLen;                   // Base register address Length
void            *gBaseVirt = NULL;           // Base register address (Virtual address, for I/O).
char             gDrvrName[]= "atri-pcie";   // Name of driver in proc.
struct pci_dev  *gDev = NULL;                // PCI device structure.
int              gDie = 0;                   // Global shutdown flag to die gracefully
int              gReadAbort = 0;             // Global read abort flag when released

// Test pattern counter
int              gXferCount = 1;             // Debug test pattern counter

// Device semaphores
DEFINE_SEMAPHORE(gSemOpen);
DEFINE_SEMAPHORE(gSemRead);

// Dropped interrupt timer 
static struct timer_list irq_timer;

//  DMA ring buffer for event transfer
evtq           *gEvtQ = NULL;

//-----------------------------------------------------------------------------
// Prototypes
//-----------------------------------------------------------------------------

irq_handler_t xpcie_irq_handler(int irq, void *dev_id, struct pt_regs *regs);
void irq_timer_callback(unsigned long data);
void xpcie_dump_regs(void);
u32 xpcie_read_reg(u32 dw_offset);
void xpcie_write_reg(u32 dw_offset, u32 val);
void xpcie_init_card(void);
void xpcie_initiator_reset(void);
unsigned int xpcie_get_transfer_size(void);
int xpcie_dma_wr_done(void);
void xpcie_remove(struct pci_dev *dev);
void xpcie_queue_flush(void);
int xpcie_probe(struct pci_dev *dev, const struct pci_device_id *id);
void dma_setup(struct work_struct *work);

// Work queue for DMA setup
static struct workqueue_struct *dma_setup_wq;
static DECLARE_WORK(dma_work, dma_setup);

//-----------------------------------------------------------------------------
// PCI driver struct
// defines main probe (initialization) and removal functions
// to kernel along with devices IDs that we handle
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
// Device open / close (release)
//

int xpcie_open(struct inode *inode, struct file *filp) {

    // Limit to one reader at a time
    // Hold the semaphore until close    
    if (down_trylock(&gSemOpen))
        return -EINVAL;

    // Reset any previous abort flags
    gReadAbort = gDie = 0;
    
    // Set up the first DMA transfer
    queue_work(dma_setup_wq, &dma_work);

    PDEBUG("%s: Open: module opened\n",gDrvrName);    
    return SUCCESS;
}

int xpcie_release(struct inode *inode, struct file *filp) {

    // Bail out of any waiting reads
    gReadAbort = 1;
    wake_up_interruptible(&gEvtQ->rd_waitq);    

    // Stop the IRQ timeout timer
    del_timer(&irq_timer);

    // Release the single-reader lock
    up(&gSemOpen);
    PDEBUG("%s: Release: device released\n",gDrvrName);    
    return SUCCESS;
}

//-----------------------------------------------------------------------------
// Device read
//
ssize_t xpcie_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {

    evtbuf *eb;
    size_t nbytes;
    int next_event = 0;
    
    PDEBUG("%s: reading %d bytes (offset %d)\n", gDrvrName, (int)count, (int)*f_pos);

    if (down_interruptible(&gSemRead))
        return -ERESTARTSYS;
    
    // Check if event queue is empty
    // FIX ME: this lock may not be necessary since the open() is locked
    while (evtq_isempty(gEvtQ) && !gReadAbort) {
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

    // If we're about to shutdown, don't go any further
    if (gReadAbort) {
        up(&gSemRead);
        return 0;
    }
    
    eb = evtq_getevent(gEvtQ, gEvtQ->rd_idx);

    // TEMP FIX ME DEBUG
    /*
    PDEBUG("%s: buffer bytes: %02x %02x %02x %02x %02x %02x %02x %02x...\n", gDrvrName,
           eb->buf[0], eb->buf[1], eb->buf[2], eb->buf[3],
           eb->buf[4], eb->buf[5], eb->buf[6], eb->buf[7]);           
    */

    // See how many more bytes are available in this event    
    if ((eb->len - *f_pos) <= count) {
        nbytes = (eb->len - *f_pos);
        next_event = 1;
    }
    else
        nbytes = count;
    
    if (copy_to_user(buf, &(eb->buf[*f_pos]), nbytes)) {
        up(&gSemRead);
        return -EFAULT;
    }

    // Have we wrapped into a new event?
    if (next_event) {        
        // Once event has been read, increment the read pointer.
        // Wake up any sleeping write preparation.
        // FIX ME: should this be atomic?
        gEvtQ->rd_idx++;
        wake_up_interruptible(&gEvtQ->wr_waitq);
        *f_pos = 0;
    }
    else {
        *f_pos += nbytes;
    }
    
    up(&gSemRead);

    PDEBUG("%s: xpcie_read: %d bytes have been read...\n", gDrvrName, (int)nbytes);
    return nbytes;
}

//
// xpcie_ioctl: (limited) driver control via IOCTL operations
//
long xpcie_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
  
  long ret = SUCCESS;
  
  switch (cmd) {
      
  case XPCIE_IOCTL_INIT:          // Initialize the firmware
      printk(KERN_INFO "%s: ioctl INIT\n", gDrvrName);
      xpcie_init_card();
      break;
  case XPCIE_IOCTL_FLUSH:         // Flush the event queue
      // FIX ME: this could hose stuff if called at the wrong time?
      printk(KERN_INFO "%s: ioctl FLUSH\n", gDrvrName);      
      xpcie_queue_flush();
      break;
  default:
      break;
  }
  
  return ret;
}

//-----------------------------------------------------------------------------
// Module file operations and init/exit.  Real initialization and exit
// done by probe/remove; this just registers the driver structure.
//

struct file_operations xpcie_intf = {
    read:           xpcie_read,
    unlocked_ioctl: xpcie_ioctl,    
    open:           xpcie_open,
    release:        xpcie_release,
};

static int __init xpcie_init(void) {
    return pci_register_driver(&pci_driver);
}

static void __exit xpcie_exit(void) {
    pci_unregister_driver(&pci_driver);
}

//-----------------------------------------------------------------------------
// Device probe and remove: since we're not hotplugging, called on
// module load and remove
//
int xpcie_probe(struct pci_dev *dev, const struct pci_device_id *id) {

    int irqFlags = 0;

    // Kernel has found a device for us
    gDev = dev;

    // Enable device
    if (0 > pci_enable_device(gDev)) {
        printk(KERN_WARNING "%s: probe: Device not enabled.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    // Get Base Address of registers from pci structure. Should come from pci_dev
    // structure, but that element seems to be missing on the development system.
    gBaseHdwr = pci_resource_start(gDev, 0);
    
    if (0 > gBaseHdwr) {
        printk(KERN_WARNING "%s: probe: Base Address not set.\n", gDrvrName);
        return (CRIT_ERR);
    } 
    PDEBUG("%s: probe: Base hw val %lx\n", gDrvrName, (unsigned long)gBaseHdwr);
    
    // Get the Base Address Length
    gBaseLen = pci_resource_len(gDev, 0);
    PDEBUG("%s: probe: Base hw len %d\n", gDrvrName, (unsigned int)gBaseLen);
    
    // Remap the I/O register block so that it can be safely accessed.
    // I/O register block starts at gBaseHdwr and is 32 bytes long.
    gBaseVirt = ioremap(gBaseHdwr, gBaseLen);
    if (!gBaseVirt) {
        printk(KERN_WARNING "%s: probe: Could not remap memory.\n", gDrvrName);
        return (CRIT_ERR);
    } 
    PDEBUG("%s: probe: Virt HW address %lX\n", gDrvrName, (unsigned long)gBaseVirt);
        
    // Check the memory region to see if it is in use
    if (0 > check_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE)) {
        printk(KERN_WARNING "%s: probe: Memory in use.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    // Try to gain exclusive control of memory for demo hardware.
    request_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE, "3GIO_Demo_Drv");
    // Update flags
    gStatFlags = gStatFlags | HAVE_REGION;
    
    PDEBUG("%s: probe: Initialize Hardware Done..\n",gDrvrName);
    
    PDEBUG("%s: IRQ Setup..\n", gDrvrName);
    // Request IRQ from OS
    // Try to get an MSI interrupt
    if (PCI_USE_MSI) {
        if (pci_enable_msi(gDev) < 0) {
            printk(KERN_WARNING "%s: probe: Unable to enable MSI",gDrvrName);    
            return (CRIT_ERR);
        }        
        PDEBUG("%s: MSI interrupt; device IRQ is %d\n", gDrvrName, gDev->irq);
    }
    else {
        irqFlags |= IRQF_SHARED;
        PDEBUG("%s: shared interrupt; device IRQ is %d\n", gDrvrName, gDev->irq);        
    }
    
    if (0 > request_irq(gDev->irq, (irq_handler_t) xpcie_irq_handler, irqFlags, gDrvrName, gDev)) {
        printk(KERN_WARNING "%s: probe: Unable to allocate IRQ",gDrvrName);
        return (CRIT_ERR);
    }
    // Update flags stating IRQ was successfully obtained
    gStatFlags = gStatFlags | HAVE_IRQ;
        
    // Set address range for DMA transfers
    if (pci_set_dma_mask(gDev, PCI_HW_DMA_MASK) < 0) {
        printk(KERN_WARNING "%s: probe: DMA mask could not be set.\n", gDrvrName);
        return (CRIT_ERR);
    }
    
    //--- END: Initialize Hardware
    
    //--- START: Register Driver
    
    // Register with the kernel as a character device.
    if (0 > register_chrdev(gDrvrMajor, gDrvrName, &xpcie_intf)) {
        printk(KERN_WARNING "%s: probe: will not register\n", gDrvrName);
        return (CRIT_ERR);
    }
    PDEBUG("%s: probe: module registered\n", gDrvrName);
    gStatFlags = gStatFlags | HAVE_KREG;
    
    //--- END: Register Driver
    
    // Create DMA workqueue
    dma_setup_wq = create_singlethread_workqueue("atri-pcie-dma-work");
    if (dma_setup_wq == NULL) {
        printk(KERN_WARNING "%s: probe: couldn't create DMA workqueue\n", gDrvrName);
        return (CRIT_ERR);
    }
    gStatFlags = gStatFlags | HAVE_WQ;

    // Create event queue
    gEvtQ = new_evtq(gDev);
    if (gEvtQ == NULL) {
        printk(KERN_ALERT "%s: Open: couldn't create event queue\n",gDrvrName);
        return (CRIT_ERR);
    }
    
    // Initialize card registers
    xpcie_init_card();

    // Set up (but don't arm) interrupt timer
    setup_timer(&irq_timer, irq_timer_callback, 0);

    printk(KERN_ALERT "%s: driver is loaded\n", gDrvrName);
        
    return 0;
}

// Performs any cleanup required before removing the device
void xpcie_remove(struct pci_dev *dev) {

    // Delete the interrupt timer
    del_timer_sync(&irq_timer);

    // Set the abort flags
    gReadAbort = gDie = 1;

    // Wake up any sleeping DMA setup and reads and don't restart
    if (gEvtQ != NULL) {
        PDEBUG("%s: empty event queue\n", gDrvrName);
        xpcie_queue_flush();
    }
        
    // Flush the DMA workqueue and destroy it
    if (gStatFlags & HAVE_WQ) {
        PDEBUG("%s: destroy workqueue\n", gDrvrName);        
        flush_workqueue(dma_setup_wq);
        destroy_workqueue(dma_setup_wq);
    }
    
    // Check if we have a memory region and free it
    if (gStatFlags & HAVE_REGION) {
        PDEBUG("%s: release memory\n",gDrvrName);        
        release_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE);
    }
    
    // Check if we have an IRQ and free it
    if (gStatFlags & HAVE_IRQ) {
        PDEBUG("%s: free IRQ %d\n",gDrvrName, gDev->irq);    
        free_irq(gDev->irq, gDev);
        if (PCI_USE_MSI)
            pci_disable_msi(gDev);    
    }
    
    // Free up memory pointed to by virtual address
    if (gBaseVirt != NULL) {
        PDEBUG("%s: unmap memory\n",gDrvrName);          
        iounmap(gBaseVirt);
        gBaseVirt = NULL;
    }
    
    // Unregister Device Driver
    if (gStatFlags & HAVE_KREG) {
        PDEBUG("%s: unregister driver\n",gDrvrName);        
        unregister_chrdev(gDrvrMajor, gDrvrName);
    }  
    gStatFlags = 0;

    // Release event queue memory
    PDEBUG("%s: delete event queue structure\n",gDrvrName);
    delete_evtq(gEvtQ);
    
    printk(KERN_ALERT "%s driver is unloaded\n", gDrvrName);
}

//-----------------------------------------------------------------------------
// Interrupt handling and DMA setup
//

irq_handler_t xpcie_irq_handler(int irq, void *dev_id, struct pt_regs *regs) {

    unsigned long flags;
    evtbuf *eb;
    
    spin_lock_irqsave(&gEvtQ->lock, flags);

    // Disable the lost interrupt timer
    del_timer(&irq_timer);
    
    PDEBUG("%s: Interrupt Handler Start ..",gDrvrName);

    if (!gDie) {
        // Read out the actual transfer length and set in event    
        eb = evtq_getevent(gEvtQ, gEvtQ->wr_idx);
        eb->len = xpcie_get_transfer_size();
    
        // Data is now ready for processer. Increment the write pointer
        // and wake up and waiting reads
        gEvtQ->wr_idx++;
        gXferCount++;
    }    
    gEvtQ->dma_started = 0;    
    spin_unlock_irqrestore(&gEvtQ->lock, flags);
    
    wake_up_interruptible(&gEvtQ->rd_waitq);
   
    // Put the setup for the next write into a workqueue.
    // It can sleep so cannot be done here
    if (!gDie)
        queue_work(dma_setup_wq, &dma_work);
    
    PDEBUG("%s evt_queue: %u events\n", gDrvrName, evtq_entries(gEvtQ));
    PDEBUG("%s Interrupt Handler End ..\n", gDrvrName);

    return (irq_handler_t) IRQ_HANDLED;
}

void dma_setup(struct work_struct *work) {
    evtbuf *eb;
    u32 tlp_cnt;
    unsigned long flags;
    
    PDEBUG("%s: DMA write setup\n", gDrvrName);

    // This part is locked against the top half of the interrupt
    // handler.  Otherwise we could send the wrong address.
    spin_lock_irqsave(&gEvtQ->lock, flags);

    // Have we already done this, but not received an interrupt?
    if (gEvtQ->dma_started) {
        printk(KERN_WARNING "%s: dma_setup: DMA is already in progress, not starting another!\n",gDrvrName);
        spin_unlock(&gEvtQ->lock);
        return;        
    }
    
    // If the queue is full, wait until it is not    
    while (evtq_isfull(gEvtQ) && !gDie) {
        // but don't hold the lock
        spin_unlock(&gEvtQ->lock);
        if (wait_event_interruptible(gEvtQ->wr_waitq, !evtq_isfull(gEvtQ)))
            continue;
        // Reaquire lock
        spin_lock_irqsave(&gEvtQ->lock, flags);
    }

    // If we're about to shutdown, don't go any further
    if (gDie) {
        spin_unlock(&gEvtQ->lock);
        return;
    }

    PDEBUG("%s: DMA is%s done\n", gDrvrName,
                       xpcie_dma_wr_done() ? "" : " NOT");

    eb = evtq_getevent(gEvtQ, gEvtQ->wr_idx);        

    // Reset the initiator.  This also clears the DONE bit.
    if (XILINX_TEST_MODE)
        xpcie_initiator_reset();
    
    // Write the PCIe write DMA address to the device
    xpcie_write_reg(REG_WDMATLPA, eb->physaddr);

    // For testing with Xilinx XAPP1052 firmware
    if (XILINX_TEST_MODE) {
        // Write: Write DMA Expected Data Pattern with default value
        xpcie_write_reg(REG_WDMATLPP, gXferCount);
        // Write: Write DMA TLP Size register (4 dwords)
        xpcie_write_reg(REG_WDMATLPS, 0x4);
        // Write: Write DMA TLP Count register (randomize!)
        get_random_bytes(&tlp_cnt, 4);
        xpcie_write_reg(REG_WDMATLPC, (tlp_cnt&0x7ff)+1);
    }
    else {
        // Overloaded: additional waiting time for transfer start: 
        // nwords(16 downto 0) + ('0' & nwords(16 downto 0)) - REG_RDMATLPP
        xpcie_write_reg(REG_RDMATLPP, 0);
    }    
    mmiowb();

    // Tell the device to start DMA
    xpcie_write_reg(REG_DDMACR, DDMACR_WR_START);
    mmiowb();

    // Record that we've started a DMA
    gEvtQ->dma_started = 1;

    // Set up a timer in case we lose the interrupt
    irq_timer.expires = jiffies+msecs_to_jiffies(IRQ_TIMEOUT_MS);
    add_timer(&irq_timer);
    
    spin_unlock(&gEvtQ->lock);    
}

// Timer is fired if we don't receive an interrupt for
// a certain period of time
void irq_timer_callback(unsigned long data) {

    unsigned long flags;
    
    spin_lock_irqsave(&gEvtQ->lock, flags);
    printk(KERN_WARNING "%s: no IRQ in %d ms!\n",gDrvrName, IRQ_TIMEOUT_MS);
    
    // Did we somehow forget to set up a transfer?  
    if (!(gEvtQ->dma_started)) {
        printk(KERN_WARNING "%s: irq timeout: setting up another transfer.\n",gDrvrName);
        queue_work(dma_setup_wq, &dma_work);
    }
    else {
        // If we started a transfer but just never got the interrupt,
        // check to see if it's done
        if (xpcie_dma_wr_done()) {
            printk(KERN_WARNING "%s: irq timeout: DMA done; force call to handler.\n",gDrvrName);
            // Call the interrupt handler ourselves!
            xpcie_irq_handler(gDev->irq, NULL, NULL);
        }
        else {
            // DMA was started but is not done.  That is probably bad.
            printk(KERN_WARNING "%s: irq timeout: DMA started but not done; trying again.\n",gDrvrName);
            gEvtQ->dma_started = 0;            
            xpcie_initiator_reset();
            queue_work(dma_setup_wq, &dma_work);
        }
    }

    spin_unlock_irqrestore(&gEvtQ->lock, flags);    

    return;       
}

// Queue flush
void xpcie_queue_flush(void) {

    unsigned long flags;
    
    // Lock the event queue and empty it
    spin_lock_irqsave(&gEvtQ->lock, flags);
    empty_evtq(gEvtQ);
    spin_unlock_irqrestore(&gEvtQ->lock, flags);

    // Wake up stuff that was waiting
    wake_up_interruptible(&gEvtQ->wr_waitq);
    wake_up_interruptible(&gEvtQ->rd_waitq);
    
    return;
}

//-----------------------------------------------------------------------------
// Device control functions

//--- xpcie_initiator_reset(): Resets the Xilinx reference design
void xpcie_initiator_reset() {
  // Reset device and then make it active
  xpcie_write_reg(REG_DCSR, DCSR_RESET);
  mmiowb();
  xpcie_write_reg(REG_DCSR, DCSR_ACTIVE);
  mmiowb();
}

//--- xpcie_init_card(): Initializes XBMD descriptor registers to default values
void xpcie_init_card() {
  xpcie_initiator_reset();
}

void xpcie_dump_regs(void) {
    u32 i, regx;
    for (i = 0; i < 13; i++) {
        regx = xpcie_read_reg(i);
        printk(KERN_WARNING "%s : REG<%d> : 0x%X\n", gDrvrName, i, regx);
    }    
}

u32 xpcie_read_reg(u32 dw_offset) {
    u32 ret = 0;
    ret = readl(gBaseVirt + (4 * dw_offset));
    PDEBUG("%s Read Register %d Value %x\n", gDrvrName, dw_offset, ret);    
    return ret; 
}

void xpcie_write_reg(u32 dw_offset, u32 val) {
	PDEBUG("%s Write Register %d Value %x\n", gDrvrName,
                       dw_offset, val);  
    writel(val, (gBaseVirt + (4 * dw_offset)));
}

unsigned int xpcie_get_transfer_size(void) {
    u32 tlp_size, tlp_cnt, tlp_hw_cnt;
    u32 bytes;
    tlp_size = xpcie_read_reg(REG_WDMATLPS) & DMA_TLP_SIZE_MASK;
    tlp_cnt = xpcie_read_reg(REG_WDMATLPC) & DMA_TLP_CNT_MASK;
    if (XILINX_TEST_MODE) {
        bytes = (tlp_size*tlp_cnt*4);
    }
    else {
        tlp_hw_cnt = xpcie_read_reg(REG_WDMATLPEX);
        bytes = tlp_hw_cnt*2;
        PDEBUG("%s transfer size %u B (%u halfwords)\n",
               gDrvrName, bytes, tlp_hw_cnt);
    }
    return bytes;
}

int xpcie_dma_wr_done(void) {
    return (xpcie_read_reg(REG_DDMACR) & DDMACR_WR_DONE);
}

module_init(xpcie_init);
module_exit(xpcie_exit);

// Alias for udev

MODULE_AUTHOR("John Kelley");
MODULE_LICENSE("Dual BSD/GPL");

