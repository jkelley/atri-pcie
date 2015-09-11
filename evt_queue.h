/*
 * Asymmetric Event Buffer for PCIe link
 * intended for DMA from device to CPU
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
    unsigned char buf[EVTBUFSIZE];
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

inline int evtq_entries(evtq *q) { return q->wr_idx - q->rd_idx; }
inline int evtq_isfull(evtq *q)  { return NEVT == evtq_entries(q); }
inline int evtq_isempty(evtq *q) { return q->wr_idx == q->rd_idx; }
inline void empty_evtq(evtq *q) { q->wr_idx = q->rd_idx = 0; }

/*
 * Initialize the event queue.  Allocate memory for the event and map the 
 * DMA addresses.
 */
evtq *new_evtq(struct pci_dev *dev) {
    int i;    
    evtq *q = (evtq *) kmalloc(sizeof(evtq), GFP_KERNEL | GFP_DMA);
    if (q != NULL) {
        for (i = 0; i < NEVT; i++) {
            evtbuf *eb = &(q->evt[i]);
            // Don't map the physical address yet - shared resource
            eb->physaddr = (dma_addr_t)NULL;
            //eb->physaddr = pci_map_single(dev, eb->buf, EVTBUFSIZE, PCI_DMA_FROMDEVICE);
            eb->len = EVTBUFSIZE;
        }
        q->dev = dev;           
        empty_evtq(q);
        init_waitqueue_head(&q->wr_waitq);
        init_waitqueue_head(&q->rd_waitq);
    }
    return q;
}

void delete_evtq(evtq *q) {
    if (q != NULL)
        kfree(q);       
}

#endif
