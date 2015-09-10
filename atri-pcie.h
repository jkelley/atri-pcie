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
