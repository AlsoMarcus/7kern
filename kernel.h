// 7Kern - common kernel declarations
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define OS_NAME        "7OS"
#define OS_VERSION     "1.0"
#define OS_CODENAME    "Genesis"
#define KERNEL_NAME    "7Kern"
#define KERNEL_VERSION "1.0.0"
#define KERNEL_ARCH    "x86_64"
#define OS_BUILD_DATE  __DATE__
#define OS_BUILD_TIME  __TIME__

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE 4096ULL

// ---------------------------------------------------------------- port I/O
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void invlpg(uint64_t a) { __asm__ volatile("invlpg (%0)" ::"r"(a) : "memory"); }
static inline uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f)::"memory"); return f; }
static inline void irq_restore(uint64_t f) { if (f & 0x200) sti(); }
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

// ---------------------------------------------------------------- lib
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void memset32(uint32_t *d, uint32_t v, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp_prefix(const char *s, const char *prefix);  // 1 if s starts with prefix (case-insensitive)
int strcasestr_has(const char *hay, const char *needle);
char *strcpy(char *d, const char *s);
void strlcpy(char *d, const char *s, size_t n);
void strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
int isspace(int c);
int isdigit(int c);
int tolower(int c);
long strtol(const char *s, const char **end, int base);
int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void fmt_size(char *buf, size_t n, uint64_t bytes);  // "1.5 GB"

// kernel log (serial + ring buffer)
void serial_init(void);
void serial_write(const char *s);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *klog_buffer(size_t *len);
__attribute__((noreturn)) void panic(const char *fmt, ...);

// ---------------------------------------------------------------- arch
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} regs_t;

void gdt_init(void);
void idt_init(void);
void pic_init(void);
void irq_register(int irq, void (*fn)(void));
void irq_unmask(int irq);
void pit_init(uint32_t hz);
uint64_t timer_ms(void);
void sleep_ms(uint64_t ms);
void cpu_detect(void);

typedef struct {
    char vendor[16];
    char brand[52];
    uint32_t family, model, stepping;
    uint32_t logical_cpus;
    uint64_t tsc_hz;
    bool sse, sse2, sse3, ssse3, sse41, sse42, avx, avx2, aes, hypervisor, x2apic, nx, lm;
    char hv_vendor[16];
} cpu_info_t;
extern cpu_info_t g_cpu;

// ---------------------------------------------------------------- memory
extern uint64_t g_hhdm;
#define P2V(p) ((void *)((uint64_t)(p) + g_hhdm))
#define V2P(v) ((uint64_t)(v) - g_hhdm)

typedef struct {
    uint64_t total_bytes;   // all RAM reported by firmware
    uint64_t usable_bytes;  // managed by the PMM
    uint64_t kernel_bytes;
    uint64_t reclaim_bytes;
    uint64_t reserved_bytes;
    uint64_t fb_bytes;
} mem_stats_t;
extern mem_stats_t g_mem;

void pmm_init(void);
uint64_t pmm_alloc(size_t pages);  // returns physical address of zeroed pages, 0 on failure
void pmm_free(uint64_t phys, size_t pages);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);
void *vmm_map_mmio(uint64_t phys, size_t size);
void *vmm_map_phys(uint64_t phys, size_t size);

void *kmalloc(size_t n);
void *kcalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
uint64_t heap_used_bytes(void);
uint64_t heap_total_bytes(void);

typedef struct { uint64_t base, length; uint32_t type; } memmap_entry_t;
extern memmap_entry_t g_memmap[128];
extern int g_memmap_count;
const char *memmap_type_name(uint32_t type);

// ---------------------------------------------------------------- input events
enum {
    KEY_NONE = 0, KEY_ESC = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SUPER, KEY_CAPS, KEY_SHIFT, KEY_CTRL, KEY_ALT,
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_SUPER 8

enum { EV_NONE, EV_KEY, EV_MOUSE };
typedef struct {
    uint8_t type;
    uint8_t pressed;   // key events
    uint8_t mods;
    uint8_t buttons;   // mouse
    uint8_t abs;       // mouse: dx/dy are absolute 0..65535 coordinates
    int key;           // KEY_* or ASCII
    int dx, dy, dz;
} event_t;

void ps2_init(void);
bool event_pop(event_t *ev);
bool ps2_has_mouse_wheel(void);
bool ps2_has_mouse(void);
bool ps2_absolute_pointer(void);

// ---------------------------------------------------------------- time
typedef struct { int year, month, day, hour, minute, second, weekday; } datetime_t;
void rtc_init(void);
int64_t time_now(void);  // unix seconds (UTC from RTC)
void time_to_dt(int64_t t, datetime_t *dt);
int64_t dt_to_time(const datetime_t *dt);
extern int g_tz_offset_min;  // user time zone offset
extern bool g_clock_24h;
void time_local(datetime_t *dt);
extern const char *g_month_names[12];
extern const char *g_day_names[7];

// ---------------------------------------------------------------- PCI
typedef struct {
    uint8_t bus, dev, func, irq;
    uint16_t vendor, device;
    uint8_t cls, subcls, progif, rev;
    uint64_t bar[6];
    bool bar_io[6];
} pci_dev_t;
extern pci_dev_t g_pci[64];
extern int g_pci_count;
void pci_init(void);
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off);
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v);
void pci_enable_bus_master(pci_dev_t *p);
const char *pci_class_name(uint8_t cls, uint8_t sub);
const char *pci_vendor_name(uint16_t vendor);
void pci_device_name(const pci_dev_t *p, char *buf, size_t n);

// ---------------------------------------------------------------- storage
enum { DRIVE_HDD, DRIVE_SSD, DRIVE_NVME, DRIVE_OPTICAL };
typedef struct {
    char model[48];
    char serial[24];
    char bus[16];      // "IDE", "SATA (AHCI)", "NVMe"
    int kind;
    uint64_t size_bytes;
    bool media_present;
} drive_t;
extern drive_t g_drives[16];
extern int g_drive_count;
void storage_init(void);

// ---------------------------------------------------------------- ACPI / firmware / power
void acpi_init(uint64_t rsdp_phys);
void smbios_init(uint64_t entry32, uint64_t entry64);
__attribute__((noreturn)) void power_shutdown(void);
__attribute__((noreturn)) void power_reboot(void);
extern uint64_t g_smbios_ram_bytes;
extern char g_sys_vendor[64], g_sys_product[64], g_bios_vendor[64], g_bios_version[64], g_bios_date[32];
extern uint8_t g_acpi_century_reg;
extern char g_acpi_oem[8];
extern int g_acpi_revision;

// ---------------------------------------------------------------- boot info
typedef struct {
    char bootloader_name[32];
    char bootloader_version[32];
    bool uefi;
    uint64_t kernel_phys, kernel_virt, kernel_size;
    char cmdline[128];
} boot_info_t;
extern boot_info_t g_boot;

// ---------------------------------------------------------------- framebuffer
typedef struct {
    void *addr;
    uint32_t width, height, pitch, bpp;
    uint8_t rshift, gshift, bshift;
    uint64_t phys;
} fb_info_t;
extern fb_info_t g_fb;

// ---------------------------------------------------------------- RAM filesystem
#define FS_NAME_MAX 48
typedef struct fs_node {
    char name[FS_NAME_MAX];
    bool dir;
    char *data;
    size_t size, cap;
    void (*gen)(char *buf, size_t n);  // dynamic file content (e.g. /proc)
    struct fs_node *parent, *child, *next;
    int64_t mtime;
} fs_node_t;
void fs_init(void);
fs_node_t *fs_root(void);
fs_node_t *fs_resolve(fs_node_t *cwd, const char *path);
fs_node_t *fs_create(fs_node_t *cwd, const char *path, bool dir, int *err);
int fs_remove(fs_node_t *node);
int fs_write(fs_node_t *f, const char *data, size_t n, bool append);
void fs_path(fs_node_t *n, char *buf, size_t len);
uint64_t fs_used_bytes(void);
#define FS_CAPACITY (32ULL * 1024 * 1024)
