// 7Kern - freestanding C library bits, serial port and kernel log
#include "kernel.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n)::"memory");
    return ret;
}
void *memset(void *d, int c, size_t n) {
    void *ret = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return ret;
}
void memset32(uint32_t *d, uint32_t v, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    if (dd == ss || n == 0) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    // overlapping, copy backwards
    dd += n - 1; ss += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n)::"memory");
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int strcasecmp_prefix(const char *s, const char *p) {
    while (*p) { if (tolower(*s) != tolower(*p)) return 0; s++; p++; }
    return 1;
}
int strcasestr_has(const char *h, const char *n) {
    if (!*n) return 1;
    for (; *h; h++) if (strcasecmp_prefix(h, n)) return 1;
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
void strlcpy(char *d, const char *s, size_t n) {
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
void strlcat(char *d, const char *s, size_t n) {
    size_t l = strlen(d);
    if (l < n) strlcpy(d + l, s, n - l);
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)r;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
long strtol(const char *s, const char **end, int base) {
    long v = 0; int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (end) *end = s;
    return neg ? -v : v;
}

// ---------------------------------------------------------------- printf
typedef struct { char *buf; size_t n, pos; } out_t;
static void put(out_t *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }

static void put_num(out_t *o, uint64_t v, int base, bool neg, int width, bool zero, bool left, bool upper) {
    char tmp[24]; int len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[len++] = digits[v % base]; v /= base; } while (v);
    int total = len + (neg ? 1 : 0);
    if (!left && !zero) for (int i = total; i < width; i++) put(o, ' ');
    if (neg) put(o, '-');
    if (!left && zero) for (int i = total; i < width; i++) put(o, '0');
    while (len) put(o, tmp[--len]);
    if (left) for (int i = total; i < width; i++) put(o, ' ');
}

int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    out_t o = {buf, n, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false;
        int width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0, width, zero, left, false);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_num(&o, v, 10, false, width, zero, left, false);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_num(&o, v, 16, false, width, zero, left, *fmt == 'X');
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            put(&o, '0'); put(&o, 'x');
            put_num(&o, v, 16, false, 16, true, false, false);
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len] && (prec < 0 || len < prec)) len++;
            if (!left) for (int i = len; i < width; i++) put(&o, ' ');
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) for (int i = len; i < width; i++) put(&o, ' ');
            break;
        }
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void fmt_size(char *buf, size_t n, uint64_t b) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    uint64_t whole = b, frac = 0;
    while (whole >= 1024 && u < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
    if (u == 0 || whole >= 100 || frac == 0) ksnprintf(buf, n, "%lu %s", whole, units[u]);
    else ksnprintf(buf, n, "%lu.%lu %s", whole, frac, units[u]);
}

// ---------------------------------------------------------------- serial (COM1)
#define COM1 0x3F8
static bool serial_ok;
void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);  // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  // 8N1
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    // loopback test so we don't spin on a missing port
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    serial_ok = inb(COM1 + 0) == 0xAE;
    outb(COM1 + 4, 0x0F);
}
static void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (uint8_t)c);
}
void serial_write(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}

// ---------------------------------------------------------------- kernel log
#define KLOG_SIZE 16384
static char klog_buf[KLOG_SIZE];
static size_t klog_len;

void klog(const char *fmt, ...) {
    char line[256], out[300];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    uint64_t ms = timer_ms();
    ksnprintf(out, sizeof out, "[%5lu.%03lu] %s\n", ms / 1000, ms % 1000, line);
    serial_write(out);
    uint64_t f = irq_save();
    size_t n = strlen(out);
    if (klog_len + n >= KLOG_SIZE) {  // drop oldest half
        size_t drop = KLOG_SIZE / 2;
        while (drop < klog_len && klog_buf[drop - 1] != '\n') drop++;
        memmove(klog_buf, klog_buf + drop, klog_len - drop);
        klog_len -= drop;
    }
    memcpy(klog_buf + klog_len, out, n);
    klog_len += n;
    klog_buf[klog_len] = 0;
    irq_restore(f);
}
const char *klog_buffer(size_t *len) { if (len) *len = klog_len; return klog_buf; }
