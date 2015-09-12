/*
 * Read the ATRI PCI device and spit out the results
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define DEVNAME "/dev/atri-pcie"

int main(int argc, char **argv) {

    int f = open(DEVNAME, O_RDONLY);
    if (f < 0) {
        printf("ERROR: couldn't open device %s\n", DEVNAME);
        return -1;
    }

    // Do some stuff
    
    close(f);
        
    return 0;
}
