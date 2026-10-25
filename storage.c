// 7Kern - storage device detection: legacy IDE (PIO), AHCI SATA, NVMe, virtio-blk.
// Drives are identified (model, serial, capacity) for the system information pages.
#include "kernel.h"

drive_t g_drives[16];
int g_drive_count;

static drive_t *new_drive(void) {
    if (g_drive_count >= (int)ARRAY_LEN(g_drives)) return NULL;
    drive_t *d = &g_drives[g_drive_count++];
    memset(d, 0, sizeof *d);
    return d;
}

static void trim(char *s) {
    int n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) s[--n] = 0;
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

// ATA strings are stored as big-endian 16-bit words
static void ata_string(const uint16_t *id, int w0, int words, char *out, size_t n) {
    size_t k = 0;
    for (int i = 0; i < words && k + 2 < n; i++) {
        out[k++] = id[w0 + i] >> 8;
        out[k++] = id[w0 + i] & 0xFF;
    }
    out[k] = 0;
    trim(out);
}

static void parse_identify(drive_t *d, const uint16_t *id, bool atapi) {
    ata_string(id, 27, 20, d->model, sizeof d->model);
    ata_string(id, 10, 10, d->serial, sizeof d->serial);
    if (atapi) { d->kind = DRIVE_OPTICAL; return; }
    uint64_t sectors;
    if (id[83] & (1 << 10))
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint32_t ss = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1 << 12))) ss = ((uint32_t)id[117] | ((uint32_t)id[118] << 16)) * 2;
    d->size_bytes = sectors * ss;
    d->media_present = true;
    d->kind = id[217] == 1 ? DRIVE_SSD : DRIVE_HDD;
}

// ---------------------------------------------------------------- legacy IDE (PIO)
static bool ata_wait(uint16_t io, uint8_t mask, uint8_t want, uint8_t *st_out) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = inb(io + 7);
        if (st == 0xFF) return false;
        if (!(st & 0x80) && (st & 0x01)) { if (st_out) *st_out = st; return false; }
        if (!(st & 0x80) && (st & mask) == want) { if (st_out) *st_out = st; return true; }
    }
    return false;
}

static bool atapi_capacity(uint16_t io, uint16_t ctl, uint64_t *bytes) {
    for (int attempt = 0; attempt < 3; attempt++) {
        outb(io + 1, 0);
        outb(io + 4, 8);
        outb(io + 5, 0);
        outb(io + 7, 0xA0);
        for (int i = 0; i < 4; i++) inb(ctl);
        if (!ata_wait(io, 0x08, 0x08, NULL)) { inb(io + 7); continue; }
        uint8_t pkt[12] = {0x25};
        for (int i = 0; i < 6; i++) outw(io, pkt[i * 2] | (pkt[i * 2 + 1] << 8));
        if (!ata_wait(io, 0x08, 0x08, NULL)) { inb(io + 7); continue; }
        uint8_t r[8];
        for (int i = 0; i < 4; i++) { uint16_t w = inw(io); r[i * 2] = w & 0xFF; r[i * 2 + 1] = w >> 8; }
        uint32_t last = ((uint32_t)r[0] << 24) | (r[1] << 16) | (r[2] << 8) | r[3];
        uint32_t bs = ((uint32_t)r[4] << 24) | (r[5] << 16) | (r[6] << 8) | r[7];
        if (bs == 0 || bs > 65536) bs = 2048;
        *bytes = ((uint64_t)last + 1) * bs;
        return true;
    }
    return false;
}

static void ide_probe(uint16_t io, uint16_t ctl, int slave) {
    outb(ctl, 0x02);  // nIEN: no interrupts
    outb(io + 6, slave ? 0xB0 : 0xA0);
    for (int i = 0; i < 15; i++) inb(ctl);
    uint8_t st = inb(io + 7);
    if (st == 0xFF || st == 0x7F) return;
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);
    for (int i = 0; i < 4; i++) inb(ctl);
    st = inb(io + 7);
    if (st == 0) return;
    int i;
    for (i = 0; i < 2000000 && ((st = inb(io + 7)) & 0x80); i++) {}
    if (st & 0x80) return;
    uint8_t lm = inb(io + 4), lh = inb(io + 5);
    bool atapi = false;
    if ((lm == 0x14 && lh == 0xEB) || (lm == 0x69 && lh == 0x96)) {
        atapi = true;
        outb(io + 7, 0xA1);
        for (int j = 0; j < 4; j++) inb(ctl);
    } else if (lm || lh) {
        if (!(lm == 0x3C && lh == 0xC3)) return;
    }
    if (!ata_wait(io, 0x08, 0x08, NULL)) return;
    uint16_t id[256];
    for (int j = 0; j < 256; j++) id[j] = inw(io);
    drive_t *d = new_drive();
    if (!d) return;
    strlcpy(d->bus, "IDE", sizeof d->bus);
    parse_identify(d, id, atapi);
    if (atapi) d->media_present = atapi_capacity(io, ctl, &d->size_bytes);
}

// ---------------------------------------------------------------- AHCI
#define HBA_R32(o) (*(volatile uint32_t *)(abar + (o)))
static uint8_t *abar;

static bool ahci_cmd(int port, uint8_t cmd, bool atapi, const uint8_t *acmd, uint64_t buf_phys, uint32_t len,
                     uint64_t mem_phys) {
    uint32_t pb = 0x100 + port * 0x80;
    uint8_t *mem = P2V(mem_phys);
    uint32_t *hdr = (uint32_t *)mem;  // command list slot 0
    uint8_t *tbl = mem + 0x800;
    memset(hdr, 0, 32);
    memset(tbl, 0, 0x100);
    hdr[0] = 5 | (atapi ? (1 << 5) : 0) | (1u << 16);  // CFL=5 dwords, PRDTL=1
    hdr[2] = (uint32_t)(mem_phys + 0x800);
    hdr[3] = (uint32_t)((mem_phys + 0x800) >> 32);
    tbl[0] = 0x27;  // H2D register FIS
    tbl[1] = 0x80;  // command
    tbl[2] = cmd;
    tbl[3] = atapi ? 1 : 0;  // features: DMA for packet commands
    tbl[7] = 0;
    if (atapi) {
        tbl[5] = len & 0xFF;
        tbl[6] = (len >> 8) & 0xFF;
        memcpy(tbl + 0x40, acmd, 12);
    }
    uint32_t *prd = (uint32_t *)(tbl + 0x80);
    prd[0] = (uint32_t)buf_phys;
    prd[1] = (uint32_t)(buf_phys >> 32);
    prd[3] = (len - 1) | (1u << 31);

    for (int i = 0; i < 1000000 && (HBA_R32(pb + 0x20) & 0x88); i++) {}
    HBA_R32(pb + 0x10) = 0xFFFFFFFF;
    HBA_R32(pb + 0x38) = 1;
    for (int i = 0; i < 5000000; i++) {
        if (!(HBA_R32(pb + 0x38) & 1)) break;
        if (HBA_R32(pb + 0x10) & (1u << 30)) return false;
    }
    if (HBA_R32(pb + 0x38) & 1) return false;
    return !(HBA_R32(pb + 0x20) & 1);
}

static void ahci_port(int port) {
    uint32_t pb = 0x100 + port * 0x80;
    uint32_t ssts = HBA_R32(pb + 0x28);
    if ((ssts & 0xF) != 3) return;
    uint32_t sig = HBA_R32(pb + 0x24);
    bool atapi = sig == 0xEB140101;
    if (sig != 0x00000101 && !atapi) return;  // port multipliers / bridges not supported

    // stop the port's command engine
    HBA_R32(pb + 0x18) &= ~1u;
    for (int i = 0; i < 1000000 && (HBA_R32(pb + 0x18) & (1 << 15)); i++) {}
    HBA_R32(pb + 0x18) &= ~(1u << 4);
    for (int i = 0; i < 1000000 && (HBA_R32(pb + 0x18) & (1 << 14)); i++) {}

    uint64_t mem = pmm_alloc(1), buf = pmm_alloc(1);
    if (!mem || !buf) return;
    HBA_R32(pb + 0x00) = (uint32_t)mem;
    HBA_R32(pb + 0x04) = (uint32_t)(mem >> 32);
    HBA_R32(pb + 0x08) = (uint32_t)(mem + 0x400);
    HBA_R32(pb + 0x0C) = (uint32_t)((mem + 0x400) >> 32);
    HBA_R32(pb + 0x30) = 0xFFFFFFFF;  // clear SERR
    HBA_R32(pb + 0x10) = 0xFFFFFFFF;
    HBA_R32(pb + 0x14) = 0;           // no interrupts
    HBA_R32(pb + 0x18) |= (1u << 4);
    HBA_R32(pb + 0x18) |= 1u;

    drive_t *d = new_drive();
    if (!d) return;
    strlcpy(d->bus, "SATA", sizeof d->bus);
    if (ahci_cmd(port, atapi ? 0xA1 : 0xEC, false, NULL, buf, 512, mem)) {
        parse_identify(d, P2V(buf), atapi);
    } else {
        strlcpy(d->model, atapi ? "SATA optical drive" : "SATA disk", sizeof d->model);
        d->kind = atapi ? DRIVE_OPTICAL : DRIVE_HDD;
    }
    if (atapi) {
        uint8_t pkt[12] = {0x25};
        for (int t = 0; t < 3 && !d->media_present; t++) {
            if (ahci_cmd(port, 0xA0, true, pkt, buf, 8, mem)) {
                uint8_t *r = P2V(buf);
                uint32_t last = ((uint32_t)r[0] << 24) | (r[1] << 16) | (r[2] << 8) | r[3];
                uint32_t bs = ((uint32_t)r[4] << 24) | (r[5] << 16) | (r[6] << 8) | r[7];
                if (bs == 0 || bs > 65536) bs = 2048;
                d->size_bytes = ((uint64_t)last + 1) * bs;
                d->media_present = true;
            }
        }
    }
    pmm_free(buf, 1);  // keep 'mem': the port still points at it
}

static void ahci_init(pci_dev_t *p) {
    if (!p->bar[5]) return;
    pci_enable_bus_master(p);
    abar = vmm_map_mmio(p->bar[5], 0x1100);
    HBA_R32(0x04) |= (1u << 31);  // AHCI enable
    uint32_t pi = HBA_R32(0x0C);
    uint32_t vs = HBA_R32(0x10);
    klog("ahci: controller v%u.%u, ports %08x", vs >> 16, (vs >> 8) & 0xFF, pi);
    for (int i = 0; i < 32; i++)
        if (pi & (1u << i)) ahci_port(i);
}

// ---------------------------------------------------------------- NVMe
static void nvme_init(pci_dev_t *p) {
    if (!p->bar[0]) return;
    pci_enable_bus_master(p);
    volatile uint8_t *r = vmm_map_mmio(p->bar[0], 0x4000);
#define NV32(o) (*(volatile uint32_t *)(r + (o)))
    uint64_t cap = NV32(0) | ((uint64_t)NV32(4) << 32);
    uint32_t dstrd = (cap >> 32) & 0xF;
    uint64_t timeout = ((cap >> 24) & 0xFF) * 500 + 500;
    if (NV32(0x14) & 1) {
        NV32(0x14) &= ~1u;
        uint64_t t = timer_ms();
        while ((NV32(0x1C) & 1) && timer_ms() - t < timeout) {}
    }
    uint64_t asq = pmm_alloc(1), acq = pmm_alloc(1), buf = pmm_alloc(1);
    if (!asq || !acq || !buf) return;
    NV32(0x0C) = 0xFFFFFFFF;  // mask interrupts
    NV32(0x24) = (15u << 16) | 15u;
    NV32(0x28) = (uint32_t)asq; NV32(0x2C) = (uint32_t)(asq >> 32);
    NV32(0x30) = (uint32_t)acq; NV32(0x34) = (uint32_t)(acq >> 32);
    NV32(0x14) = (6u << 16) | (4u << 20) | 1u;
    uint64_t t = timer_ms();
    while (!(NV32(0x1C) & 1) && timer_ms() - t < timeout) {
        if (NV32(0x1C) & 2) break;
    }
    if (!(NV32(0x1C) & 1)) { klog("nvme: controller failed to become ready"); return; }

    uint32_t *sq = P2V(asq);
    volatile uint32_t *cq = P2V(acq);
    int tail = 0, head = 0, phase = 1;
    uint32_t sq_db = 0x1000, cq_db = 0x1000 + (4u << dstrd);

    // submits an admin command and waits for its completion
#define NVME_ADMIN(opc, nsid, cdw10, out_ok)                                     \
    do {                                                                         \
        uint32_t *e = sq + tail * 16;                                            \
        memset(e, 0, 64);                                                        \
        e[0] = (opc) | ((uint32_t)(tail + 1) << 16);                             \
        e[1] = (nsid);                                                           \
        e[6] = (uint32_t)buf; e[7] = (uint32_t)(buf >> 32);                     \
        e[10] = (cdw10);                                                         \
        tail = (tail + 1) % 16;                                                  \
        NV32(sq_db) = tail;                                                      \
        out_ok = false;                                                          \
        uint64_t t0 = timer_ms();                                                \
        while (timer_ms() - t0 < 2000) {                                         \
            uint32_t dw3 = cq[head * 4 + 3];                                     \
            if (((dw3 >> 16) & 1) == (uint32_t)phase) {                          \
                out_ok = ((dw3 >> 17) & 0x7FFF) == 0;                            \
                head = (head + 1) % 16; if (head == 0) phase ^= 1;               \
                NV32(cq_db) = head;                                              \
                break;                                                           \
            }                                                                    \
        }                                                                        \
    } while (0)

    bool ok;
    NVME_ADMIN(0x06, 0, 1, ok);
    if (!ok) { klog("nvme: identify controller failed"); return; }
    uint8_t *idc = P2V(buf);
    drive_t *d = new_drive();
    if (!d) return;
    strlcpy(d->bus, "NVMe", sizeof d->bus);
    d->kind = DRIVE_NVME;
    d->media_present = true;
    memcpy(d->model, idc + 24, 40); d->model[40] = 0; trim(d->model);
    memcpy(d->serial, idc + 4, 20); d->serial[20] = 0; trim(d->serial);
    uint32_t nn = *(uint32_t *)(idc + 516);
    uint64_t total = 0;
    for (uint32_t ns = 1; ns <= nn && ns <= 4; ns++) {
        NVME_ADMIN(0x06, ns, 0, ok);
        if (!ok) continue;
        uint8_t *idn = P2V(buf);
        uint64_t nsze = *(uint64_t *)idn;
        uint8_t fl = idn[26] & 0xF;
        uint32_t lbads = idn[128 + fl * 4 + 2];
        if (lbads < 9 || lbads > 16) lbads = 9;
        total += nsze << lbads;
    }
    d->size_bytes = total;
    pmm_free(buf, 1);
}

// ---------------------------------------------------------------- virtio-blk (legacy interface)
static void virtio_blk_init(pci_dev_t *p) {
    if (!p->bar_io[0]) return;
    uint16_t io = p->bar[0];
    uint64_t sectors = inl(io + 0x14) | ((uint64_t)inl(io + 0x18) << 32);
    drive_t *d = new_drive();
    if (!d) return;
    strlcpy(d->model, "Virtio block device", sizeof d->model);
    strlcpy(d->bus, "Virtio", sizeof d->bus);
    d->kind = DRIVE_SSD;
    d->size_bytes = sectors * 512;
    d->media_present = true;
}

void storage_init(void) {
    bool ide_seen = false;
    for (int i = 0; i < g_pci_count; i++) {
        pci_dev_t *p = &g_pci[i];
        if (p->cls != 0x01) {
            if (p->vendor == 0x1AF4 && p->device == 0x1001) virtio_blk_init(p);
            continue;
        }
        if (p->subcls == 0x01 && !ide_seen) {
            ide_seen = true;
            uint16_t io0 = (p->progif & 1) && p->bar[0] ? p->bar[0] : 0x1F0;
            uint16_t ct0 = (p->progif & 1) && p->bar[1] ? p->bar[1] + 2 : 0x3F6;
            uint16_t io1 = (p->progif & 4) && p->bar[2] ? p->bar[2] : 0x170;
            uint16_t ct1 = (p->progif & 4) && p->bar[3] ? p->bar[3] + 2 : 0x376;
            ide_probe(io0, ct0, 0); ide_probe(io0, ct0, 1);
            ide_probe(io1, ct1, 0); ide_probe(io1, ct1, 1);
        } else if (p->subcls == 0x06 && p->progif == 0x01) {
            ahci_init(p);
        } else if (p->subcls == 0x08) {
            nvme_init(p);
        }
    }
    for (int i = 0; i < g_drive_count; i++) {
        char sz[32];
        fmt_size(sz, sizeof sz, g_drives[i].size_bytes);
        klog("storage: [%s] %s, %s%s", g_drives[i].bus, g_drives[i].model,
             g_drives[i].media_present ? sz : "no media", g_drives[i].kind == DRIVE_OPTICAL ? " (optical)" : "");
    }
}
