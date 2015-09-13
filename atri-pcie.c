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

// Device semaphore
// FIX ME this is not really used correctly at the moment
DEFINE_SEMAPHORE(gSem);

// Queue of DMA buffers for event transfer
evtq           *gEvtQ;

//-----------------------------------------------------------------------------
// Prototypes
//-----------------------------------------------------------------------------

irq_handler_t XPCIe_IRQHandler (int irq, void *dev_id, struct pt_regs *regs);
u32   XPCIe_ReadReg (u32 dw_offset);
void  XPCIe_WriteReg (u32 dw_offset, u32 val);
void  XPCIe_InitCard (void);
void  XPCIe_InitiatorReset (void);
void dma_wr_setup(struct work_struct *work);

// Work queue for DMA setup
static struct workqueue_struct *dma_setup_wq;
static DECLARE_WORK(dma_work, dma_wr_setup);

//-----------------------------------------------------------------------------
// Called with device is opened
int XPCIe_Open(struct inode *inode, struct file *filp) {

    // FIX ME: does this really need a lock?  
    if (down_interruptible(&gSem))
        return -ERESTARTSYS;

    gEvtQ = new_evtq(gDev);
    if (gEvtQ == NULL) {
        printk(KERN_ALERT"%s: Open: couldn't create event queue\n",gDrvrName);
        up(&gSem);
        return -ENOMEM;
    }

    // Set up the first DMA transfer
    queue_work(dma_setup_wq, &dma_work);
    
    up(&gSem);
    printk(KERN_INFO"%s: Open: module opened\n",gDrvrName);    
    return SUCCESS;
}

// Called when device is released
int XPCIe_Release(struct inode *inode, struct file *filp)
{
    // FIX ME: cannot really support more than one reader!
    down(&gSem);
    delete_evtq(gEvtQ);
    up(&gSem);
    printk(KERN_INFO"%s: Release: module released\n",gDrvrName);
    return SUCCESS;
}

ssize_t XPCIe_Read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {

    evtbuf *eb;
    if (down_interruptible(&gSem))
        return -ERESTARTSYS;
    
    // Check if event queue is empty 
    while (evtq_isempty(gEvtQ)) {
        up(&gSem); 

        // If we're non blocking, return
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        // Otherwise, wait until there is something there
        if (wait_event_interruptible(gEvtQ->rd_waitq, !evtq_isempty(gEvtQ)))
            return -ERESTARTSYS; /* signal caught */

        /* Loop, but first reacquire the lock */
        if (down_interruptible(&gSem))
            return -ERESTARTSYS;
    }

    // Make sure buffer is large enough
    eb = evtq_getevent(gEvtQ, gEvtQ->rd_idx);
    if (eb->len > count) {
        up(&gSem);
        return -ENOMEM; // Is this OK?
    }
    if (copy_to_user(buf, eb->buf, eb->len)) {
        up(&gSem);
        return -EFAULT;
    }
    
    // Once event has been read, increment the read pointer.
    // Wake up any sleeping write preparation.
    gEvtQ->rd_idx++;
    up(&gSem);
    wake_up_interruptible(&gEvtQ->wr_waitq);
    
    printk(KERN_INFO"%s: XPCIe_Read: %d bytes have been read...\n", gDrvrName, (int)eb->len);
    return eb->len;
}

// Aliasing write, read, ioctl, etc...
struct file_operations XPCIe_Intf = {
    read:           XPCIe_Read,
    // write:          XPCIe_Write_Orig,
    // unlocked_ioctl: XPCIe_Ioctl,
    open:           XPCIe_Open,
    release:        XPCIe_Release,
};

static int XPCIe_init(void)
{
  // Find the Xilinx PCIE device
  gDev = pci_get_device(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_XILINX_PCIE, gDev);
  if (NULL == gDev) {
    printk(KERN_WARNING"%s: Init: Hardware not found.\n", gDrvrName);
    return (CRIT_ERR);
  }
  else
    pci_dev_put(gDev);
  
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
  printk(KERN_INFO"%s: Init: Device IRQ: %X\n",gDrvrName, gIrq);

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
 
  // Request IRQ from OS.
  printk(KERN_INFO"%s: IRQ Setup..\n", gDrvrName);
  // Try to get an MSI interrupt
  if (0 > pci_enable_msi(gDev))
    printk(KERN_WARNING"%s: Init: Unable to enable MSI",gDrvrName);    
  
  if (0 > request_irq(gIrq, (irq_handler_t) XPCIe_IRQHandler, IRQF_SHARED, gDrvrName, gDev)) {
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

  //--- END: Initialize Hardware

  //--- START: Register Driver

  // Register with the kernel as a character device.
  if (0 > register_chrdev(gDrvrMajor, gDrvrName, &XPCIe_Intf)) {
    printk(KERN_WARNING"%s: Init: will not register\n", gDrvrName);
    return (CRIT_ERR);
  }
  printk(KERN_INFO"%s: Init: module registered\n", gDrvrName);
  gStatFlags = gStatFlags | HAVE_KREG;

  //--- END: Register Driver

  // Create DMA workqueue
  dma_setup_wq = create_singlethread_workqueue("atri-pcie-dma-work");
  gStatFlags = gStatFlags | HAVE_WQ;
  
  printk(KERN_ALERT"%s driver is loaded\n", gDrvrName);

  // Initializing card registers
  XPCIe_InitCard();

  return 0;
}

//--- XPCIe_InitiatorReset(): Resets the Xilinx reference design
void XPCIe_InitiatorReset() {
  // Reset device and then make it active
  XPCIe_WriteReg(REG_DCSR, DCSR_RESET);
  XPCIe_WriteReg(REG_DCSR, DCSR_ACTIVE);
}

//--- XPCIe_InitCard(): Initializes XBMD descriptor registers to default values
void XPCIe_InitCard() {

  XPCIe_InitiatorReset();

  /*
  XPCIe_WriteReg(2, gWriteHWAddr);        // Write: Write DMA TLP Address register with starting address
  XPCIe_WriteReg(3, 0x20);                // Write: Write DMA TLP Size register with default value (32dwords)
  XPCIe_WriteReg(4, 0x2000);              // Write: Write DMA TLP Count register with default value (2000)
  XPCIe_WriteReg(5, 0x00000000);          // Write: Write DMA TLP Pattern register with default value (0x0)

  XPCIe_WriteReg(6, 0xfeedbeef);          // Write: Read DMA Expected Data Pattern with default value (feedbeef)
  XPCIe_WriteReg(7, gReadHWAddr);         // Write: Read DMA TLP Address register with starting address.
  XPCIe_WriteReg(8, 0x20);                // Write: Read DMA TLP Size register with default value (32dwords)
  XPCIe_WriteReg(9, 0x2000);              // Write: Read DMA TLP Count register with default value (2000)
  */
}

//--- XPCIe_exit(): Performs any cleanup required before releasing the device
static void XPCIe_exit(void) {

    // Flush the DMA workqueue and destroy it
    printk(KERN_DEBUG"%s: destroy workqueue\n", gDrvrName);
    if (gStatFlags & HAVE_WQ) {
        flush_workqueue(dma_setup_wq);
        destroy_workqueue(dma_setup_wq);
    }
    
    printk(KERN_DEBUG"%s: Release memory\n",gDrvrName);
    // Check if we have a memory region and free it
    if (gStatFlags & HAVE_REGION)
        (void) release_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE);
    
    // Check if we have an IRQ and free it
    printk(KERN_DEBUG"%s: Free IRQ\n",gDrvrName);  
    pci_disable_msi(gDev);
    if (gStatFlags & HAVE_IRQ) {
        (void) free_irq(gIrq, gDev);
    }
    
    // Free up memory pointed to by virtual address
    printk(KERN_DEBUG"%s: unmap memory\n",gDrvrName);  
    if (gBaseVirt != NULL)
        iounmap(gBaseVirt);
    
    //gBaseVirt = NULL;
    
    // Unregister Device Driver
    printk(KERN_DEBUG"%s: unregister driver\n",gDrvrName);    
    if (gStatFlags & HAVE_KREG) {
        unregister_chrdev(gDrvrMajor, gDrvrName);
    }  
    gStatFlags = 0;
    
    printk(KERN_ALERT"%s driver is unloaded\n", gDrvrName);
}

irq_handler_t XPCIe_IRQHandler(int irq, void *dev_id, struct pt_regs *regs) {
    evtbuf *eb;
    printk(KERN_DEBUG"%s: Interrupt Handler Start ..",gDrvrName);
    
    // Unmap the DMA address
    eb = evtq_getevent(gEvtQ, gEvtQ->wr_idx);
    pci_unmap_single(gDev, eb->physaddr, eb->len, PCI_DMA_FROMDEVICE);
    
    // Data is now ready for processer. Increment the write pointer
    // and wake up and waiting reads
    gEvtQ->wr_idx++;
    wake_up_interruptible(&gEvtQ->rd_waitq);
   
    // Put the setup for the next write into a workqueue.
    // It can sleep so cannot be done here
    queue_work(dma_setup_wq, &dma_work);
    
    printk(KERN_DEBUG"%s Interrupt Handler End ..\n", gDrvrName);
    return (irq_handler_t) IRQ_HANDLED;
}

void dma_wr_setup(struct work_struct *work) {
    evtbuf *eb;
    // If the queue is full, wait until it is not
    printk(KERN_INFO"%s: DMA write setup\n", gDrvrName);
    while (evtq_isfull(gEvtQ)) {
        if (wait_event_interruptible(gEvtQ->wr_waitq, !evtq_isfull(gEvtQ)))
            continue;
    }
    
    // Map the DMA buffer
    eb = evtq_getevent(gEvtQ, gEvtQ->wr_idx);
    eb->physaddr = pci_map_single(gDev, eb->buf, eb->len, PCI_DMA_FROMDEVICE);
    if (eb->physaddr == 0)  {
        printk(KERN_ALERT"%s: Write Setup: Map error.\n", gDrvrName);
        return;
    }
    
    // Write the DMA address to the device
    XPCIe_WriteReg(REG_RDMATLPA, eb->physaddr);
    
    // Tell the device to start DMA
    XPCIe_WriteReg(REG_DDMACR, DDMACR_START);
}

u32 XPCIe_ReadReg(u32 dw_offset) {
    u32 ret = 0;
    printk(KERN_INFO"%s Read Register %d\n", gDrvrName, dw_offset);
    ret = readl(gBaseVirt + (4 * dw_offset));
    return ret; 
}

void XPCIe_WriteReg(u32 dw_offset, u32 val) {
	printk(KERN_INFO"%s Write Register %d Value %x\n", gDrvrName,
	       dw_offset, val);  
    writel(val, (gBaseVirt + (4 * dw_offset)));
}

// Driver Entry Point
module_init(XPCIe_init);

// Driver Exit Point
module_exit(XPCIe_exit);

