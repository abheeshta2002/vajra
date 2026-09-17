#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Minimal PCI config-space access -- the "configuration mechanism #1"
 * every x86 chipset since the original PCI spec supports: two 32-bit
 * I/O ports (0xCF8 selects bus/slot/function/register, 0xCFC is the
 * data window onto whatever was just selected). This is the first
 * time this kernel has needed to find a device rather than being
 * handed a fixed, well-known port range (ATA's 0x1F0, the LAPIC's
 * fixed MMIO address) -- virtio-net-pci's own I/O port base isn't
 * fixed; it's whatever BAR0 the chipset assigned it, discovered here.
 *
 * Deliberately scoped to bus 0 only, function 0 only -- true for
 * every device on QEMU's default `pc`/`q35` machine topology this
 * kernel targets, and a real limitation for anything behind a PCI-to-
 * PCI bridge or a multi-function card. Generalizing to a full
 * recursive bus scan is future work, not needed by anything that
 * exists yet.
 * ---------------------------------------------------------------- */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR_DEVICE 0x00
#define PCI_REG_COMMAND       0x04
#define PCI_REG_STATUS        0x04 /* high 16 bits of the same dword as COMMAND */
#define PCI_REG_BAR0          0x10
#define PCI_REG_CAP_PTR       0x34

#define PCI_COMMAND_IO_SPACE   0x1
#define PCI_COMMAND_BUS_MASTER 0x4
#define PCI_STATUS_CAP_LIST    (1u << 4) /* bit 4 of the status word, i.e. bit 20 of the dword */

#define PCI_CAP_ID_MSIX 0x11

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t pci_config_read32(uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

/* Walks the PCI capability linked list (if the device has one at all
 * -- STATUS bit 4 says so) looking for one matching cap_id. Needed
 * because legacy virtio-pci's device-specific config space (where a
 * net device's MAC address lives) starts 4 bytes later than usual if
 * an MSI-X capability is present -- true for QEMU's virtio-net-pci by
 * default. Getting this wrong doesn't fail loudly, it just reads the
 * MAC address 4 bytes off, which is exactly the kind of silent-wrong
 * bug worth handling properly instead of hardcoding one offset and
 * hoping. */
static int pci_has_capability(uint8_t slot, uint8_t cap_id) {
    uint32_t status_cmd = pci_config_read32(slot, 0, PCI_REG_STATUS);
    if (!((status_cmd >> 16) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }

    uint8_t ptr = (uint8_t)(pci_config_read32(slot, 0, PCI_REG_CAP_PTR) & 0xFF);
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint32_t cap = pci_config_read32(slot, 0, ptr);
        if ((cap & 0xFF) == cap_id) {
            return 1;
        }
        ptr = (uint8_t)((cap >> 8) & 0xFF);
    }
    return 0;
}

/* Scans bus 0, function 0 of every slot for a device matching
 * (vendor_id, device_id). Returns 0 and fills *out on success, -1 if
 * nothing matched. On success, also enables I/O space + bus mastering
 * on the found device -- every caller of this function needs both
 * (I/O space to reach BAR0 at all, bus mastering so the device can DMA
 * into memory this kernel gives it), so there's no reason to make
 * every caller repeat that. */
int hal_pci_find_device(uint16_t vendor_id, uint16_t device_id, struct hal_pci_device *out) {
    for (uint16_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_config_read32((uint8_t)slot, 0, PCI_REG_VENDOR_DEVICE);
        uint16_t vid = (uint16_t)(id & 0xFFFF);
        if (vid == 0xFFFF) {
            continue; /* no device in this slot */
        }
        uint16_t did = (uint16_t)((id >> 16) & 0xFFFF);
        if (vid != vendor_id || did != device_id) {
            continue;
        }

        uint32_t bar0 = pci_config_read32((uint8_t)slot, 0, PCI_REG_BAR0);
        if (!(bar0 & 0x1)) {
            continue; /* not an I/O-space BAR -- not what this driver expects */
        }

        uint32_t cmd = pci_config_read32((uint8_t)slot, 0, PCI_REG_COMMAND);
        cmd |= PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER;
        pci_config_write32((uint8_t)slot, 0, PCI_REG_COMMAND, cmd);

        out->slot = (uint8_t)slot;
        out->io_base = (uint16_t)(bar0 & ~0x3u);
        out->has_msix = (uint8_t)pci_has_capability((uint8_t)slot, PCI_CAP_ID_MSIX);
        return 0;
    }

    return -1;
}
