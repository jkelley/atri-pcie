/*
 * Asymmetric Event Buffer for PCIe link
 * for DMA from device to CPU
 * 
 * John Kelley
 * jkelley@icecube.wisc.edu
 */

#ifndef __ATRI_EVT_QUEUE__
#define __ATRI_EVT_QUEUE__

#define NEVTQ_BITS  6
#define NEVT       (1 << NEVTQ_BITS)
#define EVTQMASK   (NEVT-1)

#define EVTBUFSIZE  512000

typedef struct {
    unsigned char *buf;
    dma_addr_t physaddr;
    size_t len;
} evtbuf;

typedef struct {
    evtbuf evt[NEVT];
    struct pci_dev *dev;
    unsigned rd_idx;
    unsigned wr_idx;
    wait_queue_head_t rd_waitq;
    wait_queue_head_t wr_waitq;
} evtq;

inline evtbuf *evtq_getentry(unsigned i) { return &(evt[i&EVTQMASK]); }
inline int evtq_entries(evtq *q) { return q->wr_idx - q->rd_idx; }
inline int evtq_isfull(evtq *q)  { return NEVT == evtq_entries(q); }
inline int evtq_isempty(evtq *q) { return q->wr_idx == q->rd_idx; }
inline void empty_evtq(evtq *q) { q->wr_idx = q->rd_idx = 0; }

/*
 * Read an event after a mapped DMA transfer in a userspace
 * buffer.  Can sleep.
 */
/*
int read_evt(evtq *q, char *buf) {
    if (!evtq_isempty(q)) {
    
    }
}
*/

/*
 * Clean up all memory allocated for the event queue.
 */
void delete_evtq(evtq *q) {
    int i;    
    if (q == NULL)
        return;
    
    for (i = 0; i < NEVT; i++) {
        evtbuf *eb = evtq_getentry(q, i);
        if (eb->buf != NULL)
            kfree(eb);
    }  
    kfree(q);       
}

/*
 * Initialize the event queue.  Allocate memory for the event and map the 
 * DMA addresses.
 */
evtq *new_evtq(struct pci_dev *dev) {
    int i;
    int failed = 0;
    
    // Allocate the queue itself
    evtq *q = (evtq *) kmalloc(sizeof(evtq), GFP_KERNEL);
    if (q == NULL) {
        delete_evtq(q);
        return NULL;
    }

    // Allocate all of the events (DMA buffers)
    for (i = 0; i < NEVT; i++) {
        evtbuf *eb = evtq_getentry(q, i);        
        eb->buf = kmalloc(EVTBUFSIZE, GFP_KERNEL | GFP_DMA);
        failed |= (eb->buf == NULL);
        
        // Don't map the physical address yet - shared resource
        eb->physaddr = (dma_addr_t)NULL;            
        //eb->physaddr = pci_map_single(dev, eb->buf, EVTBUFSIZE, PCI_DMA_FROMDEVICE);
        
        eb->len = EVTBUFSIZE;
    }
    if (failed) {
        delete_evtq(q);
        return NULL;
    }

    q->dev = dev;           
    empty_evtq(q);
    init_waitqueue_head(&q->wr_waitq);
    init_waitqueue_head(&q->rd_waitq);

    return q;
}


#endif