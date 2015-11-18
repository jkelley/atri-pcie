/*
 * Header file for ATRI PCIe link device driver
 * based on Xilinx XAPP1052 sample driver
 *
 * John Kelley
 * jkelley@icecube.wisc.edu
 */

#define SUCCESS                    0
#define CRIT_ERR                  -1

// Max DMA Buffer Size
#define BUF_SIZE                  (4096 * 1024)

// Xilinx PCI firmware vendor ID
#define PCI_VENDOR_ID_XILINX      0x10ee

// Xilinx PCI firmware device ID
#define PCI_DEVICE_ID_XILINX_PCIE 0x0007

// Registers on the firmware side (8 dwords)
#define PCIE_REGISTER_SIZE        (4*8)

// PCI DMA mask -- just use 32 bits
// device can actually support more
#define PCI_HW_DMA_MASK           0xffffffff

// Use MSI or normal shared interrupts?
// WARNING: legacy interrupt handling is broken still
#define PCI_USE_MSI               1

// Timer duration for lost interrupt (ms)
#define IRQ_TIMEOUT_MS            5000

// Xilinx XAPP1052 firmware test; driver sets up
// transfer itself using a test pattern
#define XILINX_TEST_MODE          0

// Register definitions (Xilinx)
#define REG_DCSR       0  // Device Control Status Register
#define REG_DDMACR     1  // Device DMA Control Register
#define REG_WDMATLPA   2  // Write DMA TLP Address Register
#define REG_WDMATLPS   3  // Write DMA TLP Size Register
#define REG_WDMATLPC   4  // Write DMA TLP Count Register
#define REG_WDMATLPP   5  // Write DMA TLP Test Pattern Register
#define REG_RDMATLPP   6  // Read DMA TLP Expected Pattern
#define REG_RDMATLPA   7  // Read DMA TLP Address Register
#define REG_RDMATLPS   8  // Read DMA TLP Size Register
#define REG_RDMATLPC   9  // Read DMA TLP Count Register
#define REG_RDMASTAT  12  // Read DMA Status Register
#define REG_DLTRSSTAT 16  // Device Link Transaction Size Status Register

// Registers redefined for ATRI firmware
#define REG_WDMATLPEX 5  // Write DMA TLP extra dword count (last TLP)

// Register bits
#define DCSR_RESET        1
#define DCSR_ACTIVE       0
#define DDMACR_WR_START   1
#define DDMACR_WR_INTDIS (1 << 7)
#define DDMACR_WR_DONE   (1 << 8)
#define DDMACR_RD_START  (1 << 16 )
#define DDMACR_RD_INTDIS (1 << 23 )
#define DDMACR_RD_DONE   (1 << 24 )

#define DMA_TLP_SIZE_MASK 0x1fff
#define DMA_TLP_CNT_MASK  0xffff

//Status flags indicating if resource was acquired by driver
#define HAVE_REGION 0x01                    // I/O Memory region
#define HAVE_IRQ    0x02                    // Interupt
#define HAVE_KREG   0x04                    // Kernel registration
#define HAVE_WQ     0x08                    // DMA work queue

// Ioctl commands
enum {
    XPCIE_IOCTL_INIT,
    XPCIE_IOCTL_FLUSH,
    XPCIE_IOCTL_NUMCOMMANDS
};

// Debug printk can be disabled
#undef PDEBUG
#ifdef ATRI_DEBUG
#define PDEBUG(fmt, args...) printk(KERN_DEBUG fmt, ## args)
#else
#define PDEBUG(fmt, args...) /* it's off jim */
#endif
