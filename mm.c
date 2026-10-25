// 7Kern - physical memory manager, page mapping and kernel heap
#include "kernel.h"

uint64_t g_hhdm;
mem_stats_t g_mem;
memmap_entry_t g_memmap[128];
int g_memmap_count;

const char *memmap_type_name(uint32_t t) {
    static const char *names[] = {"Usable", "Reserved", "ACPI reclaimable", "ACPI NVS",
                                  "Bad memory", "Bootloader", "Kernel & modules", "Framebuffer"};
    return t < 8 ? names[t] : "Unknown";
}

// ---------------------------------------------------------------- PMM (bitmap, 1 bit per 4K page)
static uint64_t *bitmap;
static uint64_t total_pages, free_pages, usable_pages, search_hint;

static inline void bit_set(uint64_t p) { bitmap[p >> 6] |= 1ULL << (p & 63); }
static inline void bit_clr(uint64_t p) { bitmap[p >> 6] &= ~(1ULL << (p & 63)); }
static inline bool bit_get(uint64_t p) { return bitmap[p >> 6] & (1ULL << (p & 63)); }

void pmm_init(void) {
    uint64_t top = 0;
    for (int i = 0; i < g_memmap_count; i++) {
        memmap_entry_t *e = &g_memmap[i];
        switch (e->type) {
        case 0: g_mem.usable_bytes += e->length; break;
        case 5: g_mem.reclaim_bytes += e->length; break;
        case 6: g_mem.kernel_bytes += e->length; break;
        case 2: case 3: g_mem.reclaim_bytes += e->length; break;
        case 7: g_mem.fb_bytes += e->length; break;
        default: g_mem.reserved_bytes += e->length; break;
        }
        if (e->type == 0 && e->base + e->length > top) top = e->base + e->length;
    }
    g_mem.total_bytes = g_mem.usable_bytes + g_mem.reclaim_bytes + g_mem.kernel_bytes;
    total_pages = top / PAGE_SIZE;
    uint64_t bm_bytes = ALIGN_UP((total_pages + 63) / 64 * 8, PAGE_SIZE);

    // place the bitmap in the first usable region big enough
    uint64_t bm_phys = 0;
    for (int i = 0; i < g_memmap_count; i++) {
        memmap_entry_t *e = &g_memmap[i];
        if (e->type == 0 && e->length >= bm_bytes && e->base >= 0x100000) { bm_phys = e->base; break; }
    }
    if (!bm_phys) panic("PMM: no room for page bitmap");
    bitmap = P2V(bm_phys);
    memset(bitmap, 0xFF, bm_bytes);
    for (int i = 0; i < g_memmap_count; i++) {
        memmap_entry_t *e = &g_memmap[i];
        if (e->type != 0) continue;
        uint64_t s = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t n = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = s; p < n; p++) {
            if (p < 256) continue;  // keep the first 1 MiB untouched
            bit_clr(p); free_pages++; usable_pages++;
        }
    }
    for (uint64_t p = bm_phys / PAGE_SIZE; p < (bm_phys + bm_bytes) / PAGE_SIZE; p++) {
        if (!bit_get(p)) { bit_set(p); free_pages--; }
    }
    search_hint = 256;
    klog("pmm: %lu MiB usable, %lu MiB free, bitmap %lu KiB at %p", g_mem.usable_bytes >> 20,
         (uint64_t)((free_pages * PAGE_SIZE) >> 20), (uint64_t)(bm_bytes >> 10), (void *)bm_phys);
}

static uint64_t find_run(uint64_t from, uint64_t to, size_t n) {
    uint64_t run = 0, start = 0;
    for (uint64_t p = from; p < to; p++) {
        if ((p & 63) == 0 && run == 0 && p + 64 <= to && bitmap[p >> 6] == ~0ULL) { p += 63; continue; }
        if (bit_get(p)) { run = 0; continue; }
        if (run == 0) start = p;
        if (++run == n) return start;
    }
    return 0;
}

uint64_t pmm_alloc(size_t n) {
    if (n == 0) return 0;
    uint64_t f = irq_save();
    uint64_t s = find_run(search_hint, total_pages, n);
    if (!s) s = find_run(256, total_pages, n);
    if (s) {
        for (uint64_t p = s; p < s + n; p++) bit_set(p);
        free_pages -= n;
        search_hint = s + n;
    }
    irq_restore(f);
    if (!s) return 0;
    memset(P2V(s * PAGE_SIZE), 0, n * PAGE_SIZE);
    return s * PAGE_SIZE;
}

void pmm_free(uint64_t phys, size_t n) {
    uint64_t f = irq_save();
    uint64_t s = phys / PAGE_SIZE;
    for (uint64_t p = s; p < s + n; p++) if (bit_get(p)) { bit_clr(p); free_pages++; }
    if (s < search_hint) search_hint = s;
    irq_restore(f);
}
uint64_t pmm_free_bytes(void) { return free_pages * PAGE_SIZE; }
uint64_t pmm_used_bytes(void) { return g_mem.total_bytes - free_pages * PAGE_SIZE; }

// ---------------------------------------------------------------- page mapping (4-level)
#define PTE_P   1ULL
#define PTE_W   2ULL
#define PTE_PWT 8ULL
#define PTE_PCD 16ULL
#define PTE_PS  128ULL
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

static bool map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *table = P2V(read_cr3() & PTE_ADDR);
    int shifts[3] = {39, 30, 21};
    for (int l = 0; l < 3; l++) {
        int idx = (virt >> shifts[l]) & 511;
        uint64_t e = table[idx];
        if (!(e & PTE_P)) {
            uint64_t np = pmm_alloc(1);
            if (!np) return false;
            table[idx] = np | PTE_P | PTE_W;
            e = table[idx];
        } else if (e & PTE_PS) {
            return true;  // already covered by a large page
        }
        table = P2V(e & PTE_ADDR);
    }
    table[(virt >> 12) & 511] = (phys & PTE_ADDR) | flags | PTE_P;
    invlpg(virt);
    return true;
}

static void *map_range(uint64_t phys, size_t size, uint64_t flags) {
    uint64_t s = ALIGN_DOWN(phys, PAGE_SIZE), e = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = s; p < e; p += PAGE_SIZE) map_page(p + g_hhdm, p, flags);
    return P2V(phys);
}
void *vmm_map_mmio(uint64_t phys, size_t size) { return map_range(phys, size, PTE_W | PTE_PCD | PTE_PWT); }
void *vmm_map_phys(uint64_t phys, size_t size) { return map_range(phys, size, PTE_W); }

// ---------------------------------------------------------------- kernel heap
typedef struct block {
    size_t size;  // payload bytes
    struct block *next;
    uint32_t magic, free;
    uint64_t pad;
} block_t;
#define BLOCK_MAGIC 0x7A11C8EDu
#define HDR sizeof(block_t)
_Static_assert(sizeof(block_t) == 32, "heap header size");

static block_t *heap_head;
static uint64_t heap_total, heap_used;

static void heap_insert(block_t *b) {
    if (!heap_head || b < heap_head) { b->next = heap_head; heap_head = b; return; }
    block_t *p = heap_head;
    while (p->next && p->next < b) p = p->next;
    b->next = p->next;
    p->next = b;
}

static bool heap_grow(size_t need) {
    size_t bytes = ALIGN_UP(need + HDR, 4ULL << 20);
    uint64_t phys = pmm_alloc(bytes / PAGE_SIZE);
    if (!phys) {
        bytes = ALIGN_UP(need + HDR, PAGE_SIZE);
        phys = pmm_alloc(bytes / PAGE_SIZE);
        if (!phys) return false;
    }
    block_t *b = P2V(phys);
    b->size = bytes - HDR;
    b->magic = BLOCK_MAGIC;
    b->free = 1;
    heap_total += bytes;
    heap_insert(b);
    return true;
}

static inline bool adjacent(block_t *a, block_t *b) { return (uint8_t *)a + HDR + a->size == (uint8_t *)b; }

void *kmalloc(size_t n) {
    if (!n) n = 1;
    n = ALIGN_UP(n, 16);
    uint64_t f = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t *b = heap_head; b; b = b->next) {
            if (!b->free || b->size < n) continue;
            if (b->size >= n + HDR + 64) {
                block_t *r = (block_t *)((uint8_t *)b + HDR + n);
                r->size = b->size - n - HDR;
                r->magic = BLOCK_MAGIC;
                r->free = 1;
                r->next = b->next;
                b->next = r;
                b->size = n;
            }
            b->free = 0;
            heap_used += b->size + HDR;
            irq_restore(f);
            return (uint8_t *)b + HDR;
        }
        if (!heap_grow(n)) break;
    }
    irq_restore(f);
    klog("kmalloc: out of memory (%lu bytes)", (uint64_t)n);
    return NULL;
}

void *kcalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (b->magic != BLOCK_MAGIC || b->free) { klog("kfree: bad pointer %p", ptr); return; }
    uint64_t f = irq_save();
    b->free = 1;
    heap_used -= b->size + HDR;
    if (b->next && b->next->free && adjacent(b, b->next)) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
    }
    block_t *prev = NULL;
    for (block_t *p = heap_head; p && p != b; p = p->next) prev = p;
    if (prev && prev->free && adjacent(prev, b)) {
        prev->size += HDR + b->size;
        prev->next = b->next;
    }
    irq_restore(f);
}

void *krealloc(void *ptr, size_t n) {
    if (!ptr) return kmalloc(n);
    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (b->size >= n) return ptr;
    void *np = kmalloc(n);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

uint64_t heap_used_bytes(void) { return heap_used; }
uint64_t heap_total_bytes(void) { return heap_total; }
