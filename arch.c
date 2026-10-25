// 7Kern - x86_64 CPU setup: GDT/TSS, IDT, 8259 PIC, PIT timer, CPUID
#include "kernel.h"

// ---------------------------------------------------------------- GDT + TSS
typedef struct __attribute__((packed)) {
    uint32_t rsv0;
    uint64_t rsp[3];
    uint64_t rsv1;
    uint64_t ist[7];
    uint64_t rsv2;
    uint16_t rsv3, iomap;
} tss_t;

static uint64_t gdt[7];
static tss_t tss;
static uint8_t df_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[8192] __attribute__((aligned(16)));

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;  // kernel code 64-bit
    gdt[2] = 0x00CF92000000FFFFULL;  // kernel data
    gdt[3] = 0x00CFF2000000FFFFULL;  // user data
    gdt[4] = 0x00AFFA000000FFFFULL;  // user code
    memset(&tss, 0, sizeof tss);
    tss.ist[0] = (uint64_t)(df_stack + sizeof df_stack);
    tss.ist[1] = (uint64_t)(nmi_stack + sizeof nmi_stack);
    tss.iomap = sizeof(tss_t);
    uint64_t base = (uint64_t)&tss, limit = sizeof(tss_t) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr = {sizeof(gdt) - 1, (uint64_t)gdt};
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :: "m"(gdtr) : "rax", "memory");
    __asm__ volatile("ltr %0" ::"r"((uint16_t)0x28));
}

// ---------------------------------------------------------------- IDT
typedef struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} idt_entry_t;

static idt_entry_t idt[256];
extern uint64_t isr_table[48];
extern void isr_ignore(void);
static void (*irq_handlers[16])(void);

static void idt_set(int v, uint64_t fn, int ist) {
    idt[v].off_lo = fn & 0xFFFF;
    idt[v].sel = 0x08;
    idt[v].ist = ist;
    idt[v].attr = 0x8E;
    idt[v].off_mid = (fn >> 16) & 0xFFFF;
    idt[v].off_hi = fn >> 32;
    idt[v].zero = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) idt_set(i, (uint64_t)isr_ignore, 0);
    for (int i = 0; i < 48; i++) idt_set(i, isr_table[i], 0);
    idt[8].ist = 1;  // double fault on its own stack
    idt[2].ist = 2;  // NMI
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(idtr));
}

void panic_exception(regs_t *r);  // main.c

void isr_dispatch(regs_t *r) {
    if (r->vector < 32) {
        panic_exception(r);
        return;
    }
    int irq = (int)r->vector - 32;
    if (irq == 7 || irq == 15) {  // spurious IRQ check
        uint16_t port = irq == 7 ? 0x20 : 0xA0;
        outb(port, 0x0B);
        if (!(inb(port) & 0x80)) {
            if (irq == 15) outb(0x20, 0x20);
            return;
        }
    }
    if (irq_handlers[irq]) irq_handlers[irq]();
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// ---------------------------------------------------------------- 8259 PIC
static uint16_t irq_mask = 0xFFFF;

void pic_init(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();  // master offset 32
    outb(0xA1, 0x28); io_wait();  // slave offset 40
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    irq_mask = 0xFFFF & ~(1 << 2);  // cascade
    outb(0x21, irq_mask & 0xFF);
    outb(0xA1, irq_mask >> 8);
}
void irq_unmask(int irq) {
    irq_mask &= ~(1 << irq);
    outb(0x21, irq_mask & 0xFF);
    outb(0xA1, irq_mask >> 8);
}
void irq_register(int irq, void (*fn)(void)) { irq_handlers[irq] = fn; irq_unmask(irq); }

// ---------------------------------------------------------------- PIT
static volatile uint64_t ticks;
static uint32_t tick_hz = 200;
static void pit_irq(void) { ticks++; }

void pit_init(uint32_t hz) {
    tick_hz = hz;
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, pit_irq);
}
uint64_t timer_ms(void) { return ticks * 1000 / tick_hz; }
void sleep_ms(uint64_t ms) {
    uint64_t end = timer_ms() + ms;
    while (timer_ms() < end) hlt();
}

// ---------------------------------------------------------------- CPU
cpu_info_t g_cpu;

void cpu_detect(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max = a;
    memcpy(g_cpu.vendor, &b, 4);
    memcpy(g_cpu.vendor + 4, &d, 4);
    memcpy(g_cpu.vendor + 8, &c, 4);
    g_cpu.vendor[12] = 0;
    if (max >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        g_cpu.stepping = a & 0xF;
        g_cpu.model = (a >> 4) & 0xF;
        g_cpu.family = (a >> 8) & 0xF;
        if (g_cpu.family == 0xF) g_cpu.family += (a >> 20) & 0xFF;
        if (g_cpu.family >= 6) g_cpu.model |= ((a >> 16) & 0xF) << 4;
        g_cpu.logical_cpus = (b >> 16) & 0xFF;
        if (!g_cpu.logical_cpus) g_cpu.logical_cpus = 1;
        g_cpu.sse = d & (1 << 25); g_cpu.sse2 = d & (1 << 26);
        g_cpu.sse3 = c & 1; g_cpu.ssse3 = c & (1 << 9);
        g_cpu.sse41 = c & (1 << 19); g_cpu.sse42 = c & (1 << 20);
        g_cpu.aes = c & (1 << 25); g_cpu.avx = c & (1 << 28);
        g_cpu.x2apic = c & (1 << 21); g_cpu.hypervisor = c & (1u << 31);
    }
    if (max >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_cpu.avx2 = b & (1 << 5);
    }
    if (max >= 4 && !strcmp(g_cpu.vendor, "GenuineIntel")) {
        cpuid(4, 0, &a, &b, &c, &d);
        uint32_t cores = ((a >> 26) & 0x3F) + 1;
        if (cores > g_cpu.logical_cpus) g_cpu.logical_cpus = cores;
    }
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        uint32_t ea = a;
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        g_cpu.nx = d & (1 << 20);
        g_cpu.lm = d & (1 << 29);
        if (ea >= 0x80000004) {
            uint32_t *p = (uint32_t *)g_cpu.brand;
            for (uint32_t l = 0; l < 3; l++) cpuid(0x80000002 + l, 0, &p[l * 4], &p[l * 4 + 1], &p[l * 4 + 2], &p[l * 4 + 3]);
            g_cpu.brand[48] = 0;
            // trim leading/trailing spaces
            char *s = g_cpu.brand;
            while (*s == ' ') s++;
            memmove(g_cpu.brand, s, strlen(s) + 1);
            int n = strlen(g_cpu.brand);
            while (n > 0 && g_cpu.brand[n - 1] == ' ') g_cpu.brand[--n] = 0;
        }
    }
    if (!g_cpu.brand[0]) ksnprintf(g_cpu.brand, sizeof g_cpu.brand, "%s Family %u Model %u", g_cpu.vendor, g_cpu.family, g_cpu.model);
    if (g_cpu.hypervisor) {
        cpuid(0x40000000, 0, &a, &b, &c, &d);
        memcpy(g_cpu.hv_vendor, &b, 4);
        memcpy(g_cpu.hv_vendor + 4, &c, 4);
        memcpy(g_cpu.hv_vendor + 8, &d, 4);
        g_cpu.hv_vendor[12] = 0;
    }
    // measure TSC frequency against the PIT (interrupts must be enabled)
    uint64_t t0 = timer_ms();
    while (timer_ms() == t0) hlt();
    uint64_t start = rdtsc(), ms0 = timer_ms();
    while (timer_ms() - ms0 < 100) hlt();
    uint64_t end = rdtsc();
    g_cpu.tsc_hz = (end - start) * 10;
}
