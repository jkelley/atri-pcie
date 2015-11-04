/*
 * Read the ATRI PCI device and spit out the results.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#define DEVNAME "/dev/atri-pcie"
#define MAXEVTSIZE 512000

/* Ioctl commands */
enum {
    XPCIE_IOCTL_INIT,
    XPCIE_IOCTL_FLUSH
};

int main(int argc, char **argv) {
    int f, i, j, cnt, nevts, nbytes;
    unsigned char *evtbuf;

    if (argc < 2) {
        printf("Usage: %s <# of events / chunks> [<chunk size>]\n", argv[0]);
        return 0;
    }

    nevts = atoi(argv[1]);
    printf("ATRI PCIe read tester: getting %d events\n", nevts);

    if (argc > 2) {
        nbytes = atoi(argv[2]);
        printf("Read chunk size: %d bytes\n", nbytes);
    }
    else {
        nbytes = MAXEVTSIZE;
    }
        
    // Allocate memory for the event buffer
    evtbuf = (unsigned char *)malloc(MAXEVTSIZE);
    if (evtbuf == NULL) {
        printf("Error: couldn't allocate event memory.\n");
        return -1;
    }

    // Open the device file
    f = open(DEVNAME, O_RDONLY);    
    if (f < 0) {
        printf("Error: couldn't open device %s\n", DEVNAME);
        return -1;
    }

    // Flush the queue of stale events
    ioctl(f, XPCIE_IOCTL_FLUSH);
    
    // Loop and read as many events as requested
    // Print out the first few bytes of each
    for (i = 0; i < nevts; i++) {
        cnt = read(f, evtbuf, nbytes);
        printf("Event %d: got %d bytes\n", i+1, cnt);

        for (j = 0; j < 8; j++)
            printf("%02x ", evtbuf[j]);
        printf("\n");

    }
    
    close(f);
    
    return 0;
}
