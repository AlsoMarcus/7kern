// 7Kern - ACPI tables (FADT/DSDT for power off and reset), SMBIOS, power management
#include "kernel.h"

char g_sys_vendor[64], g_sys_product[64], g_bios_vendor[64], g_bios_version[64], g_bios_date[32];
uint64_t g_smbios_ram_bytes;
char g_acpi_oem[8];
int g_acpi_revision;

static uint16_t pm1a_cnt, pm1b_cnt;
static uint32_t smi_cmd;
static uint8_t acpi_enable_val;
static uint16_t slp_typa, slp_typb;
static bool s5_found;
static bool reset_supported;
static uint8_t reset_space, reset_value;
static uint64_t reset_addr;

typedef struct __attribute__((packed)) {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} sdt_t;

static sdt_t *map_table(uint64_t phys) {
    sdt_t *h = vmm_map_phys(phys, sizeof(sdt_t));
    vmm_map_phys(phys, h->length);
    return h;
}

static void parse_s5(uint8_t *dsdt, uint32_t len) {
    for (uint32_t i = 0; i + 4 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4)) continue;
        if (!(i >= 1 && (dsdt[i - 1] == 0x08 || (i >= 2 && dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\')))) continue;
        uint8_t *p = dsdt + i + 4;
        if (*p != 0x12) continue;  // PackageOp
        p++;
        p += ((*p & 0xC0) >> 6) + 1;  // skip PkgLength
        p++;                          // NumElements
        uint16_t v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) { p++; v[k] = *p++; }       // BytePrefix
            else if (*p == 0x00 || *p == 0x01) v[k] = *p++;  // Zero / One
            else v[k] = *p++;
        }
        slp_typa = v[0];
        slp_typb = v[1];
        s5_found = true;
        return;
    }
}

static void parse_fadt(sdt_t *f) {
    uint8_t *b = (uint8_t *)f;
    uint32_t len = f->length;
    uint32_t dsdt32 = *(uint32_t *)(b + 40);
    smi_cmd = *(uint32_t *)(b + 48);
    acpi_enable_val = b[52];
    pm1a_cnt = (uint16_t)*(uint32_t *)(b + 64);
    pm1b_cnt = (uint16_t)*(uint32_t *)(b + 68);
    if (len > 108) g_acpi_century_reg = b[108];
    uint32_t flags = len >= 116 ? *(uint32_t *)(b + 112) : 0;
    if (len >= 129 && (flags & (1 << 10))) {
        reset_supported = true;
        reset_space = b[116];
        reset_addr = *(uint64_t *)(b + 120);
        reset_value = b[128];
    }
    uint64_t dsdt = dsdt32;
    if (len >= 148 && *(uint64_t *)(b + 140)) dsdt = *(uint64_t *)(b + 140);
    if (dsdt) {
        sdt_t *d = map_table(dsdt);
        if (!memcmp(d->sig, "DSDT", 4)) parse_s5((uint8_t *)d + sizeof(sdt_t), d->length - sizeof(sdt_t));
    }
}

void acpi_init(uint64_t rsdp_phys) {
    if (!rsdp_phys) { klog("acpi: no RSDP"); return; }
    uint8_t *rsdp = vmm_map_phys(rsdp_phys, 36);
    if (memcmp(rsdp, "RSD PTR ", 8)) { klog("acpi: bad RSDP signature"); return; }
    memcpy(g_acpi_oem, rsdp + 9, 6);
    g_acpi_oem[6] = 0;
    g_acpi_revision = rsdp[15];
    bool xsdt = rsdp[15] >= 2 && *(uint64_t *)(rsdp + 24);
    sdt_t *root = map_table(xsdt ? *(uint64_t *)(rsdp + 24) : *(uint32_t *)(rsdp + 16));
    int esz = xsdt ? 8 : 4;
    int n = (root->length - sizeof(sdt_t)) / esz;
    uint8_t *ents = (uint8_t *)root + sizeof(sdt_t);
    char sigs[128] = "";
    for (int i = 0; i < n; i++) {
        uint64_t a = xsdt ? *(uint64_t *)(ents + i * 8) : *(uint32_t *)(ents + i * 4);
        sdt_t *t = map_table(a);
        char s[6] = {t->sig[0], t->sig[1], t->sig[2], t->sig[3], ' ', 0};
        strlcat(sigs, s, sizeof sigs);
        if (!memcmp(t->sig, "FACP", 4)) parse_fadt(t);
    }
    klog("acpi: rev %d, OEM '%s', tables: %s", g_acpi_revision, g_acpi_oem, sigs);
    klog("acpi: PM1a_CNT=%x S5=%s (%u/%u) reset=%s", pm1a_cnt, s5_found ? "yes" : "no", slp_typa, slp_typb,
         reset_supported ? "yes" : "no");
}

// ---------------------------------------------------------------- SMBIOS
static const char *sm_str(uint8_t *s, uint8_t idx) {
    if (!idx) return "";
    char *p = (char *)s + s[1];
    for (int i = 1; i < idx && *p; i++) p += strlen(p) + 1;
    return p;
}

static void sm_copy(char *dst, size_t n, const char *s) {
    strlcpy(dst, s, n);
    int l = strlen(dst);
    while (l > 0 && dst[l - 1] == ' ') dst[--l] = 0;
}

void smbios_init(uint64_t e32, uint64_t e64) {
    uint64_t table = 0, len = 0;
    if (e64) {
        uint8_t *e = vmm_map_phys(e64, 32);
        if (!memcmp(e, "_SM3_", 5)) { table = *(uint64_t *)(e + 0x10); len = *(uint32_t *)(e + 0x0C); }
    }
    if (!table && e32) {
        uint8_t *e = vmm_map_phys(e32, 32);
        if (!memcmp(e, "_SM_", 4)) { table = *(uint32_t *)(e + 0x18); len = *(uint16_t *)(e + 0x16); }
    }
    if (!table || !len) { klog("smbios: not available"); return; }
    uint8_t *p = vmm_map_phys(table, len), *end = p + len;
    while (p + 4 <= end) {
        uint8_t type = p[0], hl = p[1];
        if (hl < 4) break;
        if (type == 0 && hl >= 9) {
            sm_copy(g_bios_vendor, sizeof g_bios_vendor, sm_str(p, p[4]));
            sm_copy(g_bios_version, sizeof g_bios_version, sm_str(p, p[5]));
            sm_copy(g_bios_date, sizeof g_bios_date, sm_str(p, p[8]));
        } else if (type == 1 && hl >= 8) {
            sm_copy(g_sys_vendor, sizeof g_sys_vendor, sm_str(p, p[4]));
            sm_copy(g_sys_product, sizeof g_sys_product, sm_str(p, p[5]));
        } else if (type == 17 && hl >= 0x15) {
            uint16_t sz = *(uint16_t *)(p + 0x0C);
            if (sz == 0x7FFF && hl >= 0x20) g_smbios_ram_bytes += (uint64_t)(*(uint32_t *)(p + 0x1C)) << 20;
            else if (sz != 0 && sz != 0xFFFF) g_smbios_ram_bytes += (sz & 0x8000) ? (uint64_t)(sz & 0x7FFF) << 10 : (uint64_t)sz << 20;
        } else if (type == 127) break;
        // skip formatted area and string set (terminated by double NUL)
        uint8_t *s = p + hl;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) s++;
        p = s + 2;
    }
    klog("smbios: %s %s, firmware %s %s", g_sys_vendor, g_sys_product, g_bios_vendor, g_bios_version);
}

// ---------------------------------------------------------------- power
void power_shutdown(void) {
    cli();
    klog("power: shutting down");
    if (pm1a_cnt && s5_found) {
        if (smi_cmd && acpi_enable_val && !(inw(pm1a_cnt) & 1)) {
            outb(smi_cmd, acpi_enable_val);
            for (int i = 0; i < 3000000 && !(inw(pm1a_cnt) & 1); i++) io_wait();
        }
        outw(pm1a_cnt, (slp_typa << 10) | (1 << 13));
        if (pm1b_cnt) outw(pm1b_cnt, (slp_typb << 10) | (1 << 13));
        for (int i = 0; i < 1000000; i++) io_wait();
    }
    // emulator-specific fallbacks
    outw(0x604, 0x2000);   // QEMU
    outw(0xB004, 0x2000);  // Bochs / old QEMU
    outw(0x4004, 0x3400);  // VirtualBox
    extern void (*g_shutdown_failed_hook)(void);
    if (g_shutdown_failed_hook) g_shutdown_failed_hook();
    for (;;) hlt();
}

void power_reboot(void) {
    cli();
    klog("power: rebooting");
    if (reset_supported && reset_space == 1) outb((uint16_t)reset_addr, reset_value);
    for (int i = 0; i < 100000; i++) io_wait();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);  // 8042 pulse reset line
    for (int i = 0; i < 100000; i++) io_wait();
    outb(0xCF9, 0x06);  // PCI reset control
    for (int i = 0; i < 100000; i++) io_wait();
    struct __attribute__((packed)) { uint16_t l; uint64_t b; } null_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" ::"m"(null_idt));  // triple fault
    for (;;) hlt();
}
