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
#include <asm/uaccess.h>

#include "atri-pcie.h"

// Xilinx PCI firmware vendor ID
#define PCI_VENDOR_ID_XILINX      0x10ee

// Xilinx PCI firmware device ID
#define PCI_DEVICE_ID_XILINX_PCIE 0x0007

// Registers on the firmware side (8 dwords)
#define PCIE_REGISTER_SIZE        (4*8)

#define REG_DCSR      0  // Device Control Status Register
#define REG_DMACR     1  // Write DMA Control Status Register
#define REG_WDMATLPA  2  // Write DMA TLP Address Register
#define REG_WDMATLPS  3  // Write DMA TLP Size Register
#define REG_WDMATLPC  4  // Write DMA TLP Count Register
#define REG_WDMATLPP  5  // Write DMA TLP Test Pattern Register
#define REG_RDMATLPP  6  // Read DMA TLP Expected Pattern
#define REG_RDMATLPA  7  // Read DMA TLP Address Register
#define REG_RDMATLPS  8  // Read DMA TLP Size Register
#define REG_RDMATLPC  9  // Read DMA TLP Count Register
#define REG_RDMASTAT 12  // Read DMA Status Register

//Status Flags: 
//       1 = Resouce successfully acquired
//       0 = Resource not acquired.      
#define HAVE_REGION 0x01                    // I/O Memory region
#define HAVE_IRQ    0x02                    // Interupt
#define HAVE_KREG   0x04                    // Kernel registration

int             gDrvrMajor = 241;           // Major number not dynamic.
unsigned int    gStatFlags = 0x00;          // Status flags used for cleanup.
unsigned long   gBaseHdwr;                  // Base register address (Hardware address)
unsigned long   gBaseLen;                   // Base register address Length
void           *gBaseVirt = NULL;           // Base register address (Virtual address, for I/O).
char            gDrvrName[]= "atri-pcie";   // Name of driver in proc.
struct pci_dev *gDev = NULL;                // PCI device structure.
int             gIrq;                       // IRQ assigned by PCI system.
char           *gBufferUnaligned = NULL;    // Pointer to Unaligned DMA buffer.
char           *gReadBuffer      = NULL;    // Pointer to dword aligned DMA buffer.
char           *gWriteBuffer     = NULL;    // Pointer to dword aligned DMA buffer.
dma_addr_t      gReadHWAddr;
dma_addr_t      gWriteHWAddr;

//-----------------------------------------------------------------------------
// Prototypes
//-----------------------------------------------------------------------------

irq_handler_t XPCIe_IRQHandler (int irq, void *dev_id, struct pt_regs *regs);
u32   XPCIe_ReadReg (u32 dw_offset);
void  XPCIe_WriteReg (u32 dw_offset, u32 val);
void  XPCIe_InitCard (void);
void  XPCIe_InitiatorReset (void);

// Called with device is opened
int XPCIe_Open(struct inode *inode, struct file *filp)
{
  printk(KERN_INFO"%s: Open: module opened\n",gDrvrName);
  return SUCCESS;
}

// Called when device is released
int XPCIe_Release(struct inode *inode, struct file *filp)
{
  printk(KERN_INFO"%s: Release: module released\n",gDrvrName);
  return SUCCESS;
}

// FIX ME 
ssize_t XPCIe_Write_Orig(struct file *filp, const char *buf, size_t count,
			 loff_t *f_pos)
{
  int ret = SUCCESS;
  memcpy((char *)gWriteBuffer, buf, count);
  printk(KERN_INFO"%s: XPCIe_Write_Orig: %d bytes have been written...\n", gDrvrName, (int)count);
  memcpy((char *)gReadBuffer, buf, count);
  printk(KERN_INFO"%s: XPCIe_Write_Orig: %d bytes have been written...\n", gDrvrName, (int)count);
  return (ret);
}

// FIX ME
ssize_t XPCIe_Read_Orig(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
  memcpy(buf, (char *)gWriteBuffer, count);
  printk(KERN_INFO"%s: XPCIe_Read_Orig: %d bytes have been read...\n", gDrvrName, (int)count);
  return (0);
}

// Aliasing write, read, ioctl, etc...
struct file_operations XPCIe_Intf = {
    read:           XPCIe_Read_Orig,
    write:          XPCIe_Write_Orig,
    //    unlocked_ioctl: XPCIe_Ioctl,
    open:           XPCIe_Open,
    release:        XPCIe_Release,
};


static int XPCIe_init(void)
{
  // Find the Xilinx EP device.  The device is found by matching device and vendor ID's which is defined
  // at the top of this file.  Be default, the driver will look for 10EE & 0007.  If the core is generated 
  // with other settings, the defines at the top must be changed or the driver will not load
  gDev = pci_get_device (PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_XILINX_PCIE, gDev);
  if (NULL == gDev) {

    // If a matching device or vendor ID is not found, return failure and update kernel log. 
    // NOTE: In fedora systems, the kernel log is located at: /var/log/messages
    printk(KERN_WARNING"%s: Init: Hardware not found.\n", gDrvrName);
    return (CRIT_ERR);
  }
  else
    pci_dev_put(gDev);

  // Get Base Address of registers from pci structure. Should come from pci_dev
  // structure, but that element seems to be missing on the development system.
  gBaseHdwr = pci_resource_start (gDev, 0);

  if (0 > gBaseHdwr) {
    printk(KERN_WARNING"%s: Init: Base Address not set.\n", gDrvrName);
    return (CRIT_ERR);
  } 

  // Print Base Address to kernel log
  printk(KERN_INFO"%s: Init: Base hw val %lx\n", gDrvrName, (unsigned long)gBaseHdwr);

  // Get the Base Address Length
  gBaseLen = pci_resource_len (gDev, 0);

  // Print the Base Address Length to Kernel Log
  printk(KERN_INFO"%s: Init: Base hw len %d\n", gDrvrName, (unsigned int)gBaseLen);

  // Remap the I/O register block so that it can be safely accessed.
  // I/O register block starts at gBaseHdwr and is 32 bytes long.
  // It is cast to char because that is the way Linus does it.
  // Reference "/usr/src/Linux-2.4/Documentation/IO-mapping.txt".
  gBaseVirt = ioremap(gBaseHdwr, gBaseLen);
  if (!gBaseVirt) {
    printk(KERN_WARNING"%s: Init: Could not remap memory.\n", gDrvrName);
    return (CRIT_ERR);
  } 

  // Print out the aquired virtual base addresss
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
  printk(KERN_INFO"%s: ISR Setup..\n", gDrvrName);
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

  //--- START: Allocate Buffers

  // Allocate the read buffer with size BUF_SIZE and return the starting address
  gReadBuffer = pci_alloc_consistent(gDev, BUF_SIZE, &gReadHWAddr);
  if (NULL == gReadBuffer) {
    printk(KERN_CRIT"%s: Init: Unable to allocate gBuffer.\n",gDrvrName);
    return (CRIT_ERR);
  }
  // Print Read buffer size and address to kernel log
  printk(KERN_INFO"%s: Read Buffer Allocation: %lX->%lX\n", gDrvrName, (unsigned long)gReadBuffer,
	 (unsigned long)gReadHWAddr);

  // Allocate the write buffer with size BUF_SIZE and return the starting address
  gWriteBuffer = pci_alloc_consistent(gDev, BUF_SIZE, &gWriteHWAddr);
  if (NULL == gWriteBuffer) {
    printk(KERN_CRIT"%s: Init: Unable to allocate gBuffer.\n",gDrvrName);
    return (CRIT_ERR);
  }
  // Print Write buffer size and address to kernel log  
  printk(KERN_INFO"%s: Write Buffer Allocation: %lX->%lX\n", gDrvrName, (unsigned long)gWriteBuffer,
	 (unsigned long)gWriteHWAddr);

  //--- END: Allocate Buffers

  //--- START: Register Driver

  // Register with the kernel as a character device.
  if (0 > register_chrdev(gDrvrMajor, gDrvrName, &XPCIe_Intf)) {
    printk(KERN_WARNING"%s: Init: will not register\n", gDrvrName);
    return (CRIT_ERR);
  }
  printk(KERN_INFO"%s: Init: module registered\n", gDrvrName);
  gStatFlags = gStatFlags | HAVE_KREG;

  //--- END: Register Driver

  // The driver is now successfully loaded.  All HW is initialized, IRQ's assigned, and buffers allocated
  printk("%s driver is loaded\n", gDrvrName);


  // Initializing card registers
  XPCIe_InitCard();

  return 0;
}

//--- XPCIe_InitiatorReset(): Resets the XBMD reference design
void XPCIe_InitiatorReset() {
  // Reset device
  XPCIe_WriteReg(REG_DCSR, 1);
  // Make device active
  XPCIe_WriteReg(REG_DCSR, 0);
}

//--- XPCIe_InitCard(): Initializes XBMD descriptor registers to default values
void XPCIe_InitCard() {

  XPCIe_InitiatorReset();

  XPCIe_WriteReg(2, gWriteHWAddr);        // Write: Write DMA TLP Address register with starting address
  XPCIe_WriteReg(3, 0x20);                // Write: Write DMA TLP Size register with default value (32dwords)
  XPCIe_WriteReg(4, 0x2000);              // Write: Write DMA TLP Count register with default value (2000)
  XPCIe_WriteReg(5, 0x00000000);          // Write: Write DMA TLP Pattern register with default value (0x0)

  XPCIe_WriteReg(6, 0xfeedbeef);          // Write: Read DMA Expected Data Pattern with default value (feedbeef)
  XPCIe_WriteReg(7, gReadHWAddr);         // Write: Read DMA TLP Address register with starting address.
  XPCIe_WriteReg(8, 0x20);                // Write: Read DMA TLP Size register with default value (32dwords)
  XPCIe_WriteReg(9, 0x2000);              // Write: Read DMA TLP Count register with default value (2000)
}

//--- XPCIe_exit(): Performs any cleanup required before releasing the device
static void XPCIe_exit(void) {

  printk(KERN_INFO"%s: Release memory",gDrvrName);
  // Check if we have a memory region and free it
  if (gStatFlags & HAVE_REGION)
     (void) release_mem_region(gBaseHdwr, PCIE_REGISTER_SIZE);

  // Check if we have an IRQ and free it
  printk(KERN_INFO"%s: Free IRQ",gDrvrName);  
  pci_disable_msi(gDev);
  if (gStatFlags & HAVE_IRQ) {
    (void) free_irq(gIrq, gDev);
  }
  
  printk(KERN_INFO"%s: Free consistent",gDrvrName);  
  // Free memory allocated to our Endpoint
  if (gReadBuffer != NULL)
    pci_free_consistent(gDev, BUF_SIZE, gReadBuffer, gReadHWAddr);
  if (gWriteBuffer != NULL)
    pci_free_consistent(gDev, BUF_SIZE, gWriteBuffer, gWriteHWAddr);

  //gReadBuffer = NULL;
  //gWriteBuffer = NULL;
  
  printk(KERN_INFO"%s: unmap memory",gDrvrName);  
  // Free up memory pointed to by virtual address
  if (gBaseVirt != NULL)
    iounmap(gBaseVirt);
  
  //gBaseVirt = NULL;

  printk(KERN_INFO"%s: unregister driver",gDrvrName);    
  // Unregister Device Driver
  if (gStatFlags & HAVE_KREG) {
    unregister_chrdev(gDrvrMajor, gDrvrName);
  }
  
  gStatFlags = 0;
  
  printk(KERN_ALERT"%s driver is unloaded\n", gDrvrName);
}

// Driver Entry Point
module_init(XPCIe_init);

// Driver Exit Point
module_exit(XPCIe_exit);

// FIX ME does nothing
irq_handler_t XPCIe_IRQHandler(int irq, void *dev_id, struct pt_regs *regs)
{
  u32 i, regx;

  printk(KERN_WARNING"%s: Interrupt Handler Start ..",gDrvrName);
  
  for (i = 0; i < 32; i++) {
     regx = XPCIe_ReadReg(i);
     printk(KERN_WARNING"%s : REG<%d> : 0x%X\n", gDrvrName, i, regx);
  }

  printk(KERN_WARNING"%s Interrupt Handler End ..\n", gDrvrName);
  return (irq_handler_t) IRQ_HANDLED;
}

u32 XPCIe_ReadReg (u32 dw_offset) {
        u32 ret = 0;
	printk(KERN_INFO"%s Read Register %d\n", gDrvrName, dw_offset);
        ret = readl(gBaseVirt + (4 * dw_offset));
        return ret; 
}

void XPCIe_WriteReg (u32 dw_offset, u32 val) {
	printk(KERN_INFO"%s Write Register %d Value %x\n", gDrvrName,
	       dw_offset, val);  
        writel(val, (gBaseVirt + (4 * dw_offset)));
}

