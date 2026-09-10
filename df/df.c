#include "../_sys/_main.h"

struct options {
    uint64_t block_size;
    int human;
    int decimal_human;
    int inodes;
    int types;
    int posix;
    int all;
};

struct mount_info {
    const char *source, *target;
    size_t source_len, target_len;
};

static int put(const char *s, size_t n)
{
    return write_all_fd(1, s, n);
}

static int spaces(size_t n)
{
    while (n--) if (put(" ", 1)) return 1;
    return 0;
}

static int put_u64(uint64_t v)
{
    char buf[24];
    char *p = buf + sizeof(buf);
    do { *--p = (char)('0' + v % 10); } while (v /= 10);
    return put(p, (size_t)(buf + sizeof(buf) - p));
}

static size_t digits_u64(uint64_t v)
{
    size_t n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static int put_human(uint64_t bytes, int decimal)
{
    static const char suffix[] = "KMGTPE";
    uint64_t base = decimal ? 1000 : 1024;
    uint64_t divisor = 1;
    uint64_t whole, tenth;
    int exp = 0;

    while (exp < 6 && bytes >= divisor * base) {
        divisor *= base;
        exp++;
    }
    if (!exp) return put_u64(bytes);

    whole = bytes / divisor;
    tenth = (bytes % divisor) * 10 / divisor;
    if (whole >= 10) {
        if (tenth >= 5) whole++;
        return put_u64(whole) || put(&suffix[exp - 1], 1);
    }
    if (put_u64(whole)) return 1;
    if (tenth) {
        if (put(".", 1) || put_u64(tenth)) return 1;
    }
    if (put(&suffix[exp - 1], 1)) return 1;
    return 0;
}

static int mount_matches(const char *path, const char *target, size_t len)
{
    if (len == 1 && target[0] == '/') return path[0] == '/';
    return path[0] == '/' && strlen(path) >= len &&
           memmem(path, len, target, len) == path &&
           (path[len] == '\0' || path[len] == '/');
}

static void find_mount(const char *path, const char *mounts,
                       struct mount_info *best)
{
    const char *p = mounts;
    const char *query = path[0] == '/' ? path : "/";
    size_t best_len = 0;

    best->source = path;
    best->source_len = strlen(path);
    best->target = path;
    best->target_len = best->source_len;
    if (!mounts) return;

    while (*p) {
        const char *line = p, *source, *target;
        size_t source_len, target_len;
        while (*p && *p != ' ' && *p != '\n') p++;
        source = line;
        source_len = (size_t)(p - source);
        if (*p != ' ') { while (*p && *p++ != '\n'); continue; }
        p++;
        target = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        target_len = (size_t)(p - target);
        if (mount_matches(query, target, target_len) && target_len >= best_len) {
            best->source = source;
            best->source_len = source_len;
            best->target = target;
            best->target_len = target_len;
            best_len = target_len;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static uint64_t parse_size(const char *s)
{
    uint64_t n = 0, multiplier = 1;
    char c;

    while (*s >= '0' && *s <= '9') n = n * 10 + (unsigned)(*s++ - '0');
    c = *s;
    if (c == 'K' || c == 'k') multiplier = 1024ULL;
    else if (c == 'M' || c == 'm') multiplier = 1024ULL * 1024;
    else if (c == 'G' || c == 'g') multiplier = 1024ULL * 1024 * 1024;
    else if (c == 'T' || c == 't') multiplier = 1024ULL * 1024 * 1024 * 1024;
    else if (c == 'P' || c == 'p') multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024;
    else if (c == 'E' || c == 'e') multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024;
    return n * multiplier;
}

static int is_block_size_option(const char *s)
{
    return s[0] == '-' && s[1] == '-' && s[2] == 'b' &&
           s[3] == 'l' && s[4] == 'o' && s[5] == 'c' && s[6] == 'k' &&
           s[7] == '-' && s[8] == 's' && s[9] == 'i' && s[10] == 'z' &&
           s[11] == 'e' && s[12] == '=';
}

static void parse_options(int argc, char **argv, struct options *o, int *first)
{
    int i;
    o->block_size = 1024;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == '-') {
            if (!a[2]) { i++; break; }
            if (is_block_size_option(a))
                o->block_size = parse_size(a + 13);
            else if (strcasecmp(a, "--block-size") == 0 && i + 1 < argc)
                o->block_size = parse_size(argv[++i]);
            else if (strcasecmp(a, "--human-readable") == 0) o->human = 1;
            else if (strcasecmp(a, "--si") == 0) o->decimal_human = 1;
            else if (strcasecmp(a, "--inodes") == 0) o->inodes = 1;
            else if (strcasecmp(a, "--print-type") == 0) o->types = 1;
            else if (strcasecmp(a, "--portability") == 0) o->posix = 1;
            else if (strcasecmp(a, "--all") == 0) o->all = 1;
            continue;
        }
        for (int j = 1; a[j]; j++) {
            if (a[j] == 'h') o->human = 1;
            else if (a[j] == 'a') o->all = 1;
            else if (a[j] == 'H') o->decimal_human = 1;
            else if (a[j] == 'i') o->inodes = 1;
            else if (a[j] == 'P') o->posix = 1;
            else if (a[j] == 'T') o->types = 1;
            else if (a[j] == 'k') o->block_size = 1024;
            else if (a[j] == 'm') o->block_size = 1024ULL * 1024;
            else if (a[j] == 'B') {
                const char *size = a[j + 1] ? a + j + 1 :
                    (i + 1 < argc ? argv[++i] : "1024");
                o->block_size = parse_size(size);
                break;
            }
        }
    }
    if (!o->block_size) o->block_size = 1024;
    *first = i;
}

static const char *type_name(long type)
{
    switch ((unsigned long)type) {
    case 0xEF53: return "ext";
    case 0x01021994: return "tmpfs";
    case 0x9FA0: return "proc";
    case 0x62656572: return "sysfs";
    case 0x794C7630: return "overlay";
    case 0x58465342: return "xfs";
    default: return "unknown";
    }
}

static int print_value(uint64_t blocks, uint64_t block_size,
                       const struct options *o)
{
    if (o->human || o->decimal_human)
        return put_human(blocks * block_size, o->decimal_human);
    return put_u64((blocks * block_size + o->block_size - 1) /
                   o->block_size);
}

static int print_count(uint64_t value, const struct options *o)
{
    return o->human || o->decimal_human ?
        put_human(value, o->decimal_human) : put_u64(value);
}

static size_t value_len(uint64_t blocks, uint64_t block_size,
                        const struct options *o)
{
    uint64_t value = blocks * block_size;
    if (!o->human && !o->decimal_human)
        return digits_u64((value + o->block_size - 1) / o->block_size);
    {
        uint64_t base = o->decimal_human ? 1000 : 1024;
        uint64_t divisor = 1, whole, tenth;
        int exp = 0;
        while (exp < 6 && value >= divisor * base) {
            divisor *= base;
            exp++;
        }
        if (!exp) return digits_u64(value);
        whole = value / divisor;
        tenth = (value % divisor) * 10 / divisor;
        return (whole >= 10 ? digits_u64(whole + (tenth >= 5)) :
                digits_u64(whole) + (tenth ? 2 : 0)) + 1;
    }
}

static int print_field(uint64_t blocks, uint64_t block_size,
                       const struct options *o, size_t width, size_t gap,
                       int inode)
{
    size_t len;
    if (inode) {
        len = (o->human || o->decimal_human) ?
            value_len(blocks, 1, o) : digits_u64(blocks);
        if (spaces(width > len ? width - len : 0) || print_count(blocks, o)) return 1;
    } else {
        len = value_len(blocks, block_size, o);
        if (spaces(width > len ? width - len : 0) ||
            print_value(blocks, block_size, o)) return 1;
    }
    return spaces(gap);
}

static int print_fs(const char *path, const struct statfs *s,
                    const struct options *o, const char *mounts,
                    size_t source_width)
{
    uint64_t total, used, avail;
    uint64_t bsize = s->f_frsize ? (uint64_t)s->f_frsize : (uint64_t)s->f_bsize;
    int pct;
    struct mount_info mount;

    find_mount(path, mounts, &mount);

    if (o->inodes) {
        total = s->f_files;
        used = total - s->f_ffree;
        avail = s->f_ffree;
    } else {
        total = s->f_blocks;
        used = total - s->f_bfree;
        avail = s->f_bavail;
    }
    pct = used + avail ? (int)((used * 100 + used + avail - 1) /
                               (used + avail)) : 0;

    if (put(mount.source, mount.source_len) ||
        spaces(source_width > mount.source_len ? source_width - mount.source_len : 1))
        return 1;
    if (o->types) {
        const char *type = type_name(s->f_type);
        size_t len = strlen(type);
        if (put(type, len) || spaces(8 > len ? 8 - len : 1)) return 1;
    }
    if (print_field(total, bsize, o, (o->human || o->decimal_human) ? 4 : 9,
                    (o->human || o->decimal_human) ? 3 : 1, o->inodes) ||
        print_field(used, bsize, o, (o->human || o->decimal_human) ? 4 : 8,
                    (o->human || o->decimal_human) ? 2 : 2, o->inodes) ||
        print_field(avail, bsize, o, (o->human || o->decimal_human) ? 4 : 8,
                    1, o->inodes) ||
        spaces(4 > digits_u64((uint64_t)pct) + 1 ?
               4 - digits_u64((uint64_t)pct) - 1 : 0) ||
        put_u64((uint64_t)pct) || put("% ", 2) ||
        put(mount.target, mount.target_len) ||
        put("\n", 1)) return 1;
    return 0;
}

static int next_mount(const char **cursor, struct mount_info *mount)
{
    const char *p = *cursor, *start;
    if (!p || !*p) return 0;
    start = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    mount->source = start;
    mount->source_len = (size_t)(p - start);
    if (*p != ' ') return 0;
    p++;
    start = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    mount->target = start;
    mount->target_len = (size_t)(p - start);
    while (*p && *p != '\n') p++;
    if (*p) p++;
    *cursor = p;
    return 1;
}

static size_t mount_source_width(const char *mounts, size_t minimum)
{
    struct mount_info mount;
    size_t width = minimum;
    while (next_mount(&mounts, &mount))
        if (mount.source_len + 1 > width) width = mount.source_len + 1;
    return width;
}

static int print_all_mounts(const char *mounts, const struct options *o,
                            size_t source_width)
{
    const char *all_mounts = mounts;
    struct mount_info mount;
    struct statfs s;
    struct { long type; int32_t fsid[2]; } seen[128];
    size_t nseen = 0;
    int status = 0;

    while (next_mount(&mounts, &mount)) {
        char path[4096];
        size_t len = mount.target_len;
        if (len >= sizeof(path)) continue;
        for (size_t i = 0; i < len; i++) path[i] = mount.target[i];
        path[len] = '\0';
        if (statfs(path, &s) < 0) { status = 1; continue; }
        if (!o->all && ((!o->inodes && !s.f_blocks) ||
                        (o->inodes && !s.f_files))) continue;
        if (!o->all) {
            int duplicate = 0;
            for (size_t i = 0; i < nseen; i++)
                if (seen[i].type == s.f_type && seen[i].fsid[0] == s.f_fsid[0] &&
                    seen[i].fsid[1] == s.f_fsid[1]) duplicate = 1;
            if (duplicate) continue;
            if (nseen < sizeof(seen) / sizeof(seen[0])) {
                seen[nseen].type = s.f_type;
                seen[nseen].fsid[0] = s.f_fsid[0];
                seen[nseen].fsid[1] = s.f_fsid[1];
                nseen++;
            }
        }
        if (print_fs(path, &s, o, all_mounts, source_width)) status = 1;
    }
    return status;
}

static int print_header(const struct options *o, size_t source_width)
{
    if (put("Filesystem", sizeof("Filesystem") - 1) ||
        spaces(source_width - sizeof("Filesystem") + 1)) return 1;
    if (o->inodes) {
        if (o->types && (put("Type", 4) || spaces(4))) return 1;
        return put(o->types ? "Inodes IUsed IFree IUse% Mounted on\n" :
                   "Inodes   IUsed   IFree IUse% Mounted on\n",
                   o->types ? sizeof("Inodes IUsed IFree IUse% Mounted on\n") - 1 :
                              sizeof("Inodes   IUsed   IFree IUse% Mounted on\n") - 1);
    }
    if (o->types && (put("Type", 4) || spaces(4))) return 1;
    if (o->posix)
        return put("1024-blocks Used Available Capacity Mounted on\n",
                   sizeof("1024-blocks Used Available Capacity Mounted on\n") - 1);
    if (o->human || o->decimal_human)
        return put("Size  Used Avail Use% Mounted on\n",
                   sizeof("Size  Used Avail Use% Mounted on\n") - 1);
    return put("1K-blocks     Used Available Use% Mounted on\n",
               sizeof("1K-blocks     Used Available Use% Mounted on\n") - 1);
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct options o;
    struct arena mounts;
    const char *mount_data = 0;
    size_t mount_used = 0;
    size_t source_width;
    int mount_fd;
    int first, status = 0;

    parse_options(argc, argv, &o, &first);
    mount_fd = open("/proc/self/mounts", O_RDONLY, 0);
    if (mount_fd >= 0 && !arena_init(&mounts, 16384) &&
        !arena_read_all(&mounts, mount_fd, &mount_used)) {
        ((char *)mounts.base)[mount_used] = '\0';
        mount_data = mounts.base;
    }
    if (mount_fd >= 0) close(mount_fd);
    source_width = first == argc ?
        mount_source_width(mount_data, (o.human || o.decimal_human) ? 35 : 15) :
        ((o.human || o.decimal_human) ? 16 : 15);
    if (print_header(&o, source_width))
        exit(1);
    if (first == argc) {
        if (mount_data)
            status = print_all_mounts(mount_data, &o, source_width);
        else {
            struct statfs s;
            if (statfs(".", &s) < 0 || print_fs(".", &s, &o, mount_data, 15)) status = 1;
        }
    } else {
        for (int i = first; i < argc; i++) {
            struct statfs s;
            if (statfs(argv[i], &s) < 0 || print_fs(argv[i], &s, &o, mount_data, 15)) status = 1;
        }
    }
    exit(status);
    __builtin_unreachable();
}
