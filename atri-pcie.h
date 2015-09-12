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

// Register definitions
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

// Register commands
#define DCSR_RESET   0x1
#define DCSR_ACTIVE  0x0

//Status flags indicating if resource was acquired
#define HAVE_REGION 0x01                    // I/O Memory region
#define HAVE_IRQ    0x02                    // Interupt
#define HAVE_KREG   0x04                    // Kernel registration

// IOCTRL commands
enum {
  INITCARD,
  INITRST,
  DISPREGS,
  RDDCSR,
  RDDDMACR,
  RDWDMATLPA,
  RDWDMATLPS,
  RDWDMATLPC,
  RDWDMATLPP,
  RDRDMATLPP,
  RDRDMATLPA,
  RDRDMATLPS,
  RDRDMATLPC,
  RDWDMAPERF,
  RDRDMAPERF,
  RDRDMASTAT,
  RDNRDCOMP,
  RDRCOMPDSIZE,
  RDDLWSTAT,
  RDDLTRSSTAT,
  RDDMISCCONT,
  RDDMISCONT,
  RDDLNKC,
  DFCCTL,
  DFCPINFO,
  DFCNPINFO,
  DFCINFO,

  // RDCFGREG,
  // WRCFGREG,
  // RDBMDREG,
  // WRBMDREG,

  WRDDMACR,
  WRWDMATLPS,
  WRWDMATLPC,
  WRWDMATLPP,
  WRRDMATLPS,
  WRRDMATLPC,
  WRRDMATLPP,
  WRDMISCCONT,
  WRDDLNKC,
  
  NUMCOMMANDS
};
