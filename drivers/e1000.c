/*
 * drivers/e1000.c - Intel 82540EM (e1000) Ethernet NIC driver
 *
 * Supports:
 *   - MMIO register access (BAR0)
 *   - MAC address from EEPROM
 *   - 32 RX descriptors with 2KB data buffers
 *   - 32 TX descriptors with 2KB data buffers
 *   - IRQ-driven receive (with polling fallback)
 *   - Simple transmit via write to TDT
 *
 * QEMU default NIC: Intel 82540EM (vendor=0x8086, device=0x100E)
 * Use: -netdev user,id=net0 -device e1000,netdev=net0
 */
#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <interrupts.h>
#include <memory.h>
#include <net/net.h>
#include <kernel.h>
#include <string.h>

/* =========================================================
 * e1000 MMIO register offsets
 * ========================================================= */
#define E1000_CTRL      0x0000   /* Device Control */
#define E1000_STATUS    0x0008   /* Device Status */
#define E1000_EECD      0x0010   /* EEPROM/Flash Control */
#define E1000_EERD      0x0014   /* EEPROM Read */
#define E1000_ICR       0x00C0   /* Interrupt Cause Read (clears on read) */
#define E1000_IMS       0x00D0   /* Interrupt Mask Set */
#define E1000_IMC       0x00D8   /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100   /* Receive Control */
#define E1000_TCTL      0x0400   /* Transmit Control */
#define E1000_TIPG      0x0410   /* TX IPG (inter-packet gap) */
#define E1000_RDBAL     0x2800   /* RX Descriptor Base Low */
#define E1000_RDBAH     0x2804   /* RX Descriptor Base High */
#define E1000_RDLEN     0x2808   /* RX Descriptor Ring Length (bytes) */
#define E1000_RDH       0x2810   /* RX Descriptor Head */
#define E1000_RDT       0x2818   /* RX Descriptor Tail */
#define E1000_TDBAL     0x3800   /* TX Descriptor Base Low */
#define E1000_TDBAH     0x3804   /* TX Descriptor Base High */
#define E1000_TDLEN     0x3808   /* TX Descriptor Ring Length */
#define E1000_TDH       0x3810   /* TX Descriptor Head */
#define E1000_TDT       0x3818   /* TX Descriptor Tail */
#define E1000_RAL0      0x5400   /* Receive Address Low 0 */
#define E1000_RAH0      0x5404   /* Receive Address High 0 */
#define E1000_MTA       0x5200   /* Multicast Table Array (128 DWORDs) */

/* CTRL register bits */
#define E1000_CTRL_SLU  (1 << 6)    /* Set Link Up */
#define E1000_CTRL_RST  (1 << 26)   /* Device Reset */

/* RCTL register bits */
#define E1000_RCTL_EN   (1 << 1)
#define E1000_RCTL_SBP  (1 << 2)
#define E1000_RCTL_UPE  (1 << 3)    /* Unicast Promisc. */
#define E1000_RCTL_MPE  (1 << 4)    /* Multicast Promisc. */
#define E1000_RCTL_BAM  (1 << 15)   /* Broadcast Accept */
#define E1000_RCTL_BSIZE_2048 0     /* 2KB buffers (default) */
#define E1000_RCTL_SECRC (1 << 26)  /* Strip Ethernet CRC */

/* TCTL register bits */
#define E1000_TCTL_EN   (1 << 1)
#define E1000_TCTL_PSP  (1 << 3)    /* Pad Short Packets */
#define E1000_TCTL_CT   (0x0F << 4)
#define E1000_TCTL_COLD (0x040 << 12)

/* TX descriptor CMD bits */
#define E1000_TXD_CMD_EOP  (1 << 0)  /* End of Packet */
#define E1000_TXD_CMD_IFCS (1 << 1)  /* Insert FCS */
#define E1000_TXD_CMD_RS   (1 << 3)  /* Report Status */

/* RX descriptor status bits */
#define E1000_RXD_STAT_DD  (1 << 0)  /* Descriptor Done */
#define E1000_RXD_STAT_EOP (1 << 1)  /* End of Packet */

/* Interrupt bits */
#define E1000_ICR_TXDW  (1 << 0)   /* TX Desc Written Back */
#define E1000_ICR_TXQE  (1 << 1)   /* TX Queue Empty */
#define E1000_ICR_LSC   (1 << 2)   /* Link Status Change */
#define E1000_ICR_RXT0  (1 << 7)   /* RX Timer Interrupt */

/* =========================================================
 * Descriptor structures (must be 16 bytes each)
 * ========================================================= */

typedef struct {
    uint64_t addr;      /* Buffer physical address */
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;      /* Buffer physical address */
    uint16_t length;
    uint8_t  cso;       /* Checksum Offset */
    uint8_t  cmd;       /* Command */
    uint8_t  status;
    uint8_t  css;       /* Checksum Start */
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

/* =========================================================
 * Driver state
 * ========================================================= */

#define E1000_RX_DESC_COUNT 32
#define E1000_TX_DESC_COUNT 32
#define E1000_BUF_SIZE      2048

static volatile uint8_t*  e1000_mmio = NULL;
static bool               e1000_ready = false;
mac_addr_t e1000_mac;

/* Descriptor rings - physically contiguous aligned to 16 bytes */
static e1000_rx_desc_t rx_descs[E1000_RX_DESC_COUNT] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_descs[E1000_TX_DESC_COUNT] __attribute__((aligned(16)));

/* Packet data buffers */
static uint8_t rx_bufs[E1000_RX_DESC_COUNT][E1000_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t tx_bufs[E1000_TX_DESC_COUNT][E1000_BUF_SIZE] __attribute__((aligned(64)));

static uint32_t rx_tail = 0;
static uint32_t tx_tail = 0;

/* Packet counters */
static uint32_t g_tx_packets = 0;
static uint32_t g_rx_packets = 0;
static uint32_t g_tx_errors  = 0;
static uint32_t g_rx_errors  = 0;

/* =========================================================
 * MMIO register access
 * ========================================================= */

static inline uint32_t e1000_read(uint32_t reg)
{
    return *(volatile uint32_t*)(e1000_mmio + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t*)(e1000_mmio + reg) = val;
}

/* =========================================================
 * EEPROM read (93C46/93C66 protocol via EERD register)
 * ========================================================= */

static uint16_t eeprom_read(uint8_t addr)
{
    /* Start read */
    e1000_write(E1000_EERD, (1u) | ((uint32_t)addr << 8));

    /* Wait for completion (bit 4 = DONE) */
    uint32_t val;
    for (int i = 0; i < 10000; i++) {
        val = e1000_read(E1000_EERD);
        if (val & (1 << 4)) break;
    }
    return (uint16_t)(val >> 16);
}

/* =========================================================
 * Initialization
 * ========================================================= */

int e1000_init(void)
{
    /* Find the e1000 PCI device */
    pci_device_t* pci = pci_find_device(PCI_VENDOR_INTEL, PCI_DEVICE_E1000);
    if (!pci) {
        kinfo("e1000: no Intel 82540EM found");
        return -1;
    }
    kinfo("e1000: found at %02x:%02x.%x BAR0=0x%08x IRQ=%u",
          pci->bus, pci->slot, pci->func, pci->bar[0], pci->irq_line);

    /* Enable bus mastering */
    pci_enable_bus_mastering(pci);

    /* Map BAR0 (MMIO registers) into kernel virtual address space */
    uint64_t bar0_phys = pci_bar_address(pci, 0);
    if (bar0_phys == 0) {
        klog_warn("e1000: BAR0 is zero, aborting");
        return -1;
    }

    /* Map 128KB of MMIO registers at BAR0 */
    uint64_t bar0_virt = KERNEL_VMA_BASE + bar0_phys;
    for (uint64_t off = 0; off < 0x20000; off += PAGE_SIZE) {
        vmm_map_page(kernel_pml4,
                     bar0_virt + off,
                     bar0_phys + off,
                     PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    }
    e1000_mmio = (volatile uint8_t*)bar0_virt;

    /* Software reset */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 1000000; i++) __asm__("pause");

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU);

    /* Read MAC address from EEPROM */
    uint16_t mac0 = eeprom_read(0);
    uint16_t mac1 = eeprom_read(1);
    uint16_t mac2 = eeprom_read(2);
    e1000_mac.b[0] = (uint8_t)(mac0 & 0xFF);
    e1000_mac.b[1] = (uint8_t)(mac0 >> 8);
    e1000_mac.b[2] = (uint8_t)(mac1 & 0xFF);
    e1000_mac.b[3] = (uint8_t)(mac1 >> 8);
    e1000_mac.b[4] = (uint8_t)(mac2 & 0xFF);
    e1000_mac.b[5] = (uint8_t)(mac2 >> 8);
    kinfo("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x",
          e1000_mac.b[0], e1000_mac.b[1], e1000_mac.b[2],
          e1000_mac.b[3], e1000_mac.b[4], e1000_mac.b[5]);

    /* Zero multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* ---- RX descriptor ring setup ---- */
    memset(rx_descs, 0, sizeof(rx_descs));
    for (int i = 0; i < E1000_RX_DESC_COUNT; i++) {
        rx_descs[i].addr   = VIRT_TO_PHYS((uintptr_t)rx_bufs[i]);
        rx_descs[i].status = 0;
    }
    uint64_t rx_phys = VIRT_TO_PHYS((uintptr_t)rx_descs);
    e1000_write(E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000_write(E1000_RDLEN, E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_RDH, 0);
    rx_tail = E1000_RX_DESC_COUNT - 1;
    e1000_write(E1000_RDT, rx_tail);

    /* RCTL: enable RX, broadcast accept, 2KB buffers, strip CRC */
    e1000_write(E1000_RCTL,
                E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE |
                E1000_RCTL_MPE | E1000_RCTL_BAM | E1000_RCTL_SECRC);

    /* ---- TX descriptor ring setup ---- */
    memset(tx_descs, 0, sizeof(tx_descs));
    for (int i = 0; i < E1000_TX_DESC_COUNT; i++) {
        tx_descs[i].addr   = VIRT_TO_PHYS((uintptr_t)tx_bufs[i]);
        tx_descs[i].status = 0x01;  /* Mark initially done */
    }
    uint64_t tx_phys = VIRT_TO_PHYS((uintptr_t)tx_descs);
    e1000_write(E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000_write(E1000_TDLEN, E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDH, 0);
    tx_tail = 0;
    e1000_write(E1000_TDT, 0);

    /* TCTL: enable TX, pad short packets */
    e1000_write(E1000_TCTL,
                E1000_TCTL_EN | E1000_TCTL_PSP |
                E1000_TCTL_CT | E1000_TCTL_COLD);

    /* TX inter-packet gap (standard values) */
    e1000_write(E1000_TIPG, 0x00702008);

    /* Enable receive interrupt */
    e1000_write(E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_LSC);

    /* Register IRQ handler */
    uint8_t irq = pci->irq_line;
    if (irq > 0 && irq < 16) {
        irq_register_handler(irq, (irq_handler_t) e1000_irq_handler);
        pic_clear_mask(irq);
        kinfo("e1000: registered IRQ %u", irq);
    }

    /* Write MAC to receive address register */
    uint32_t ral = ((uint32_t)e1000_mac.b[0])
                 | ((uint32_t)e1000_mac.b[1] << 8)
                 | ((uint32_t)e1000_mac.b[2] << 16)
                 | ((uint32_t)e1000_mac.b[3] << 24);
    uint32_t rah = ((uint32_t)e1000_mac.b[4])
                 | ((uint32_t)e1000_mac.b[5] << 8)
                 | (1u << 31);  /* Address Valid bit */
    e1000_write(E1000_RAL0, ral);
    e1000_write(E1000_RAH0, rah);

    e1000_ready = true;
    kinfo("e1000: initialized, link %s",
          (e1000_read(E1000_STATUS) & 0x2) ? "up" : "down");

    /* Publish MAC and callbacks to network stack */
    memcpy(net_iface.mac.b, e1000_mac.b, 6);
    net_iface.send = e1000_send;
    net_iface.poll = e1000_receive_poll;  /* Used by TCP/UDP spin-wait loops */
    net_iface.up   = true;

    return 0;
}

/* =========================================================
 * Transmit
 * ========================================================= */

int e1000_send(const void* data, size_t len)
{
    if (!e1000_ready) return -1;
    if (len > E1000_BUF_SIZE) return -1;

    uint32_t idx = tx_tail % E1000_TX_DESC_COUNT;

    /* Wait for descriptor to be free */
    for (int wait = 0; !(tx_descs[idx].status & 0x01) && wait < 100000; wait++)
        __asm__("pause");

    if (!(tx_descs[idx].status & 0x01)) {
        klog_warn("e1000: TX descriptor busy, dropping packet");
        g_tx_errors++;
        return -1;
    }

    memcpy(tx_bufs[idx], data, len);
    tx_descs[idx].length = (uint16_t)len;
    tx_descs[idx].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    tx_descs[idx].status = 0;

    tx_tail = (tx_tail + 1) % E1000_TX_DESC_COUNT;
    e1000_write(E1000_TDT, tx_tail);
    g_tx_packets++;

    return 0;
}

/* =========================================================
 * Receive (poll or IRQ)
 * ========================================================= */

void e1000_receive_poll(void)
{
    if (!e1000_ready) return;

    while (1) {
        uint32_t next = (rx_tail + 1) % E1000_RX_DESC_COUNT;
        e1000_rx_desc_t* desc = &rx_descs[next];

        if (!(desc->status & E1000_RXD_STAT_DD)) break;

        if (desc->length > 0 && !(desc->errors)) {
            net_receive(rx_bufs[next], desc->length);
            g_rx_packets++;
        } else if (desc->errors) {
            g_rx_errors++;
        }

        desc->status = 0;
        rx_tail = next;
        e1000_write(E1000_RDT, rx_tail);
    }
}

void e1000_get_stats(e1000_stats_t* out)
{
    if (!out) return;
    out->tx_packets = g_tx_packets;
    out->rx_packets = g_rx_packets;
    out->tx_errors  = g_tx_errors;
    out->rx_errors  = g_rx_errors;
}

bool e1000_link_up(void)
{
    if (!e1000_mmio) return false;
    return (e1000_read(E1000_STATUS) & 0x2) != 0;
}

void e1000_irq_handler(void* regs)
{
    (void)regs;
    uint32_t icr = e1000_read(E1000_ICR);  /* Clears interrupt */

    if (icr & E1000_ICR_RXT0) {
        e1000_receive_poll();
    }
    if (icr & E1000_ICR_LSC) {
        kinfo("e1000: link status changed -> %s",
              (e1000_read(E1000_STATUS) & 0x2) ? "up" : "down");
    }
}

void e1000_get_mac(mac_addr_t* out)
{
    memcpy(out->b, e1000_mac.b, 6);
}
