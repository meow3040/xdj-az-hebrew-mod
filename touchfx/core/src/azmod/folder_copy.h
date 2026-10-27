/* SPDX-License-Identifier: GPL-3.0-only */
/* folder_copy.h: the plan of a FOLDER-view copy (folder_copy.c), as pure functions over a file
 * system the caller provides, so the host tests and the fuzz harness compile this file as is.
 *
 * Names come from a USB stick (EP147's folder rows, then readdir on the stick), so every name is
 * checked before it is joined into a path: no empty name, no "." or "..", no '/' or '\\', no
 * control characters. Nothing is ever overwritten:
 *   - a copied FOLDER whose name is taken on the destination becomes "Name (1)", "Name (2)", ...
 *     (never merged into the existing one);
 *   - a FILE whose name is taken is skipped when the existing one has the same size (already
 *     there), else it becomes "Name (1).ext", ...;
 *   - macOS and Windows litter ("._*", ".DS_Store", ".Trashes", ".Spotlight-V100", ".fseventsd",
 *     "System Volume Information") and our own unfinished "*.azcp" files are never copied.
 * The walk stops at FC_MAX_FILES files, FC_MAX_DEPTH levels and FC_MAX_ENTRIES entries read. */
#ifndef AZMOD_FOLDER_COPY_H
#define AZMOD_FOLDER_COPY_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FC_PATH_MAX 512
#define FC_NAME_MAX 255
#define FC_MAX_FILES 2000
#define FC_MAX_DEPTH 12
#define FC_MAX_ENTRIES 20000
#define FC_MAX_SUFFIX 99 /* "Name (99)" is the last name tried */

/* 1 when `nm` can be one path component. */
static inline int fc_name_ok(const char *nm) {
    if (!nm || !nm[0] || !strcmp(nm, ".") || !strcmp(nm, ".."))
        return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) nm; *p; p++, n++)
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\' || n >= FC_NAME_MAX)
            return 0;
    return 1;
}

/* 1 when an entry is never copied. */
static inline int fc_skip_name(const char *nm) {
    static const char *const junk[] = {".DS_Store", ".Trashes", ".Spotlight-V100", ".fseventsd",
                                       ".TemporaryItems", "System Volume Information"};
    if (!strncmp(nm, "._", 2))
        return 1;
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        if (!strcmp(nm, junk[i]))
            return 1;
    size_t n = strlen(nm);
    return n >= 5 && !strcmp(nm + n - 5, ".azcp");
}

/* out = a "/" b ("a" may be empty: then just b). 0, or -1 when it does not fit or b is not a
 * valid component. */
static inline int fc_join(char *out, size_t cap, const char *a, const char *b) {
    if (!fc_name_ok(b))
        return -1;
    int r = a && a[0] ? snprintf(out, cap, "%s/%s", a, b) : snprintf(out, cap, "%s", b);
    return r < 0 || (size_t) r >= cap ? -1 : 0;
}

/* The k-th alternative of `name` (k >= 1): "base (k).ext" for a file (the extension is the part
 * after the last '.', when that is not the first character), "name (k)" for a folder. */
static inline int fc_alt_name(char *out, size_t cap, const char *name, int k, int is_dir) {
    const char *dot = is_dir ? NULL : strrchr(name, '.');
    if (dot == name)
        dot = NULL;
    int r = dot ? snprintf(out, cap, "%.*s (%d)%s", (int) (dot - name), name, k, dot)
                : snprintf(out, cap, "%s (%d)", name, k);
    return r < 0 || (size_t) r >= cap || !fc_name_ok(out) ? -1 : 0;
}

/* The file system the plan reads, supplied by the caller (real or a test fake). */
struct fc_fs {
    void *ctx;
    /* what is at `path`: 0 nothing, 1 a file (size in *size), 2 a folder */
    int (*stat)(void *ctx, const char *path, uint64_t *size);
    /* calls `each` once per entry name of folder `path` (no "." / ".."); 0, or -1 on error.
     * Stops early when `each` returns nonzero. */
    int (*list)(void *ctx, const char *path, int (*each)(void *arg, const char *name), void *arg);
};

/* Where `name` (a file of `size` bytes, or a folder) lands in destination folder `dir`:
 * FC_PUT with the path in out, FC_SKIP (a file of that name and size is there), or -1. */
enum {
    FC_PUT = 1,
    FC_SKIP = 2
};
static inline int fc_dest(const struct fc_fs *fs, const char *dir, const char *name, int is_dir,
                          uint64_t size, char *out, size_t cap) {
    char cand[FC_PATH_MAX], alt[FC_NAME_MAX + 16];
    for (int k = 0; k <= FC_MAX_SUFFIX; k++) {
        const char *nm = name;
        if (k) {
            if (fc_alt_name(alt, sizeof alt, name, k, is_dir) < 0)
                return -1;
            nm = alt;
        }
        if (fc_join(cand, sizeof cand, dir, nm) < 0)
            return -1;
        uint64_t have = 0;
        int st = fs->stat(fs->ctx, cand, &have);
        if (st == 0) {
            snprintf(out, cap, "%s", cand);
            return FC_PUT;
        }
        if (!is_dir && st == 1 && have == size && k == 0)
            return FC_SKIP;
    }
    return -1;
}

/* One file of the plan: source and destination paths, relative to their mounts. */
struct fc_item {
    char src[FC_PATH_MAX], dst[FC_PATH_MAX];
    uint64_t size;
    int is_dir; /* a folder to create (listed before anything inside it) */
};
struct fc_plan {
    struct fc_item *items;
    int n, cap;      /* items used / allocated by the caller */
    uint64_t bytes;  /* total bytes of the files */
    int files;       /* files in the plan */
    int skipped;     /* files already there (same name and size) */
    int entries;     /* entries read */
    int err;         /* 0, or why the walk stopped (FC_E_*) */
};
enum {
    FC_E_NAME = 1,
    FC_E_FILES,
    FC_E_DEPTH,
    FC_E_ENTRIES,
    FC_E_LIST,
    FC_E_FULL,
    FC_E_PATH
};

struct fc_walk_arg {
    const struct fc_fs *fs;
    struct fc_plan *p;
    const char *src, *dst;
    int depth;
};
static int fc_walk_dir(const struct fc_fs *fs, struct fc_plan *p, const char *src, const char *dst,
                       int depth);
static inline int fc_walk_each(void *arg, const char *name) {
    struct fc_walk_arg *a = (struct fc_walk_arg *) arg;
    struct fc_plan *p = a->p;
    if (++p->entries > FC_MAX_ENTRIES) {
        p->err = FC_E_ENTRIES;
        return 1;
    }
    if (fc_skip_name(name))
        return 0;
    char s[FC_PATH_MAX], d[FC_PATH_MAX];
    if (fc_join(s, sizeof s, a->src, name) < 0 || fc_join(d, sizeof d, a->dst, name) < 0) {
        p->err = FC_E_NAME;
        return 1;
    }
    uint64_t size = 0;
    int st = a->fs->stat(a->fs->ctx, s, &size);
    if (st == 2)
        return fc_walk_dir(a->fs, p, s, d, a->depth + 1) ? 1 : 0;
    if (st != 1)
        return 0; /* gone meanwhile, or neither file nor folder: not copied */
    if (p->files >= FC_MAX_FILES) {
        p->err = FC_E_FILES;
        return 1;
    }
    if (p->n >= p->cap) {
        p->err = FC_E_FULL;
        return 1;
    }
    struct fc_item *it = &p->items[p->n++];
    snprintf(it->src, sizeof it->src, "%s", s);
    snprintf(it->dst, sizeof it->dst, "%s", d);
    it->size = size;
    it->is_dir = 0;
    p->files++;
    p->bytes += size;
    return 0;
}
/* the folder `dst` itself, then everything under `src` into it */
static int fc_walk_dir(const struct fc_fs *fs, struct fc_plan *p, const char *src, const char *dst,
                       int depth) {
    if (depth > FC_MAX_DEPTH) {
        p->err = FC_E_DEPTH;
        return -1;
    }
    if (p->n >= p->cap) {
        p->err = FC_E_FULL;
        return -1;
    }
    struct fc_item *it = &p->items[p->n++];
    snprintf(it->src, sizeof it->src, "%s", src);
    snprintf(it->dst, sizeof it->dst, "%s", dst);
    it->size = 0;
    it->is_dir = 1;
    struct fc_walk_arg a = {fs, p, src, dst, depth};
    if (fs->list(fs->ctx, src, fc_walk_each, &a) < 0 && !p->err)
        p->err = FC_E_LIST;
    return p->err ? -1 : 0;
}

/* The plan for copying `src` (a file or folder, relative to the source mount; fs_src reads
 * the source) into folder `dst_dir` (relative to the destination mount; fs_dst reads it; ""
 * = its top). 0 with p filled, or -1 with p->err set. */
static inline int fc_plan_make(const struct fc_fs *fs_src, const struct fc_fs *fs_dst,
                               const char *src, const char *dst_dir, struct fc_plan *p) {
    p->n = p->files = p->skipped = p->entries = p->err = 0;
    p->bytes = 0;
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    uint64_t size = 0;
    int st = fs_src->stat(fs_src->ctx, src, &size);
    if (!fc_name_ok(base) || fc_skip_name(base) || (st != 1 && st != 2)) {
        p->err = FC_E_NAME;
        return -1;
    }
    char d[FC_PATH_MAX];
    int r = fc_dest(fs_dst, dst_dir, base, st == 2, size, d, sizeof d);
    if (r < 0) {
        p->err = FC_E_PATH;
        return -1;
    }
    if (r == FC_SKIP) {
        p->skipped = 1;
        return 0;
    }
    if (st == 2)
        return fc_walk_dir(fs_src, p, src, d, 0);
    if (p->cap < 1) {
        p->err = FC_E_FULL;
        return -1;
    }
    struct fc_item *it = &p->items[p->n++];
    snprintf(it->src, sizeof it->src, "%s", src);
    snprintf(it->dst, sizeof it->dst, "%s", d);
    it->size = size;
    it->is_dir = 0;
    p->files = 1;
    p->bytes = size;
    return 0;
}

/* The selected FOLDER-view entry as a path relative to the stick: the level names 0..cursor
 * joined with '/'. 0, or -1 when a name is not a valid component or it does not fit. */
static inline int fc_rel_from_levels(const char *const *names, int n, char *out, size_t cap) {
    if (n < 1)
        return -1;
    out[0] = 0;
    char tmp[FC_PATH_MAX];
    for (int i = 0; i < n; i++) {
        if (fc_join(tmp, sizeof tmp, out, names[i]) < 0 || strlen(tmp) >= cap)
            return -1;
        snprintf(out, cap, "%s", tmp);
    }
    return 0;
}
#endif
