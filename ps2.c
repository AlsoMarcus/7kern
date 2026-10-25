// 7Kern - PS/2 keyboard and mouse driver (8042 controller)
#include "kernel.h"

#define PS2_DATA 0x60
#define PS2_STAT 0x64
#define PS2_CMD  0x64

// ---------------------------------------------------------------- event queue
#define EVQ 512
static event_t evq[EVQ];
static volatile uint32_t ev_head, ev_tail;

static void ev_push(const event_t *e) {
    // merge consecutive mouse motion with identical button state
    if (e->type == EV_MOUSE && ev_head != ev_tail) {
        event_t *last = &evq[(ev_head - 1) % EVQ];
        if (last->type == EV_MOUSE && last->buttons == e->buttons && last->abs == e->abs) {
            if (e->abs) { last->dx = e->dx; last->dy = e->dy; }
            else { last->dx += e->dx; last->dy += e->dy; }
            last->dz += e->dz;
            return;
        }
    }
    if (ev_head - ev_tail >= EVQ) return;  // full, drop
    evq[ev_head % EVQ] = *e;
    ev_head++;
}

bool event_pop(event_t *e) {
    uint64_t f = irq_save();
    bool ok = ev_tail != ev_head;
    if (ok) { *e = evq[ev_tail % EVQ]; ev_tail++; }
    irq_restore(f);
    return ok;
}

// ---------------------------------------------------------------- controller helpers
static bool wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STAT) & 2)) return true;
    return false;
}
static bool wait_read(void) {
    for (int i = 0; i < 100000; i++) { if (inb(PS2_STAT) & 1) return true; io_wait(); }
    return false;
}
static void ctl_cmd(uint8_t c) { wait_write(); outb(PS2_CMD, c); }
static void ctl_data(uint8_t d) { wait_write(); outb(PS2_DATA, d); }
static int read_byte(void) { return wait_read() ? inb(PS2_DATA) : -1; }

static int kbd_send(uint8_t b) {
    for (int tries = 0; tries < 3; tries++) {
        ctl_data(b);
        int r = read_byte();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}
static int mouse_send(uint8_t b) {
    for (int tries = 0; tries < 3; tries++) {
        ctl_cmd(0xD4);
        ctl_data(b);
        int r = read_byte();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

// ---------------------------------------------------------------- keyboard (scancode set 1)
static const uint8_t kmap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0};
static const uint8_t kmap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0};

static bool e0, shift_l, shift_r, ctrl, alt, caps, numlock, super;

static uint8_t mods(void) {
    return ((shift_l || shift_r) ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0) | (super ? MOD_SUPER : 0);
}

static void key_event(int key, bool pressed) {
    event_t e = {0};
    e.type = EV_KEY;
    e.key = key;
    e.pressed = pressed;
    e.mods = mods();
    ev_push(&e);
}

static void kbd_byte(uint8_t sc) {
    if (sc == 0xE0) { e0 = true; return; }
    if (sc == 0xE1) return;  // pause key sequence, ignore
    bool rel = sc & 0x80;
    uint8_t code = sc & 0x7F;
    if (e0) {
        e0 = false;
        int k = 0;
        switch (code) {
        case 0x2A: case 0x37: return;  // fake shift / print screen
        case 0x48: k = KEY_UP; break;
        case 0x50: k = KEY_DOWN; break;
        case 0x4B: k = KEY_LEFT; break;
        case 0x4D: k = KEY_RIGHT; break;
        case 0x47: k = KEY_HOME; break;
        case 0x4F: k = KEY_END; break;
        case 0x49: k = KEY_PGUP; break;
        case 0x51: k = KEY_PGDN; break;
        case 0x52: k = KEY_INSERT; break;
        case 0x53: k = KEY_DELETE; break;
        case 0x1C: k = '\n'; break;
        case 0x35: k = '/'; break;
        case 0x1D: ctrl = !rel; return;
        case 0x38: alt = !rel; return;
        case 0x5B: case 0x5C: super = !rel; k = KEY_SUPER; break;
        default: return;
        }
        key_event(k, !rel);
        return;
    }
    switch (code) {
    case 0x2A: shift_l = !rel; return;
    case 0x36: shift_r = !rel; return;
    case 0x1D: ctrl = !rel; return;
    case 0x38: alt = !rel; return;
    case 0x3A: if (!rel) caps = !caps; return;
    case 0x45: if (!rel) numlock = !numlock; return;
    case 0x46: return;
    }
    int k = 0;
    if (code >= 0x3B && code <= 0x44) k = KEY_F1 + (code - 0x3B);
    else if (code == 0x57) k = KEY_F11;
    else if (code == 0x58) k = KEY_F12;
    else if (code >= 0x47 && code <= 0x53) {
        static const int nav[] = {KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, 0, KEY_RIGHT, '+',
                                  KEY_END, KEY_DOWN, KEY_PGDN, KEY_INSERT, KEY_DELETE};
        static const char num[] = "789-456+1230.";
        k = (numlock || code == 0x4A || code == 0x4E) ? num[code - 0x47] : nav[code - 0x47];
        if (!k) return;
    } else if (code < 128) {
        bool sh = shift_l || shift_r;
        uint8_t c = sh ? kmap_shift[code] : kmap[code];
        if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
        if (c == 27) k = KEY_ESC;
        else k = c;
    }
    if (k) key_event(k, !rel);
}

// ---------------------------------------------------------------- VMware / QEMU absolute pointer ("vmmouse")
#define VMW_MAGIC 0x564D5868u
#define VMW_PORT  0x5658
static bool vmmouse;
static uint8_t vm_buttons;

static void vmw(uint32_t cmd, uint32_t arg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t si, di;
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d), "=S"(si), "=D"(di)
                     : "a"(VMW_MAGIC), "b"(arg), "c"(cmd), "d"(VMW_PORT)
                     : "memory");
    (void)si; (void)di;
}

static bool vmmouse_enable(void) {
    uint32_t a, b, c, d;
    vmw(41, 0x45414552, &a, &b, &c, &d);  // COMMAND: enable
    vmw(40, 0, &a, &b, &c, &d);           // STATUS
    if ((a & 0xFFFF) == 0) return false;
    vmw(39, 1, &a, &b, &c, &d);           // DATA: version id
    if (a != 0x3442554A) return false;
    vmw(41, 0x53424152, &a, &b, &c, &d);  // COMMAND: request absolute
    return true;
}

static void vmmouse_init(void) {
    if (!g_cpu.hypervisor) return;  // the backdoor only exists in virtual machines
    uint32_t a, b, c, d;
    vmw(10, 0, &a, &b, &c, &d);  // GETVERSION
    if (b != VMW_MAGIC) return;
    vmmouse = vmmouse_enable();
}

static void vmmouse_poll(void) {
    for (int k = 0; k < 16; k++) {
        uint32_t a, b, c, d;
        vmw(40, 0, &a, &b, &c, &d);
        if (a == 0xFFFF0000) { vmmouse = vmmouse_enable(); return; }
        if ((a & 0xFFFF) < 4) return;
        vmw(39, 4, &a, &b, &c, &d);
        if (a & 0x00010000) continue;  // relative packet: ignore, PS/2 handles those
        vm_buttons = ((a & 0x20) ? 1 : 0) | ((a & 0x10) ? 2 : 0) | ((a & 0x08) ? 4 : 0);
        event_t e = {0};
        e.type = EV_MOUSE;
        e.abs = 1;
        e.dx = b & 0xFFFF;
        e.dy = c & 0xFFFF;
        e.dz = (int)(int8_t)(d & 0xFF);  // same sign convention as PS/2 (positive = down)
        e.buttons = vm_buttons;
        ev_push(&e);
    }
}
bool ps2_absolute_pointer(void) { return vmmouse; }

// ---------------------------------------------------------------- mouse
static bool has_mouse, has_wheel;
static uint8_t mpkt[4];
static int mcycle;

static void mouse_byte(uint8_t b) {
    if (mcycle == 0 && !(b & 0x08)) return;  // out of sync
    mpkt[mcycle++] = b;
    int need = has_wheel ? 4 : 3;
    if (mcycle < need) return;
    mcycle = 0;
    uint8_t fl = mpkt[0];
    if (fl & 0xC0) return;  // overflow
    event_t e = {0};
    e.type = EV_MOUSE;
    e.dx = (int)mpkt[1] - ((fl & 0x10) ? 256 : 0);
    e.dy = -((int)mpkt[2] - ((fl & 0x20) ? 256 : 0));
    if (has_wheel) {
        int z = mpkt[3] & 0x0F;
        if (z & 0x08) z -= 16;
        e.dz = z;
    }
    e.buttons = fl & 0x07;
    if (vmmouse) {
        // with the absolute pointer active, PS/2 packets only signal that new data is queued
        e.buttons = vm_buttons;
        if (e.dx || e.dy || e.dz) ev_push(&e);
        vmmouse_poll();
        return;
    }
    ev_push(&e);
}

static void ps2_irq(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(PS2_STAT);
        if (!(st & 1)) break;
        uint8_t b = inb(PS2_DATA);
        if (st & 0x20) mouse_byte(b);
        else kbd_byte(b);
    }
}

void ps2_init(void) {
    ctl_cmd(0xAD);
    ctl_cmd(0xA7);
    for (int i = 0; i < 64 && (inb(PS2_STAT) & 1); i++) inb(PS2_DATA);
    ctl_cmd(0x20);
    int cfg = read_byte();
    if (cfg < 0) cfg = 0x47;
    cfg |= 0x43;     // IRQ1, IRQ12, translation
    cfg &= ~0x30;    // enable both clocks
    ctl_cmd(0x60);
    ctl_data(cfg);
    ctl_cmd(0xAE);
    ctl_cmd(0xA8);

    if (kbd_send(0xF4) != 0) klog("ps2: keyboard did not ack enable");

    if (mouse_send(0xF6) == 0) {
        has_mouse = true;
        // IntelliMouse wheel detection: sample rates 200, 100, 80 then read ID
        mouse_send(0xF3); mouse_send(200);
        mouse_send(0xF3); mouse_send(100);
        mouse_send(0xF3); mouse_send(80);
        mouse_send(0xF2);
        int id = read_byte();
        has_wheel = (id == 3 || id == 4);
        mouse_send(0xF3); mouse_send(100);
        mouse_send(0xE8); mouse_send(3);  // 8 counts/mm
        mouse_send(0xF4);
    }
    for (int i = 0; i < 64 && (inb(PS2_STAT) & 1); i++) inb(PS2_DATA);
    if (has_mouse) vmmouse_init();
    klog("ps2: keyboard ready, mouse %s%s%s", has_mouse ? "ready" : "not found", has_wheel ? " (scroll wheel)" : "",
         vmmouse ? ", absolute pointer (vmmouse)" : "");
    irq_register(1, ps2_irq);
    irq_register(12, ps2_irq);
}
bool ps2_has_mouse_wheel(void) { return has_wheel; }
bool ps2_has_mouse(void) { return has_mouse; }
