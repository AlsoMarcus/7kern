// 7Kern - RAM filesystem with a small /proc
#include "kernel.h"

static fs_node_t *root;
static uint64_t used_bytes;

static fs_node_t *node_new(fs_node_t *parent, const char *name, bool dir) {
    fs_node_t *n = kcalloc(sizeof *n);
    strlcpy(n->name, name, sizeof n->name);
    n->dir = dir;
    n->parent = parent ? parent : n;
    n->mtime = time_now();
    if (parent) {
        // keep children sorted by name
        fs_node_t **pp = &parent->child;
        while (*pp && strcmp((*pp)->name, name) < 0) pp = &(*pp)->next;
        n->next = *pp;
        *pp = n;
    }
    return n;
}

fs_node_t *fs_root(void) { return root; }

static fs_node_t *find_child(fs_node_t *d, const char *name, size_t len) {
    for (fs_node_t *c = d->child; c; c = c->next)
        if (strlen(c->name) == len && !strncmp(c->name, name, len)) return c;
    return NULL;
}

fs_node_t *fs_resolve(fs_node_t *cwd, const char *path) {
    fs_node_t *n = (path[0] == '/') ? root : cwd;
    if (path[0] == '~') { n = fs_resolve(root, "/home/user"); path++; }
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *e = path;
        while (*e && *e != '/') e++;
        size_t len = e - path;
        if (len == 1 && path[0] == '.') {}
        else if (len == 2 && path[0] == '.' && path[1] == '.') n = n->parent;
        else {
            if (!n->dir) return NULL;
            n = find_child(n, path, len);
            if (!n) return NULL;
        }
        path = e;
    }
    return n;
}

// err: 1 exists, 2 parent missing, 3 bad name, 4 no space
fs_node_t *fs_create(fs_node_t *cwd, const char *path, bool dir, int *err) {
    char buf[256];
    strlcpy(buf, path, sizeof buf);
    int l = strlen(buf);
    while (l > 1 && buf[l - 1] == '/') buf[--l] = 0;
    char *slash = strrchr(buf, '/');
    fs_node_t *parent;
    const char *name;
    if (slash) {
        *slash = 0;
        parent = fs_resolve(cwd, slash == buf ? "/" : buf);
        name = slash + 1;
    } else {
        parent = cwd;
        name = buf;
    }
    if (!parent || !parent->dir) { if (err) *err = 2; return NULL; }
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= FS_NAME_MAX) { if (err) *err = 3; return NULL; }
    if (find_child(parent, name, strlen(name))) { if (err) *err = 1; return NULL; }
    if (err) *err = 0;
    return node_new(parent, name, dir);
}

int fs_remove(fs_node_t *n) {
    if (n == root || (n->dir && n->child)) return -1;
    fs_node_t *p = n->parent;
    fs_node_t **pp = &p->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    used_bytes -= n->cap;
    kfree(n->data);
    kfree(n);
    return 0;
}

int fs_write(fs_node_t *f, const char *data, size_t n, bool append) {
    if (f->dir || f->gen) return -1;
    size_t need = (append ? f->size : 0) + n + 1;
    if (need > f->cap) {
        size_t cap = ALIGN_UP(need, 64);
        if (used_bytes - f->cap + cap > FS_CAPACITY) return -2;
        char *nd = krealloc(f->data, cap);
        if (!nd) return -2;
        used_bytes += cap - f->cap;
        f->data = nd;
        f->cap = cap;
    }
    if (!append) f->size = 0;
    memcpy(f->data + f->size, data, n);
    f->size += n;
    f->data[f->size] = 0;
    f->mtime = time_now();
    return 0;
}

void fs_path(fs_node_t *n, char *buf, size_t len) {
    if (n == root) { strlcpy(buf, "/", len); return; }
    char tmp[256] = "";
    while (n != root) {
        char part[256];
        ksnprintf(part, sizeof part, "/%s%s", n->name, tmp);
        strlcpy(tmp, part, sizeof tmp);
        n = n->parent;
    }
    strlcpy(buf, tmp, len);
}

uint64_t fs_used_bytes(void) { return used_bytes; }

// ---------------------------------------------------------------- /proc generators
static void gen_version(char *b, size_t n) {
    ksnprintf(b, n, "%s version %s (%s) #1 %s %s\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_ARCH, OS_BUILD_DATE, OS_BUILD_TIME);
}
static void gen_meminfo(char *b, size_t n) {
    ksnprintf(b, n, "MemTotal:     %10lu kB\nMemFree:      %10lu kB\nMemUsed:      %10lu kB\nHeapTotal:    %10lu kB\nHeapUsed:     %10lu kB\n",
              g_mem.total_bytes >> 10, pmm_free_bytes() >> 10, pmm_used_bytes() >> 10, heap_total_bytes() >> 10, heap_used_bytes() >> 10);
}
static void gen_cpuinfo(char *b, size_t n) {
    ksnprintf(b, n, "vendor_id   : %s\nmodel name  : %s\ncpu family  : %u\nmodel       : %u\nstepping    : %u\ncpu MHz     : %lu\nlogical cpus: %u\nflags       :%s%s%s%s%s%s%s%s%s%s\n",
              g_cpu.vendor, g_cpu.brand, g_cpu.family, g_cpu.model, g_cpu.stepping, g_cpu.tsc_hz / 1000000, g_cpu.logical_cpus,
              g_cpu.lm ? " lm" : "", g_cpu.nx ? " nx" : "", g_cpu.sse ? " sse" : "", g_cpu.sse2 ? " sse2" : "", g_cpu.sse3 ? " sse3" : "",
              g_cpu.ssse3 ? " ssse3" : "", g_cpu.sse41 ? " sse4_1" : "", g_cpu.sse42 ? " sse4_2" : "", g_cpu.avx ? " avx" : "", g_cpu.avx2 ? " avx2" : "");
}
static void gen_uptime(char *b, size_t n) {
    uint64_t ms = timer_ms();
    ksnprintf(b, n, "%lu.%02lu\n", ms / 1000, (ms % 1000) / 10);
}
static void gen_dmesg(char *b, size_t n) {
    size_t len;
    const char *k = klog_buffer(&len);
    if (len >= n) k += len - (n - 1);
    strlcpy(b, k, n);
}

static void add_file(const char *path, const char *text) {
    int err;
    fs_node_t *f = fs_create(root, path, false, &err);
    if (f) fs_write(f, text, strlen(text), false);
}
static void add_gen(const char *path, void (*gen)(char *, size_t)) {
    fs_node_t *f = fs_create(root, path, false, NULL);
    if (f) f->gen = gen;
}

void fs_init(void) {
    root = node_new(NULL, "", true);
    const char *dirs[] = {"/bin", "/boot", "/etc", "/home", "/home/user", "/home/user/Documents",
                          "/home/user/Desktop", "/proc", "/tmp", "/var", "/var/log"};
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) fs_create(root, dirs[i], true, NULL);
    char buf[512];
    ksnprintf(buf, sizeof buf, "NAME=\"%s\"\nVERSION=\"%s (%s)\"\nID=7os\nPRETTY_NAME=\"%s %s\"\nKERNEL=\"%s %s\"\n",
              OS_NAME, OS_VERSION, OS_CODENAME, OS_NAME, OS_VERSION, KERNEL_NAME, KERNEL_VERSION);
    add_file("/etc/os-release", buf);
    add_file("/etc/hostname", "7os-pc\n");
    add_file("/etc/motd", "Welcome to 7OS! Type 'help' to see what you can do.\n");
    add_file("/home/user/readme.txt",
             "Welcome to 7OS!\n\n"
             "7OS is a small hobby operating system built on the 7Kern kernel.\n"
             "Everything you see - the kernel, the drivers, the window manager\n"
             "and these apps - was written from scratch.\n\n"
             "Try these commands:\n"
             "  fetch      show system information\n"
             "  ls, cd     look around the file system\n"
             "  calc 6*7   do some maths\n"
             "  settings   open the Settings app\n\n"
             "Files live in memory, so changes are lost when you restart.\n");
    add_file("/home/user/Documents/todo.txt", "- try out the terminal\n- change the wallpaper in Settings\n- have fun\n");
    add_file("/boot/7kern.info", "7Kern is loaded by the Limine bootloader using the Limine boot protocol.\n");
    add_gen("/proc/version", gen_version);
    add_gen("/proc/meminfo", gen_meminfo);
    add_gen("/proc/cpuinfo", gen_cpuinfo);
    add_gen("/proc/uptime", gen_uptime);
    add_gen("/var/log/kernel.log", gen_dmesg);
    const char *bins[] = {"calc", "cat", "cd", "clear", "date", "dmesg", "echo", "fetch", "free", "help",
                          "history", "hostname", "ls", "lsblk", "lspci", "mkdir", "pwd", "reboot", "rm",
                          "settings", "shutdown", "touch", "uname", "uptime", "whoami", "write"};
    for (size_t i = 0; i < ARRAY_LEN(bins); i++) {
        ksnprintf(buf, sizeof buf, "/bin/%s", bins[i]);
        add_file(buf, "#!7sh builtin\n");
    }
}
