// 7Kern - kernel entry point and boot sequence for 7OS
#define LIMINE_API_REVISION 2
#include "limine.h"
#include "kernel.h"
#include "wm.h"

// ---------------------------------------------------------------- Limine boot protocol requests
__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);

#define REQ __attribute__((used, section(".limine_requests"))) static volatile
REQ struct limine_framebuffer_request fb_req = {.id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0};
REQ struct limine_memmap_request mm_req = {.id = LIMINE_MEMMAP_REQUEST, .revision = 0};
REQ struct limine_hhdm_request hhdm_req = {.id = LIMINE_HHDM_REQUEST, .revision = 0};
REQ struct limine_bootloader_info_request bl_req = {.id = LIMINE_BOOTLOADER_INFO_REQUEST, .revision = 0};
REQ struct limine_firmware_type_request fw_req = {.id = LIMINE_FIRMWARE_TYPE_REQUEST, .revision = 0};
REQ struct limine_executable_address_request ka_req = {.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST, .revision = 0};
REQ struct limine_executable_file_request kf_req = {.id = LIMINE_EXECUTABLE_FILE_REQUEST, .revision = 0};
REQ struct limine_rsdp_request rsdp_req = {.id = LIMINE_RSDP_REQUEST, .revision = 0};
REQ struct limine_smbios_request smbios_req = {.id = LIMINE_SMBIOS_REQUEST, .revision = 0};
REQ struct limine_stack_size_request stack_req = {.id = LIMINE_STACK_SIZE_REQUEST, .revision = 0, .stack_size = 512 * 1024};

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

fb_info_t g_fb;
boot_info_t g_boot;

// ---------------------------------------------------------------- panic screen
static const char *exc_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint", "Overflow", "Bound range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point exception", "Alignment check", "Machine check", "SIMD floating-point exception",
    "Virtualization exception", "Control protection exception", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved"};

static bool in_panic;

static void panic_screen(const char *msg, regs_t *r) {
    cli();
    serial_write("\n*** 7Kern PANIC: ");
    serial_write(msg);
    serial_write(" ***\n");
    if (!g_fb.addr || g_fb.bpp != 32 || in_panic) { for (;;) hlt(); }
    in_panic = true;
    canvas_t c = {(uint32_t *)g_fb.addr, (int)g_fb.width, (int)g_fb.height, (int)(g_fb.pitch / 4), {0, 0, (int)g_fb.width, (int)g_fb.height}};
    // colours are written in the framebuffer's native layout below (BGRX on almost all hardware)
    gfx_fill(&c, 0, 0, c.w, c.h, 0x12141C);
    int x = imax(40, c.w / 2 - 380), y = imax(40, c.h / 2 - 240);
    gfx_round_rect(&c, x, y, 64, 64, 14, 0xE5484D, 255);
    gfx_text(&c, &font_large, x + (64 - gfx_text_width(&font_large, "!")) / 2, y + 12, "!", 0xFFFFFF);
    gfx_text(&c, &font_title, x + 84, y + 4, "7Kern stopped unexpectedly", 0xFFFFFF);
    gfx_text(&c, &font_ui, x + 84, y + 38, "A problem was detected and 7OS was halted to protect your computer.", 0xA6ABB5);
    y += 100;
    gfx_text(&c, &font_ui_bold, x, y, "Error", 0x8A90A0);
    gfx_text_fit(&c, &font_ui, x + 90, y, msg, 680, 0xFF8A8D);
    y += 36;
    if (r) {
        char line[160];
        const char *names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8 ", "R9 ", "R10", "R11", "R12", "R13", "R14", "R15"};
        uint64_t v[] = {r->rax, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi, r->rbp, r->rsp, r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15};
        ksnprintf(line, sizeof line, "RIP %016lx   CS %04lx   RFLAGS %016lx   ERR %016lx", r->rip, r->cs, r->rflags, r->error);
        gfx_text(&c, &font_mono_small, x, y, line, 0xE6E8EE); y += 20;
        ksnprintf(line, sizeof line, "CR2 %016lx   CR3 %016lx   VECTOR %lu", read_cr2(), read_cr3(), r->vector);
        gfx_text(&c, &font_mono_small, x, y, line, 0xE6E8EE); y += 28;
        for (int i = 0; i < 16; i += 2) {
            ksnprintf(line, sizeof line, "%s %016lx    %s %016lx", names[i], v[i], names[i + 1], v[i + 1]);
            gfx_text(&c, &font_mono_small, x, y, line, 0xA6ABB5);
            y += 18;
        }
        y += 14;
    }
    gfx_text(&c, &font_ui, x, y + 10, "Press any key to restart your computer.", 0xFFFFFF);
    gfx_text(&c, &font_small, x, y + 36, KERNEL_NAME " " KERNEL_VERSION " (" KERNEL_ARCH ")  " G_BULLET "  " OS_NAME " " OS_VERSION, 0x6C717D);
    // poll the keyboard controller directly (interrupts are off)
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t st = inb(0x64), sc = inb(0x60);
            if (!(st & 0x20) && sc < 0x80) power_reboot();
        }
    }
}

void panic(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    panic_screen(msg, NULL);
    for (;;) hlt();
}

void panic_exception(regs_t *r) {
    char msg[200];
    if (r->vector == 14)
        ksnprintf(msg, sizeof msg, "%s at %p (address %p, %s, %s)", exc_names[14], (void *)r->rip, (void *)read_cr2(),
                  (r->error & 2) ? "write" : "read", (r->error & 1) ? "protection violation" : "page not present");
    else
        ksnprintf(msg, sizeof msg, "%s at %p", exc_names[r->vector & 31], (void *)r->rip);
    panic_screen(msg, r);
}

// ---------------------------------------------------------------- boot
static void read_boot_info(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) { serial_write("Limine base revision 3 not supported\n"); for (;;) hlt(); }
    if (!hhdm_req.response || !mm_req.response) { serial_write("missing HHDM/memory map\n"); for (;;) hlt(); }
    g_hhdm = hhdm_req.response->offset;

    struct limine_memmap_response *mm = mm_req.response;
    g_memmap_count = 0;
    for (uint64_t i = 0; i < mm->entry_count && g_memmap_count < (int)ARRAY_LEN(g_memmap); i++) {
        g_memmap[g_memmap_count].base = mm->entries[i]->base;
        g_memmap[g_memmap_count].length = mm->entries[i]->length;
        g_memmap[g_memmap_count].type = mm->entries[i]->type;
        g_memmap_count++;
    }
    if (fb_req.response && fb_req.response->framebuffer_count) {
        struct limine_framebuffer *f = fb_req.response->framebuffers[0];
        g_fb.addr = f->address;
        g_fb.width = f->width;
        g_fb.height = f->height;
        g_fb.pitch = f->pitch;
        g_fb.bpp = f->bpp;
        g_fb.rshift = f->red_mask_shift;
        g_fb.gshift = f->green_mask_shift;
        g_fb.bshift = f->blue_mask_shift;
        g_fb.phys = (uint64_t)f->address - g_hhdm;
    }
    if (bl_req.response) {
        strlcpy(g_boot.bootloader_name, bl_req.response->name, sizeof g_boot.bootloader_name);
        strlcpy(g_boot.bootloader_version, bl_req.response->version, sizeof g_boot.bootloader_version);
    }
    if (fw_req.response) g_boot.uefi = fw_req.response->firmware_type != LIMINE_FIRMWARE_TYPE_X86BIOS;
    if (ka_req.response) {
        g_boot.kernel_phys = ka_req.response->physical_base;
        g_boot.kernel_virt = ka_req.response->virtual_base;
    }
    if (kf_req.response && kf_req.response->executable_file) {
        g_boot.kernel_size = kf_req.response->executable_file->size;
        if (kf_req.response->executable_file->cmdline)
            strlcpy(g_boot.cmdline, kf_req.response->executable_file->cmdline, sizeof g_boot.cmdline);
    }
}

void kmain(void) {
    serial_init();
    read_boot_info();
    klog("%s %s (%s) booting %s %s", KERNEL_NAME, KERNEL_VERSION, KERNEL_ARCH, OS_NAME, OS_VERSION);
    klog("boot: %s %s, %s firmware, kernel at %p (phys %p)", g_boot.bootloader_name, g_boot.bootloader_version,
         g_boot.uefi ? "UEFI" : "BIOS", (void *)g_boot.kernel_virt, (void *)g_boot.kernel_phys);
    if (!g_fb.addr) panic("no framebuffer available");
    klog("fb: %ux%u %u bpp, pitch %u", g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch);

    gdt_init();
    idt_init();
    pic_init();
    pmm_init();
    pit_init(200);
    sti();

    splash_init();
    splash_progress(8, "Starting 7Kern" G_ELLIPSIS);
    cpu_detect();
    klog("cpu: %s (%s), %u logical CPUs, %lu MHz", g_cpu.brand, g_cpu.vendor, g_cpu.logical_cpus, g_cpu.tsc_hz / 1000000);
    splash_progress(20, "Reading firmware tables" G_ELLIPSIS);
    acpi_init(rsdp_req.response ? rsdp_req.response->address : 0);
    smbios_init(smbios_req.response ? smbios_req.response->entry_32 : 0, smbios_req.response ? smbios_req.response->entry_64 : 0);
    rtc_init();
    splash_progress(35, "Scanning devices" G_ELLIPSIS);
    pci_init();
    splash_progress(50, "Detecting storage" G_ELLIPSIS);
    storage_init();
    splash_progress(65, "Starting input devices" G_ELLIPSIS);
    ps2_init();
    fs_init();
    splash_progress(80, "Preparing your desktop" G_ELLIPSIS);
    wm_init();
    splash_progress(100, "Welcome");
    klog("boot complete in %lu ms, %lu KiB heap in use", timer_ms(), heap_used_bytes() >> 10);
    splash_finish();
    wm_run();
}
