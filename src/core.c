/* OP-2.2 native extraction core.
 * Decodes the installer's LZMA1 slice chunks, verifies SHA-1, writes files,
 * and creates shortcuts.  See core.h for the public interface.
 */
#include "core.h"
#include "lzma/LzmaDec.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#define OP22_BLOCK   65536u          /* must match Inno Setup's 64 KiB buffer */
#define OP22_INBUF   (256u * 1024u)

/* ISzAlloc backed by the CRT allocator. */
static void *sz_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return malloc(size);
}
static void sz_free(ISzAllocPtr p, void *address)
{
    (void)p;
    free(address);
}
static const ISzAlloc g_szalloc = { sz_alloc, sz_free };

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || errlen == 0)
        return;
    va_start(ap, fmt);
    _vsnprintf_s(err, errlen, _TRUNCATE, fmt, ap);
    va_end(ap);
    err[errlen - 1] = '\0';
}

static wchar_t *to_wide(const char *s)
{
    int n;
    wchar_t *w;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static char *join_path(const char *dir, const char *name)
{
    size_t a, b;
    char *p;
    if (!dir || !name)
        return NULL;
    a = strlen(dir);
    b = strlen(name);
    p = (char *)malloc(a + b + 2);
    if (!p)
        return NULL;
    memcpy(p, dir, a);
    p[a] = '\\';
    memcpy(p + a + 1, name, b + 1);
    return p;
}

static void slice_filename(char *buf, size_t cap, int n, int spd)
{
    int major = n / spd + 1;
    int minor = n % spd;
    if (spd <= 1)
        _snprintf_s(buf, cap, _TRUNCATE, "op-2.2-%d.bin", major);
    else
        _snprintf_s(buf, cap, _TRUNCATE, "op-2.2-%d%c.bin", major,
                    (char)('a' + minor));
}

static int read_exact(HANDLE h, void *buf, DWORD want)
{
    DWORD done = 0;
    while (done < want) {
        DWORD got = 0;
        if (!ReadFile(h, (unsigned char *)buf + done, want - done, &got, NULL) || got == 0)
            return 0;
        done += got;
    }
    return 1;
}

static int write_all(HANDLE h, const void *buf, DWORD len)
{
    DWORD done = 0;
    while (done < len) {
        DWORD put = 0;
        if (!WriteFile(h, (const unsigned char *)buf + done, len - done, &put, NULL) || put == 0)
            return 0;
        done += put;
    }
    return 1;
}

static uint64_t file_size_of(HANDLE h)
{
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (GetFileSizeEx(h, &li))
        return (uint64_t)li.QuadPart;
    return 0;
}

static int seek_abs(HANDLE h, uint64_t off)
{
    LARGE_INTEGER li;
    li.QuadPart = (__int64)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) != 0;
}

/* Create every missing directory component of a wide path. */
static int make_dirs_w(const wchar_t *path)
{
    wchar_t tmp[2048];
    size_t n = wcslen(path);
    size_t i;
    if (n >= sizeof(tmp) / sizeof(tmp[0]) - 1)
        return 0;
    wcscpy_s(tmp, sizeof(tmp) / sizeof(tmp[0]), path);
    for (i = 3; i < n; ++i) {           /* skip drive prefix "X:\" and final NUL */
        if (tmp[i] == L'\\' || tmp[i] == L'/' || tmp[i] == L'\0') {
            wchar_t save = tmp[i];
            tmp[i] = L'\0';
            if (tmp[0] && GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES)
                CreateDirectoryW(tmp, NULL);
            tmp[i] = save;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* SHA-1                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t total;
    unsigned char block[64];
    size_t used;
} sha1_ctx;

static uint32_t rol32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_compress(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80], a, b, cc, d, e;
    unsigned i;
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; ++i)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; ++i) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & cc) | ((~b) & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;            k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;            k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->used = 0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->total += len;
    if (c->used) {
        size_t take = 64 - c->used;
        if (take > len) take = len;
        memcpy(c->block + c->used, p, take);
        c->used += take; p += take; len -= take;
        if (c->used == 64) {
            sha1_compress(c, c->block);
            c->used = 0;
        }
    }
    while (len >= 64) {
        sha1_compress(c, p);
        p += 64; len -= 64;
    }
    if (len) {
        memcpy(c->block, p, len);
        c->used = len;
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    uint64_t bits = c->total * 8;
    unsigned char tail[72];
    size_t pad;
    unsigned i;
    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    pad = (c->used < 56) ? (56 - c->used) : (120 - c->used);
    sha1_update(c, tail, pad);
    for (i = 0; i < 8; ++i)
        tail[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(c, tail, 8);
    for (i = 0; i < 5; ++i) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

/* ------------------------------------------------------------------ */
/* slice reader: presents the compressed bytes of one chunk as a        */
/* contiguous stream, skipping the 12-byte header of each new slice.   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *dir;
    int spd;
    int slice;
    HANDLE h;
    uint64_t size;      /* total size of current slice file */
    uint64_t pos;       /* current offset in current slice file */
    char *lasterr;
    size_t lasterrlen;
} slice_reader;

static void sr_close(slice_reader *r)
{
    if (r->h && r->h != INVALID_HANDLE_VALUE)
        CloseHandle(r->h);
    r->h = NULL;
}

/* Open a slice and position just after its 12-byte header. */
static int sr_open_slice(slice_reader *r, int slice)
{
    char name[64];
    char *full;
    wchar_t *wf;
    unsigned char hdr[12];
    slice_filename(name, sizeof(name), slice, r->spd);
    full = join_path(r->dir, name);
    if (!full || !(wf = to_wide(full))) {
        free(full);
        return 0;
    }
    r->h = CreateFileW(wf, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wf);
    free(full);
    if (r->h == INVALID_HANDLE_VALUE) {
        r->h = NULL;
        return 0;
    }
    if (!seek_abs(r->h, 0) || !read_exact(r->h, hdr, 12) ||
        memcmp(hdr, "idska32\x1a", 8) != 0) {
        sr_close(r);
        return 0;
    }
    r->size = ((uint64_t)hdr[8]) | ((uint64_t)hdr[9] << 8) |
              ((uint64_t)hdr[10] << 16) | ((uint64_t)hdr[11] << 24);
    r->slice = slice;
    r->pos = 12;
    if (!seek_abs(r->h, 12)) {
        sr_close(r);
        return 0;
    }
    return 1;
}

static int sr_open(slice_reader *r, const char *dir, int spd, int slice,
                   uint64_t start_offset, char *err, size_t errlen)
{
    memset(r, 0, sizeof(*r));
    r->dir = dir;
    r->spd = spd;
    r->lasterr = err;
    r->lasterrlen = errlen;
    if (!sr_open_slice(r, slice)) {
        set_err(err, errlen, "cannot open slice %d in '%s'", slice, dir);
        return 0;
    }
    if (!seek_abs(r->h, start_offset)) {
        set_err(err, errlen, "cannot seek to %llu in slice %d",
                (unsigned long long)start_offset, slice);
        sr_close(r);
        return 0;
    }
    r->pos = start_offset;
    return 1;
}

static size_t sr_read(slice_reader *r, void *buf, size_t want)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < want) {
        uint64_t avail;
        DWORD take, nread = 0;
        if (!r->h) {
            if (!sr_open_slice(r, r->slice + 1))
                break;
            continue;
        }
        if (r->pos >= r->size) {
            sr_close(r);
            continue;
        }
        avail = r->size - r->pos;
        take = (DWORD)(avail < (uint64_t)(want - got) ? avail : (uint64_t)(want - got));
        if (take > (1u << 20))
            take = (1u << 20);
        if (!ReadFile(r->h, p + got, take, &nread, NULL) || nread == 0) {
            sr_close(r);
            continue;
        }
        got += nread;
        r->pos += nread;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* output sink                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int mode;                 /* 0 = discard, 1 = file, 2 = memory */
    HANDLE file;
    unsigned char *mem;
    size_t mem_pos, mem_cap;
} sink;

static int sink_write(sink *s, const void *buf, size_t len)
{
    if (len == 0)
        return 1;
    if (s->mode == 1)
        return write_all(s->file, buf, (DWORD)len);
    if (s->mode == 2) {
        if (s->mem_pos + len > s->mem_cap)
            return 0;
        memcpy(s->mem + s->mem_pos, buf, len);
        s->mem_pos += len;
        return 1;
    }
    return 1;
}

/* Inno Setup Compression.Base.pas: TransformCallInstructions(Encode=False).
 * Applied to each 64 KiB block with AddrOffset = bytes decoded so far.
 */
static void transform_call_instructions(unsigned char *buf, size_t size,
                                        uint32_t addr_offset)
{
    size_t i, limit;
    if (size < 5)
        return;
    limit = size - 4;
    i = 0;
    while (i < limit) {
        if (buf[i] == 0xE8 || buf[i] == 0xE9) {
            uint32_t addr, rel;
            ++i;
            if (buf[i + 3] == 0x00 || buf[i + 3] == 0xFF) {
                addr = (addr_offset + (uint32_t)i + 4u) & 0xFFFFFFu;
                rel = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                      ((uint32_t)buf[i + 2] << 16);
                rel -= addr;                       /* wrap is intentional */
                if (rel & 0x800000u)
                    buf[i + 3] = (unsigned char)~buf[i + 3];
                buf[i]     = (unsigned char)rel;
                buf[i + 1] = (unsigned char)(rel >> 8);
                buf[i + 2] = (unsigned char)(rel >> 16);
            }
            i += 4;                                 /* skip whole instruction */
        } else {
            ++i;
        }
    }
}

/* Decode one data entry, optionally transform it, verify SHA-1 and feed the
 * resulting bytes to a sink. */
static int decode_entry(const op22_data *d, const char *source_dir, int spd,
                        sink *out, char *err, size_t errlen)
{
    slice_reader r;
    CLzmaDec dec;
    unsigned char hdr[9];
    unsigned char inbuf[OP22_INBUF];
    unsigned char block[OP22_BLOCK];
    size_t in_pos = 0, in_len = 0, block_used = 0;
    uint64_t comp_left = d->compressed_size;
    uint64_t produced = 0;
    uint32_t addr_offset = 0;
    sha1_ctx sha;
    unsigned char digest[20];
    CLzmaProps props;
    SRes res;
    int ok = 0;

    (void)props;
    if (!sr_open(&r, source_dir, spd, d->first_slice, d->offset, err, errlen))
        goto done;

    if (!read_exact(r.h, hdr, 9) || memcmp(hdr, "zlb\x1a", 4) != 0) {
        set_err(err, errlen, "bad chunk header at offset %llu",
                (unsigned long long)d->offset);
        goto done;
    }
    if (LzmaProps_Decode(&props, hdr + 4, 5) != SZ_OK) {
        set_err(err, errlen, "invalid LZMA properties");
        goto done;
    }
    LzmaDec_Construct(&dec);
    if (LzmaDec_Allocate(&dec, hdr + 4, LZMA_PROPS_SIZE, &g_szalloc) != SZ_OK) {
        set_err(err, errlen, "cannot allocate LZMA decoder");
        goto done;
    }
    LzmaDec_Init(&dec);

    sha1_init(&sha);
    while (produced < d->uncompressed_size) {
        SizeT dest_len, src_len;
        ELzmaStatus status;
        size_t want;

        if (in_pos == in_len) {
            if (comp_left == 0) {
                set_err(err, errlen, "compressed input exhausted early");
                goto cleanup_dec;
            }
            want = (size_t)(comp_left < sizeof(inbuf) ? comp_left : sizeof(inbuf));
            in_len = sr_read(&r, inbuf, want);
            in_pos = 0;
            if (in_len == 0) {
                set_err(err, errlen, "cannot read compressed input");
                goto cleanup_dec;
            }
            comp_left -= in_len;
        }
        dest_len = (SizeT)(sizeof(block) - block_used);
        if ((uint64_t)dest_len > d->uncompressed_size - produced)
            dest_len = (SizeT)(d->uncompressed_size - produced);
        src_len = (SizeT)(in_len - in_pos);
        res = LzmaDec_DecodeToBuf(&dec, block + block_used, &dest_len,
                                  inbuf + in_pos, &src_len,
                                  LZMA_FINISH_ANY, &status);
        in_pos += src_len;
        produced += dest_len;
        block_used += dest_len;
        if (res != SZ_OK) {
            set_err(err, errlen, "LZMA decode error %d", (int)res);
            goto cleanup_dec;
        }
        if (block_used == sizeof(block)) {
            if (d->call_optimize)
                transform_call_instructions(block, sizeof(block), addr_offset);
            sha1_update(&sha, block, sizeof(block));
            if (!sink_write(out, block, sizeof(block))) {
                set_err(err, errlen, "cannot write output");
                goto cleanup_dec;
            }
            if (d->call_optimize)
                addr_offset += OP22_BLOCK;
            block_used = 0;
        }
        if (dest_len == 0 && src_len == 0 && produced < d->uncompressed_size) {
            set_err(err, errlen, "LZMA decoder stalled");
            goto cleanup_dec;
        }
    }
    if (block_used) {
        if (d->call_optimize)
            transform_call_instructions(block, block_used, addr_offset);
        sha1_update(&sha, block, block_used);
        if (!sink_write(out, block, block_used)) {
            set_err(err, errlen, "cannot write output");
            goto cleanup_dec;
        }
    }
    sha1_final(&sha, digest);
    if (memcmp(digest, d->sha1, 20) != 0) {
        set_err(err, errlen, "SHA-1 mismatch (got %02X%02X%02X%02X, expected %02X%02X%02X%02X)",
                digest[0], digest[1], digest[2], digest[3],
                d->sha1[0], d->sha1[1], d->sha1[2], d->sha1[3]);
        goto cleanup_dec;
    }
    ok = 1;

cleanup_dec:
    LzmaDec_Free(&dec, &g_szalloc);
done:
    sr_close(&r);
    return ok;
}

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */

static int comp_index(const char *name)
{
    int i;
    for (i = 0; i < g_manifest.ncomp; ++i)
        if (strcmp(g_manifest.comps[i].name, name) == 0)
            return i;
    return -1;
}

static int comp_is_selected(unsigned mask, const char *name)
{
    int idx = comp_index(name);
    return idx >= 0 && (mask & (1u << idx)) != 0;
}

static int file_selected(const op22_file *f, unsigned comp_mask)
{
    int i;
    int own;
    if (!f->components || !f->components[0])
        return 1;
    own = comp_index(f->components);
    /* Exclusive components are installed only when explicitly selected. */
    if (own >= 0 && (g_manifest.comps[own].flags & 2u) &&
        !(comp_mask & (1u << own)))
        return 0;
    if (comp_is_selected(comp_mask, f->components))
        return 1;
    for (i = 0; i < g_manifest.ncomp; ++i) {
        const op22_comp *c = &g_manifest.comps[i];
        size_t n = strlen(c->name);
        if ((comp_mask & (1u << i)) &&
            strncmp(f->components, c->name, n) == 0 &&
            f->components[n] == '\\')
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* threaded extraction                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *source_dir;
    const char *output_dir;   /* NULL = verify only */
    const int *items;
    int count;
    int next;
    CRITICAL_SECTION cs;
    int failed;
    char err[512];
    op22_progress progress;
    void *user;
    uint64_t total_bytes;
    uint64_t done_bytes;
} work;

static DWORD WINAPI worker_thread(LPVOID param)
{
    work *w = (work *)param;
    for (;;) {
        int idx;
        char local_err[512];
        uint64_t bytes;
        sink out;
        const op22_file *file = NULL;
        const op22_data *d;
        char *full = NULL;
        wchar_t *wf = NULL;

        EnterCriticalSection(&w->cs);
        if (w->next >= w->count || w->failed) {
            int stop = (w->next >= w->count) || w->failed;
            LeaveCriticalSection(&w->cs);
            if (stop)
                break;
        }
        idx = w->items[w->next++];
        LeaveCriticalSection(&w->cs);

        local_err[0] = '\0';
        memset(&out, 0, sizeof(out));
        if (w->output_dir) {
            file = &g_manifest.files[idx];
            full = join_path(w->output_dir, file->dest);
            if (!full) {
                set_err(local_err, sizeof(local_err), "out of memory");
                goto fail;
            }
            wf = to_wide(full);
            if (!wf) {
                set_err(local_err, sizeof(local_err), "bad output path");
                goto fail;
            }
            make_dirs_w(wf);
            out.file = CreateFileW(wf, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (out.file == INVALID_HANDLE_VALUE) {
                set_err(local_err, sizeof(local_err), "cannot create '%s' (win32 %lu)",
                        full, (unsigned long)GetLastError());
                goto fail;
            }
            out.mode = 1;
            d = &g_manifest.data[file->location];
            bytes = d->uncompressed_size;
        } else {
            d = &g_manifest.data[idx];
            bytes = d->uncompressed_size;
        }

        if (!decode_entry(d, w->source_dir, g_manifest.slices_per_disk,
                          &out, local_err, sizeof(local_err)))
            goto fail;

        if (out.file && out.file != INVALID_HANDLE_VALUE)
            CloseHandle(out.file);
        free(wf);
        free(full);
        EnterCriticalSection(&w->cs);
        w->done_bytes += bytes;
        if (w->progress) {
            int done_kb = (int)(w->done_bytes / 1024u);
            int total_kb = (int)(w->total_bytes / 1024u);
            if (done_kb < 0) done_kb = INT_MAX;
            if (total_kb < 0) total_kb = INT_MAX;
            w->progress(done_kb, total_kb, w->user);
        }
        LeaveCriticalSection(&w->cs);
        continue;

    fail:
        if (out.file && out.file != INVALID_HANDLE_VALUE)
            CloseHandle(out.file);
        free(wf);
        free(full);
        EnterCriticalSection(&w->cs);
        if (!w->failed) {
            w->failed = 1;
            set_err(w->err, sizeof(w->err), "%s",
                    local_err[0] ? local_err : "extraction failed");
        }
        LeaveCriticalSection(&w->cs);
        break;
    }
    return 0;
}

static int run_jobs(const char *source_dir, const char *output_dir,
                    const int *items, int count, uint64_t total_bytes,
                    op22_progress progress, void *user,
                    char *err, size_t errlen)
{
    work w;
    SYSTEM_INFO si;
    HANDLE *threads;
    int nthreads, i, started = 0;

    memset(&w, 0, sizeof(w));
    w.source_dir = source_dir;
    w.output_dir = output_dir;
    w.items = items;
    w.count = count;
    w.progress = progress;
    w.user = user;
    w.total_bytes = total_bytes;
    InitializeCriticalSection(&w.cs);

    GetSystemInfo(&si);
    /* Windows equivalent of nproc: one worker per logical processor. */
    nthreads = (int)si.dwNumberOfProcessors;
    if (nthreads < 1) nthreads = 1;
    if (count > 0 && nthreads > count) nthreads = count;
    threads = (HANDLE *)calloc((size_t)nthreads, sizeof(HANDLE));
    if (!threads) {
        set_err(err, errlen, "out of memory allocating worker handles");
        DeleteCriticalSection(&w.cs);
        return 1;
    }

    for (i = 0; i < nthreads; ++i) {
        threads[i] = CreateThread(NULL, 0, worker_thread, &w, 0, NULL);
        if (threads[i])
            ++started;
    }
    if (started)
        WaitForMultipleObjects((DWORD)started, threads, TRUE, INFINITE);
    for (i = 0; i < started; ++i)
        CloseHandle(threads[i]);
    free(threads);
    DeleteCriticalSection(&w.cs);

    if (w.failed) {
        set_err(err, errlen, "%s", w.err);
        return 1;   /* non-zero = failure (matches core.h / UI / CLI) */
    }
    return 0;       /* zero = success */
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int op22_install(const char *source_dir, const char *output_dir,
                  unsigned comp_mask, unsigned task_mask,
                  op22_progress progress, void *user,
                  char *err, size_t errlen)
{
    int *items;
    int count = 0, i;
    uint64_t total = 0;
    int ok;
    (void)task_mask;

    if (!source_dir || !output_dir) {
        set_err(err, errlen, "invalid argument");
        return 0;
    }
    items = (int *)malloc(sizeof(int) * (size_t)(g_manifest.nfile > 0 ? g_manifest.nfile : 1));
    if (!items) {
        set_err(err, errlen, "out of memory");
        return 0;
    }
    for (i = 0; i < g_manifest.nfile; ++i) {
        const op22_file *f = &g_manifest.files[i];
        if (f->is_tmp || f->location < 0 || f->location >= g_manifest.ndata)
            continue;
        if (!file_selected(f, comp_mask))
            continue;
        items[count++] = i;
        total += g_manifest.data[f->location].uncompressed_size;
    }
    if (progress)
        progress(0, (int)(total / 1024u), user);
    ok = run_jobs(source_dir, output_dir, items, count, total,
                  progress, user, err, errlen);
    free(items);
    return ok;
}

int op22_verify_only(const char *source_dir, op22_progress progress, void *user,
                      char *err, size_t errlen)
{
    int *items;
    int i;
    uint64_t total = 0;
    int ok;
    if (!source_dir) {
        set_err(err, errlen, "invalid argument");
        return 0;
    }
    items = (int *)malloc(sizeof(int) * (size_t)(g_manifest.ndata > 0 ? g_manifest.ndata : 1));
    if (!items) {
        set_err(err, errlen, "out of memory");
        return 0;
    }
    for (i = 0; i < g_manifest.ndata; ++i) {
        items[i] = i;
        total += g_manifest.data[i].uncompressed_size;
    }
    if (progress)
        progress(0, (int)(total / 1024u), user);
    ok = run_jobs(source_dir, NULL, items, g_manifest.ndata, total,
                  progress, user, err, errlen);
    free(items);
    return ok;
}

int op22_decode_preview(const char *source_dir, int variant,
                        uint8_t **buf, size_t *len, char *err, size_t errlen)
{
    const op22_data *d;
    sink out;
    unsigned char *mem;
    int ok;

    if (buf) *buf = NULL;
    if (len) *len = 0;
    if (!source_dir || !buf || !len || variant < 0 || variant > 2) {
        set_err(err, errlen, "invalid argument");
        return 1;
    }
    d = &g_manifest.data[47 + variant];
    mem = (unsigned char *)malloc((size_t)d->uncompressed_size);
    if (!mem) {
        set_err(err, errlen, "out of memory");
        return 1;
    }
    memset(&out, 0, sizeof(out));
    out.mode = 2;
    out.mem = mem;
    out.mem_cap = (size_t)d->uncompressed_size;
    ok = decode_entry(d, source_dir, g_manifest.slices_per_disk, &out,
                      err, errlen);
    if (!ok) {
        free(mem);
        return 1;
    }
    *buf = mem;
    *len = out.mem_pos;
    return 0;   /* zero = success */
}

/* ------------------------------------------------------------------ */
/* shortcuts                                                           */
/* ------------------------------------------------------------------ */

static int make_link(const wchar_t *lnk, const wchar_t *target,
                     const wchar_t *args, const wchar_t *workdir,
                     const wchar_t *icon)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr))
        return 0;
    if (SUCCEEDED(sl->lpVtbl->SetPath(sl, target))) {
        if (args && args[0])
            sl->lpVtbl->SetArguments(sl, args);
        if (workdir && workdir[0])
            sl->lpVtbl->SetWorkingDirectory(sl, workdir);
        if (icon && icon[0])
            sl->lpVtbl->SetIconLocation(sl, icon, 0);
        hr = sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
        if (SUCCEEDED(hr)) {
            if (SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE)))
                ok = 1;
            pf->lpVtbl->Release(pf);
        }
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

static int make_desktop_link(int csidl, const wchar_t *target,
                             const wchar_t *args, const wchar_t *workdir)
{
    wchar_t folder[MAX_PATH];
    wchar_t lnk[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(NULL, csidl, NULL, 0, folder);
    if (FAILED(hr))
        return 0;
    if (_snwprintf_s(lnk, MAX_PATH, _TRUNCATE, L"%s\\Cumulative Pack 2.2.lnk",
                     folder) < 0)
        return 0;
    return make_link(lnk, target, args, workdir, NULL);
}

int op22_create_shortcuts(const char *output_dir, unsigned task_mask,
                           char *err, size_t errlen)
{
    wchar_t *outw;
    wchar_t exe[MAX_PATH], work[MAX_PATH], icon[MAX_PATH];
    wchar_t programs[MAX_PATH], group[MAX_PATH], lnk[MAX_PATH];
    HRESULT hr;
    int cominited = 0;

    (void)err;
    (void)errlen;
    if (!output_dir)
        return 1;
    outw = to_wide(output_dir);
    if (!outw)
        return 1;
    if (_snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\bin\\XR_3DA.exe", outw) < 0 ||
        _snwprintf_s(work, MAX_PATH, _TRUNCATE, L"%s\\bin", outw) < 0 ||
        _snwprintf_s(icon, MAX_PATH, _TRUNCATE, L"%s\\bin\\op_2_2.ico", outw) < 0) {
        free(outw);
        return 1;
    }
    free(outw);

    if (FAILED(CoInitialize(NULL)))
        cominited = 0;
    else
        cominited = 1;

    /* Start Menu group: <Programs>\Cumulative Pack 2.2\Cumulative Pack 2.2.lnk */
    hr = SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, programs);
    if (SUCCEEDED(hr) &&
        _snwprintf_s(group, MAX_PATH, _TRUNCATE, L"%s\\Cumulative Pack 2.2", programs) >= 0) {
        make_dirs_w(group);
        if (_snwprintf_s(lnk, MAX_PATH, _TRUNCATE,
                         L"%s\\Cumulative Pack 2.2.lnk", group) >= 0)
            make_link(lnk, exe, L"-nointro", work, icon);
    }

    if (task_mask & (1u << OP22_TASK_COMMON))
        make_desktop_link(CSIDL_COMMON_DESKTOPDIRECTORY, exe, L"-nointro", work);
    if (task_mask & (1u << OP22_TASK_CURRENT))
        make_desktop_link(CSIDL_DESKTOPDIRECTORY, exe, L"-nointro", work);

    if (cominited)
        CoUninitialize();
    return 0;   /* zero = success */
}
