/*
 * Asymmetric Event Buffer for PCIe link
 * for DMA from device to CPU
 * 
 * John Kelley
 * jkelley@icecube.wisc.edu
 */

#ifndef __ATRI_EVT_QUEUE__
#define __ATRI_EVT_QUEUE__

#define NEVTQ_BITS  5
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
    spinlock_t lock;
    int dma_started; // protect by lock
} evtq;

inline evtbuf *evtq_getevent(evtq *q, unsigned i) { return &(q->evt[i&EVTQMASK]); }
inline int evtq_entries(evtq *q) { return q->wr_idx - q->rd_idx; }
inline int evtq_isfull(evtq *q)  { return NEVT == evtq_entries(q); }
inline int evtq_isempty(evtq *q) { return q->wr_idx == q->rd_idx; }
inline void empty_evtq(evtq *q) { q->wr_idx = q->rd_idx = 0; }
    
/* 
 * delete_evtq: clean up all memory allocated for the event queue. 
 */
void delete_evtq(evtq *q) {
    int i;    
    if (q == NULL)
        return;
    
    for (i = 0; i < NEVT; i++) {
        evtbuf *eb = evtq_getevent(q, i);
        if (eb->buf != NULL)
            pci_free_consistent(q->dev, EVTBUFSIZE, eb->buf, eb->physaddr);
    }
    kfree(q);
    q = NULL;
}

/*
 * Initialize the event queue.  Allocate memory for the event and map the 
 * DMA addresses.
 */
evtq *new_evtq(struct pci_dev *dev) {
    int i;
    int failed = 0;
    evtq *q;
    
    // Allocate the queue itself
    printk(KERN_INFO"new_evtq: allocating queue %d bytes\n", (int)sizeof(evtq));
    q = (evtq *) kmalloc(sizeof(evtq), GFP_KERNEL);
    if (q == NULL)
        return NULL;

    // Allocate all of the events (DMA buffers)
    printk(KERN_INFO"new_evtq: allocating events\n");    
    for (i = 0; i < NEVT; i++) {
        evtbuf *eb = evtq_getevent(q, i);
        eb->buf = pci_alloc_consistent(dev, EVTBUFSIZE, &eb->physaddr);
        failed |= (eb->buf == NULL);
        eb->len = 0;
    }
    if (failed) {
        printk(KERN_WARNING"new_evtq: allocations failed!\n");             
        delete_evtq(q);
        return NULL;
    }
    
    q->dev = dev;           
    empty_evtq(q);
    init_waitqueue_head(&q->wr_waitq);
    init_waitqueue_head(&q->rd_waitq);
    spin_lock_init(&q->lock);
    q->dma_started = 0;
    return q;
}


#endif
