#include "vajra/hal.h"
#include "vajra/memory.h"

/* ------------------------------------------------------------------
 * Minimal legacy virtio-net-pci driver.
 *
 * Deliberately the same two-step shape as storage (Phase 8/10): this
 * is the raw HAL driver only -- send/receive a raw Ethernet frame,
 * nothing above it. No IP/UDP/TCP, no capability gating yet, not
 * reachable from actor code at all. A later milestone builds a real
 * transport and syscall surface on top of this, the same way
 * core/storage.c was built on hal_disk_read/write.
 *
 * Uses the LEGACY virtio interface (I/O-port BAR0, no FEATURES_OK
 * step) rather than the modern virtio 1.0 capability-list/MMIO
 * interface -- QEMU's virtio-net-pci is "transitional" by default and
 * supports both; legacy is the simpler register layout and is what
 * this driver speaks throughout. Negotiates ZERO optional features
 * (no checksum offload, no GSO, no mergeable RX buffers), which keeps
 * every virtio_net_hdr a fixed 10 bytes and every buffer a single,
 * ungrouped descriptor -- the simplest correct configuration, not a
 * shortcut: a real driver wanting performance would negotiate more,
 * but correctness doesn't require it.
 * ---------------------------------------------------------------- */

#define VIRTIO_VENDOR_ID          0x1AF4
#define VIRTIO_NET_DEVICE_ID_LEGACY 0x1000

#define VIRTIO_REG_HOST_FEATURES  0x00
#define VIRTIO_REG_GUEST_FEATURES 0x04
#define VIRTIO_REG_QUEUE_PFN      0x08
#define VIRTIO_REG_QUEUE_NUM      0x0C
#define VIRTIO_REG_QUEUE_SEL      0x0E
#define VIRTIO_REG_QUEUE_NOTIFY   0x10
#define VIRTIO_REG_STATUS         0x12
#define VIRTIO_REG_ISR            0x13
#define VIRTIO_REG_CONFIG         0x14 /* +4 more if has_msix -- see pci.c */

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FAILED      0x80

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_QUEUE_RX 0
#define VIRTIO_QUEUE_TX 1

#define NET_HDR_LEN 10  /* legacy virtio_net_hdr, no mergeable-rx-buffers feature negotiated */
#define RX_BUF_COUNT 8  /* how many of the RX ring's descriptors we actually keep posted */
#define FRAME_BUF_SIZE 2048 /* comfortably covers NET_HDR_LEN + a full 1514-byte Ethernet frame */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq {
    struct virtq_desc *desc;
    uint16_t *avail_idx;
    uint16_t *avail_ring;
    uint16_t *used_idx;
    uint8_t  *used_ring_bytes; /* struct virtq_used_elem { u32 id; u32 len; }, indexed manually */
    uint16_t qsize;
    uint16_t last_used_idx;
    uint16_t next_avail_idx;
};

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static struct hal_pci_device dev;
static uint16_t io_base;
static uint16_t config_off;
static struct virtq rx_q;
static struct virtq tx_q;
static uint8_t *rx_bufs[RX_BUF_COUNT];
static uint8_t *tx_buf;
static int net_ready = 0;

static uint32_t virtq_used_elem_id(struct virtq *vq, uint16_t slot) {
    uint8_t *p = vq->used_ring_bytes + (uint32_t)slot * 8;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t virtq_used_elem_len(struct virtq *vq, uint16_t slot) {
    uint8_t *p = vq->used_ring_bytes + (uint32_t)slot * 8 + 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Lays out one virtqueue's descriptor table + available ring + used
 * ring in one physically contiguous region (required -- the device
 * addresses the whole thing via a single PFN written to QUEUE_PFN;
 * see include/vajra/memory.h's own comment on why this needed a new
 * allocator primitive), sized generically from whatever QUEUE_NUM this
 * device actually reports rather than assuming 256. */
static int virtq_init(uint16_t queue_index, struct virtq *vq) {
    outw((uint16_t)(io_base + VIRTIO_REG_QUEUE_SEL), queue_index);
    uint16_t qsize = inw((uint16_t)(io_base + VIRTIO_REG_QUEUE_NUM));
    if (qsize == 0) {
        return -1;
    }

    uint32_t desc_bytes  = 16u * qsize;
    uint32_t avail_bytes = 4u + 2u * qsize;
    uint32_t used_offset = (desc_bytes + avail_bytes + 4095u) & ~4095u;
    uint32_t used_bytes  = 4u + 8u * qsize;
    uint32_t total       = used_offset + used_bytes;
    int pages = (int)((total + 4095u) / 4096u);

    void *mem = alloc_pages_contig(pages);
    if (!mem) {
        return -1;
    }

    uint8_t *base = (uint8_t *)mem;
    vq->desc             = (struct virtq_desc *)base;
    vq->avail_idx        = (uint16_t *)(base + desc_bytes + 2);
    vq->avail_ring       = (uint16_t *)(base + desc_bytes + 4);
    vq->used_idx         = (uint16_t *)(base + used_offset + 2);
    vq->used_ring_bytes  = base + used_offset + 4;
    vq->qsize            = qsize;
    vq->last_used_idx    = 0;
    vq->next_avail_idx   = 0;

    uint32_t pfn = (uint32_t)((uint64_t)mem / 4096u);
    outl((uint16_t)(io_base + VIRTIO_REG_QUEUE_PFN), pfn);
    return 0;
}

/* Publishes descriptor `desc_index` as available to the device (RX: a
 * writable empty buffer; TX: a readable frame) and notifies. */
static void virtq_publish(struct virtq *vq, uint16_t queue_index, uint16_t desc_index) {
    vq->avail_ring[vq->next_avail_idx % vq->qsize] = desc_index;
    __asm__ __volatile__("" ::: "memory"); /* ring entry visible before the index bump */
    vq->next_avail_idx++;
    *vq->avail_idx = vq->next_avail_idx;
    __asm__ __volatile__("" ::: "memory"); /* index visible before the notify */
    outw((uint16_t)(io_base + VIRTIO_REG_QUEUE_NOTIFY), queue_index);
}

static void rx_post(uint16_t desc_index) {
    rx_q.desc[desc_index].addr  = (uint64_t)(uintptr_t)rx_bufs[desc_index];
    rx_q.desc[desc_index].len   = FRAME_BUF_SIZE;
    rx_q.desc[desc_index].flags = VIRTQ_DESC_F_WRITE;
    rx_q.desc[desc_index].next  = 0;
    virtq_publish(&rx_q, VIRTIO_QUEUE_RX, desc_index);
}

int hal_net_init(void) {
    if (hal_pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID_LEGACY, &dev) != 0) {
        return -1;
    }
    io_base = dev.io_base;
    config_off = (uint16_t)(VIRTIO_REG_CONFIG + (dev.has_msix ? 4 : 0));

    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), 0); /* reset */
    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate nothing beyond the baseline -- see this file's own
     * top comment for why zero optional features is a deliberate,
     * sufficient choice here, not an oversight. */
    (void)inl((uint16_t)(io_base + VIRTIO_REG_HOST_FEATURES));
    outl((uint16_t)(io_base + VIRTIO_REG_GUEST_FEATURES), 0);

    if (virtq_init(VIRTIO_QUEUE_RX, &rx_q) != 0) {
        outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (virtq_init(VIRTIO_QUEUE_TX, &tx_q) != 0) {
        outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_FAILED);
        return -1;
    }

    for (int i = 0; i < RX_BUF_COUNT && i < rx_q.qsize; i++) {
        rx_bufs[i] = (uint8_t *)alloc_page();
        if (!rx_bufs[i]) {
            outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_FAILED);
            return -1;
        }
        rx_post((uint16_t)i);
    }

    tx_buf = (uint8_t *)alloc_page();
    if (!tx_buf) {
        outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_FAILED);
        return -1;
    }

    outb((uint16_t)(io_base + VIRTIO_REG_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    net_ready = 1;
    return 0;
}

void hal_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = inb((uint16_t)(io_base + config_off + i));
    }
}

int hal_net_send(const void *frame, uint32_t len) {
    if (!net_ready || NET_HDR_LEN + len > FRAME_BUF_SIZE) {
        return -1;
    }

    for (int i = 0; i < NET_HDR_LEN; i++) {
        tx_buf[i] = 0; /* an all-zero virtio_net_hdr: no offload, no GSO -- see top comment */
    }
    for (uint32_t i = 0; i < len; i++) {
        tx_buf[NET_HDR_LEN + i] = ((const uint8_t *)frame)[i];
    }

    uint16_t desc_index = 0; /* single, reused TX descriptor -- see this function's own comment below */
    tx_q.desc[desc_index].addr  = (uint64_t)(uintptr_t)tx_buf;
    tx_q.desc[desc_index].len   = NET_HDR_LEN + len;
    tx_q.desc[desc_index].flags = 0; /* device-readable */
    tx_q.desc[desc_index].next  = 0;
    virtq_publish(&tx_q, VIRTIO_QUEUE_TX, desc_index);

    /* Wait (bounded) for the device to report it consumed the
     * descriptor, so the next send can safely reuse tx_buf -- this
     * driver only ever has one TX descriptor in flight at a time,
     * deliberately, matching its "one raw frame at a time" scope. */
    for (uint32_t spins = 0; spins < 1000000; spins++) {
        if (*tx_q.used_idx != tx_q.last_used_idx) {
            tx_q.last_used_idx = *tx_q.used_idx;
            break;
        }
    }

    return 0;
}

int hal_net_poll_receive(void *buf, uint32_t max_len, uint32_t max_spins) {
    if (!net_ready) {
        return -1;
    }

    for (uint32_t spins = 0; spins < max_spins; spins++) {
        if (*rx_q.used_idx != rx_q.last_used_idx) {
            uint16_t slot = rx_q.last_used_idx % rx_q.qsize;
            uint32_t desc_index = virtq_used_elem_id(&rx_q, slot);
            uint32_t total_len  = virtq_used_elem_len(&rx_q, slot);
            rx_q.last_used_idx++;

            if (total_len < NET_HDR_LEN) {
                rx_post((uint16_t)desc_index); /* malformed -- re-arm and keep going */
                continue;
            }
            uint32_t frame_len = total_len - NET_HDR_LEN;
            int rc;
            if (frame_len > max_len) {
                rc = -1;
            } else {
                uint8_t *src = rx_bufs[desc_index] + NET_HDR_LEN;
                for (uint32_t i = 0; i < frame_len; i++) {
                    ((uint8_t *)buf)[i] = src[i];
                }
                rc = (int)frame_len;
            }

            rx_post((uint16_t)desc_index); /* give the buffer back to the device */
            return rc;
        }
    }

    return 0; /* nothing arrived within max_spins -- not an error */
}
