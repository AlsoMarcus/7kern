// 7Kern - PCI bus enumeration (legacy configuration mechanism #1)
#include "kernel.h"

pci_dev_t g_pci[64];
int g_pci_count;

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    return inl(0xCFC);
}
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

void pci_enable_bus_master(pci_dev_t *p) {
    uint32_t cmd = pci_read32(p->bus, p->dev, p->func, 0x04);
    cmd |= 0x06;           // memory space + bus master
    cmd &= ~(1u << 10);    // interrupts allowed (we poll anyway)
    pci_write32(p->bus, p->dev, p->func, 0x04, cmd);
}

static void add_func(uint8_t b, uint8_t d, uint8_t f) {
    uint32_t id = pci_read32(b, d, f, 0);
    if ((id & 0xFFFF) == 0xFFFF || g_pci_count >= (int)ARRAY_LEN(g_pci)) return;
    pci_dev_t *p = &g_pci[g_pci_count++];
    memset(p, 0, sizeof *p);
    p->bus = b; p->dev = d; p->func = f;
    p->vendor = id & 0xFFFF; p->device = id >> 16;
    uint32_t cl = pci_read32(b, d, f, 0x08);
    p->rev = cl & 0xFF; p->progif = (cl >> 8) & 0xFF; p->subcls = (cl >> 16) & 0xFF; p->cls = cl >> 24;
    p->irq = pci_read32(b, d, f, 0x3C) & 0xFF;
    uint8_t htype = (pci_read32(b, d, f, 0x0C) >> 16) & 0x7F;
    if (htype != 0) return;
    for (int i = 0; i < 6; i++) {
        uint32_t bar = pci_read32(b, d, f, 0x10 + i * 4);
        if (bar & 1) { p->bar_io[i] = true; p->bar[i] = bar & ~3u; continue; }
        uint64_t addr = bar & ~0xFu;
        if (((bar >> 1) & 3) == 2 && i < 5) {
            addr |= (uint64_t)pci_read32(b, d, f, 0x10 + (i + 1) * 4) << 32;
            p->bar[i] = addr;
            i++;
            continue;
        }
        p->bar[i] = addr;
    }
}

void pci_init(void) {
    for (int b = 0; b < 256; b++) {
        for (int d = 0; d < 32; d++) {
            uint32_t id = pci_read32(b, d, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            add_func(b, d, 0);
            uint8_t ht = (pci_read32(b, d, 0, 0x0C) >> 16) & 0xFF;
            if (ht & 0x80)
                for (int f = 1; f < 8; f++) add_func(b, d, f);
        }
    }
    klog("pci: %d devices", g_pci_count);
    for (int i = 0; i < g_pci_count; i++) {
        char name[96];
        pci_device_name(&g_pci[i], name, sizeof name);
        klog("pci: %02x:%02x.%d %04x:%04x %s", g_pci[i].bus, g_pci[i].dev, g_pci[i].func, g_pci[i].vendor, g_pci[i].device, name);
    }
}

const char *pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x00: return "Unclassified device";
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return "NVMe controller";
        default: return "Storage controller";
        }
    case 0x02: return s == 0 ? "Ethernet controller" : (s == 0x80 ? "Network controller" : "Network controller");
    case 0x03: return s == 0 ? "VGA compatible controller" : "Display controller";
    case 0x04: return s == 3 ? "Audio device" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        case 0x80: return "Bridge";
        default: return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default: return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    default: return "Device";
    }
}

const char *pci_vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat Virtio";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x144D: return "Samsung";
    case 0x15B7: return "Sandisk";
    case 0x1987: return "Phison";
    case 0x1C5C: return "SK hynix";
    case 0x1E0F: return "KIOXIA";
    case 0x1013: return "Cirrus Logic";
    case 0x1414: return "Microsoft";
    case 0x1106: return "VIA";
    case 0x1912: return "Renesas";
    case 0x1B21: return "ASMedia";
    case 0x8087: return "Intel";
    default: return NULL;
    }
}

static const struct { uint16_t v, d; const char *name; } known[] = {
    {0x8086, 0x1237, "440FX - 82441FX PMC [Natoma]"},
    {0x8086, 0x7000, "82371SB PIIX3 ISA [Natoma/Triton II]"},
    {0x8086, 0x7010, "82371SB PIIX3 IDE [Natoma/Triton II]"},
    {0x8086, 0x7020, "82371SB PIIX3 USB [Natoma/Triton II]"},
    {0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI"},
    {0x8086, 0x29C0, "82G33/G31/P35/P31 Express DRAM Controller"},
    {0x8086, 0x2918, "82801IB (ICH9) LPC Interface Controller"},
    {0x8086, 0x2922, "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA [AHCI]"},
    {0x8086, 0x2930, "82801I (ICH9 Family) SMBus Controller"},
    {0x8086, 0x293E, "82801I (ICH9 Family) HD Audio Controller"},
    {0x8086, 0x100E, "82540EM Gigabit Ethernet Controller"},
    {0x8086, 0x10D3, "82574L Gigabit Network Connection"},
    {0x8086, 0x2668, "82801FB/FBM/FR/FW/FRW (ICH6) HD Audio"},
    {0x8086, 0x2415, "82801AA AC'97 Audio Controller"},
    {0x8086, 0x24CD, "82801DB/DBM (ICH4) USB2 EHCI"},
    {0x1234, 0x1111, "Standard VGA (Bochs/QEMU)"},
    {0x1B36, 0x0010, "QEMU NVM Express Controller"},
    {0x1B36, 0x000D, "QEMU XHCI Host Controller"},
    {0x1B36, 0x0100, "QXL paravirtual graphic card"},
    {0x1AF4, 0x1000, "Virtio network device"},
    {0x1AF4, 0x1001, "Virtio block device"},
    {0x1AF4, 0x1041, "Virtio 1.0 network device"},
    {0x1AF4, 0x1042, "Virtio 1.0 block device"},
    {0x1AF4, 0x1050, "Virtio 1.0 GPU"},
    {0x15AD, 0x0405, "SVGA II Adapter"},
    {0x15AD, 0x0740, "Virtual Machine Communication Interface"},
    {0x80EE, 0xBEEF, "VirtualBox Graphics Adapter"},
    {0x80EE, 0xCAFE, "VirtualBox Guest Service"},
    {0x1022, 0x2000, "79c970 [PCnet32 LANCE]"},
    {0x10EC, 0x8139, "RTL-8100/8101L/8139 Fast Ethernet"},
    {0x10EC, 0x8168, "RTL8111/8168 Gigabit Ethernet"},
    {0x1013, 0x00B8, "GD 5446"},
};

void pci_device_name(const pci_dev_t *p, char *buf, size_t n) {
    const char *vn = pci_vendor_name(p->vendor);
    for (size_t i = 0; i < ARRAY_LEN(known); i++) {
        if (known[i].v == p->vendor && known[i].d == p->device) {
            ksnprintf(buf, n, "%s %s", vn ? vn : "", known[i].name);
            return;
        }
    }
    if (vn) ksnprintf(buf, n, "%s %s", vn, pci_class_name(p->cls, p->subcls));
    else ksnprintf(buf, n, "%s [%04x:%04x]", pci_class_name(p->cls, p->subcls), p->vendor, p->device);
}
