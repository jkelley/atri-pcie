/*
 * Read the ATRI PCI device and spit out the results
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#define DEVNAME "/dev/atri-pcie"
#define EVTSIZE 512000

int main(int argc, char **argv) {
    int f;
    unsigned char *evtbuf;
    int cnt;

    printf("ATRI PCIe read tester\n");
    
    // Allocate memory for the event buffer
    evtbuf = (unsigned char *)malloc(EVTSIZE);
    if (evtbuf == NULL) {
        printf("Error: couldn't allocate event memory.\n");
        return -1;
    }

    // Open the device file
    f = open(DEVNAME, O_RDONLY);
    if (f < 0) {
        printf("ERROR: couldn't open device %s\n", DEVNAME);
        return -1;
    }

    printf("Trying to read up to %d bytes\n", EVTSIZE);
    cnt = read(f, evtbuf, EVTSIZE);
    printf("Got %d bytes\n", cnt);
    
    sleep(10);
    
    close(f);
        
    return 0;
}
