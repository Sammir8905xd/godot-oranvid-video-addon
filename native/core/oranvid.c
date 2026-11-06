#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
/* oranvid.c — implementación del núcleo OranVid v2 (ORC2 / ORA2 / contenedor). */
#include "oranvid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* ===================================================================== utilidades */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

uint32_t orn_crc32(const uint8_t *data, size_t n) {
    static uint32_t tab[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

int orn_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
#endif
}

/* ---- ejecución paralela de n trabajos (el 0 en el hilo actual) ---- */
#define MAXS 64
typedef void (*par_fn)(void *, int);
typedef struct { par_fn fn; void *ctx; int idx; } par_job;

#ifdef _WIN32
static unsigned __stdcall par_tramp(void *p) { par_job *j = (par_job *)p; j->fn(j->ctx, j->idx); return 0; }
static void par_for(int n, par_fn fn, void *ctx) {
    if (n <= 1) { if (n == 1) fn(ctx, 0); return; }
    par_job jobs[MAXS]; HANDLE h[MAXS];
    for (int i = 1; i < n; i++) { jobs[i].fn = fn; jobs[i].ctx = ctx; jobs[i].idx = i; h[i] = (HANDLE)_beginthreadex(NULL, 0, par_tramp, &jobs[i], 0, NULL); }
    fn(ctx, 0);
    for (int i = 1; i < n; i++) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
}
#else
static void *par_tramp(void *p) { par_job *j = (par_job *)p; j->fn(j->ctx, j->idx); return NULL; }
static void par_for(int n, par_fn fn, void *ctx) {
    if (n <= 1) { if (n == 1) fn(ctx, 0); return; }
    par_job jobs[MAXS]; pthread_t th[MAXS]; int started[MAXS] = {0};
    for (int i = 1; i < n; i++) {
        jobs[i].fn = fn; jobs[i].ctx = ctx; jobs[i].idx = i;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) == 0) started[i] = 1; else fn(ctx, i);
    }
    fn(ctx, 0);
    for (int i = 1; i < n; i++) if (started[i]) pthread_join(th[i], NULL);
}
#endif

/* ===================================================================== cabecera */
int orn_header_parse(const uint8_t b[ORN_HEADER_SIZE], orn_header *h) {
    static const uint8_t magic[8] = {'O', 'R', 'A', 'N', 'V', 'I', 'D', 0x1A};
    if (memcmp(b, magic, 8) != 0) return -1;
    memset(h, 0, sizeof *h);
    h->version = rd16(b + 8);
    if (h->version != ORN_VERSION) return -2;
    uint16_t fl = rd16(b + 10);
    h->has_video = fl & 1; h->has_audio = (fl >> 1) & 1;
    h->width = rd32(b + 12); h->height = rd32(b + 16);
    h->fps_num = rd32(b + 20); h->fps_den = rd32(b + 24);
    h->frame_count = rd64(b + 28);
    h->sample_rate = rd32(b + 36); h->channels = rd16(b + 40);
    h->video_codec = b[42]; h->audio_codec = b[43]; h->quant_step = b[44]; h->audio_shift = b[45];
    h->gop = rd16(b + 46);
    h->audio_samples = rd64(b + 48); h->index_offset = rd64(b + 56);
    if (h->has_video && (h->width < 2 || h->height < 2 || h->width > 16384 || h->height > 16384 ||
                         (h->width & 1) || (h->height & 1) || h->quant_step == 0 || h->fps_den == 0)) return -3;
    return 0;
}

void orn_header_serialize(const orn_header *h, uint8_t b[ORN_HEADER_SIZE]) {
    static const uint8_t magic[8] = {'O', 'R', 'A', 'N', 'V', 'I', 'D', 0x1A};
    memset(b, 0, ORN_HEADER_SIZE);
    memcpy(b, magic, 8);
    wr16(b + 8, ORN_VERSION);
    wr16(b + 10, (uint16_t)((h->has_video ? 1 : 0) | (h->has_audio ? 2 : 0)));
    wr32(b + 12, h->width); wr32(b + 16, h->height);
    wr32(b + 20, h->fps_num); wr32(b + 24, h->fps_den);
    wr64(b + 28, h->frame_count);
    wr32(b + 36, h->sample_rate); wr16(b + 40, h->channels);
    b[42] = h->video_codec; b[43] = h->audio_codec; b[44] = h->quant_step; b[45] = h->audio_shift;
    wr16(b + 46, h->gop);
    wr64(b + 48, h->audio_samples); wr64(b + 56, h->index_offset);
}

/* ===================================================================== codificador aritmético binario (tipo LZMA, probs de 12 bits) */
typedef struct { uint8_t *buf; size_t len, cap; uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_size; } rc_enc;
typedef struct { const uint8_t *p, *end; uint32_t range, code; } rc_dec;

static void rce_init(rc_enc *e) { e->len = 0; e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->cache_size = 1; }
static void rce_put(rc_enc *e, uint8_t b) {
    if (e->len == e->cap) { e->cap = e->cap ? e->cap * 2 : 65536; e->buf = (uint8_t *)realloc(e->buf, e->cap); }
    e->buf[e->len++] = b;
}
static void rce_shift(rc_enc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t temp = e->cache;
        do { rce_put(e, (uint8_t)(temp + (uint8_t)(e->low >> 32))); temp = 0xFF; } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}
static inline void rce_bit(rc_enc *e, uint16_t *p, int bit) {
    uint32_t bound = (e->range >> 12) * (*p);
    if (!bit) { e->range = bound; *p = (uint16_t)(*p + ((4096 - *p) >> 5)); }
    else { e->low += bound; e->range -= bound; *p = (uint16_t)(*p - (*p >> 5)); }
    while (e->range < (1u << 24)) { e->range <<= 8; rce_shift(e); }
}
static void rce_flush(rc_enc *e) { for (int i = 0; i < 5; i++) rce_shift(e); }

static inline uint8_t rcd_byte(rc_dec *d) { return d->p < d->end ? *d->p++ : 0; }
static void rcd_init(rc_dec *d, const uint8_t *p, size_t n) {
    d->p = p; d->end = p + n; d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | rcd_byte(d);
}
static inline int rcd_bit(rc_dec *d, uint16_t *p) {
    uint32_t bound = (d->range >> 12) * (*p);
    int bit;
    if (d->code < bound) { d->range = bound; *p = (uint16_t)(*p + ((4096 - *p) >> 5)); bit = 0; }
    else { d->code -= bound; d->range -= bound; *p = (uint16_t)(*p - (*p >> 5)); bit = 1; }
    while (d->range < (1u << 24)) { d->range <<= 8; d->code = (d->code << 8) | rcd_byte(d); }
    return bit;
}

/* ===================================================================== modelos de símbolos de residuo */
#define NCTX 9
/* Símbolo s (0..255) = ceros | clase de magnitud (unaria) | mantisa. Pocas decisiones binarias por muestra. */
typedef struct { uint16_t zf[NCTX]; uint16_t un[NCTX][8]; uint16_t mt[9][4]; uint16_t lo[9][8]; } symmodel;
typedef struct { symmodel sm[4]; uint16_t bf[2][4]; } models;   /* sm: [plano luma/chroma + 2*inter] */

static uint8_t bucket_tab[512];
static int tables_ready = 0;
static void init_tables(void) {
    if (tables_ready) return;
    for (int s = 0; s < 512; s++)
        bucket_tab[s] = (uint8_t)(s == 0 ? 0 : s == 1 ? 1 : s == 2 ? 2 : s <= 4 ? 3 : s <= 8 ? 4 : s <= 16 ? 5 : s <= 32 ? 6 : s <= 64 ? 7 : 8);
    tables_ready = 1;
}
static void models_init(models *m) {
    uint16_t *p = (uint16_t *)m;
    for (size_t i = 0; i < sizeof(models) / 2; i++) p[i] = 2048;
}

static inline void enc_sym(rc_enc *e, symmodel *sm, int ctx, uint8_t s) {
    if (s == 0) { rce_bit(e, &sm->zf[ctx], 0); return; }
    rce_bit(e, &sm->zf[ctx], 1);
    int nb = 1; for (unsigned t = (unsigned)s >> 1; t; t >>= 1) nb++;      /* longitud en bits, 1..8 */
    for (int i = 1; i < 8; i++) { int bit = nb > i; rce_bit(e, &sm->un[ctx][i - 1], bit); if (!bit) break; }
    const int m = nb - 1; int node = 1;
    for (int j = 0; j < m; j++) {
        int b = (s >> (m - 1 - j)) & 1;
        if (j < 2) { rce_bit(e, &sm->mt[nb][node], b); node = (node << 1) | b; }
        else rce_bit(e, &sm->lo[nb][j], b);
    }
}
static inline uint8_t dec_sym(rc_dec *d, symmodel *sm, int ctx) {
    if (!rcd_bit(d, &sm->zf[ctx])) return 0;
    int nb = 1; while (nb < 8 && rcd_bit(d, &sm->un[ctx][nb - 1])) nb++;
    const int m = nb - 1; int node = 1; unsigned v = 1;
    for (int j = 0; j < m; j++) {
        int b;
        if (j < 2) { b = rcd_bit(d, &sm->mt[nb][node]); node = (node << 1) | b; }
        else b = rcd_bit(d, &sm->lo[nb][j]);
        v = (v << 1) | (unsigned)b;
    }
    return (uint8_t)v;
}

static inline int med(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a > b ? b : a;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}
static inline int quant(int e, int step) {
    int a = e < 0 ? -e : e;
    int q = (a + ((step * 3) >> 3)) / step;
    return e < 0 ? -q : q;
}
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline uint8_t zz8(int q) { return (uint8_t)(((unsigned)q << 1) ^ (unsigned)(q >> 7)); }
static inline int unzz8(uint8_t s) { return (int)(s >> 1) ^ -(int)(s & 1); }

/* ===================================================================== codificación de un slice (banda horizontal) */
typedef struct {
    rc_enc enc;
    rc_dec dec;
    models *m;
    uint8_t *fl[2];
    const uint8_t *src[3];
    uint8_t *rec[3];
    const uint8_t *prev[3];
    uint8_t *sym[3];
    const uint8_t *in; size_t inlen;
    int y0, y1;                       /* filas de luma [y0,y1) */
    long nblocks, nchanged;
} slice_job;

typedef struct {
    int w, h, cw, ch, step, nthreads;
    int pw[3], ph[3]; size_t off[3];
    int slice_rows, nslices;
    int is_key, is_enc;
    uint8_t *buf[2]; int cur, have_prev;
    uint8_t *sym[3];
    slice_job jobs[MAXS];
    uint64_t index; int gop;
    uint8_t *out; size_t out_cap;
    double mse;
} codec;

static int codec_init(codec *c, int w, int h, int step, int threads) {
    if (w < 2 || h < 2 || (w & 1) || (h & 1) || step < 1 || step > 255) return -1;
    init_tables();
    memset(c, 0, sizeof *c);
    c->w = w; c->h = h; c->cw = w / 2; c->ch = h / 2; c->step = step;
    c->pw[0] = w; c->pw[1] = c->pw[2] = c->cw;
    c->ph[0] = h; c->ph[1] = c->ph[2] = c->ch;
    c->off[0] = 0; c->off[1] = (size_t)w * h; c->off[2] = c->off[1] + (size_t)c->cw * c->ch;
    if (threads < 1) threads = 1;
    if (threads > 32) threads = 32;
    c->nthreads = threads;
    size_t fs = ORN_FRAME_SIZE(w, h);
    c->buf[0] = (uint8_t *)calloc(fs, 1); c->buf[1] = (uint8_t *)calloc(fs, 1);
    for (int p = 0; p < 3; p++) c->sym[p] = (uint8_t *)calloc((size_t)c->pw[p] * c->ph[p], 1);
    if (!c->buf[0] || !c->buf[1] || !c->sym[0] || !c->sym[1] || !c->sym[2]) return -1;
    return 0;
}

static void codec_free(codec *c) {
    free(c->buf[0]); free(c->buf[1]);
    for (int p = 0; p < 3; p++) free(c->sym[p]);
    for (int i = 0; i < MAXS; i++) {
        free(c->jobs[i].enc.buf); free(c->jobs[i].m); free(c->jobs[i].fl[0]); free(c->jobs[i].fl[1]);
    }
    free(c->out);
}

static int slice_alloc(codec *c, slice_job *j) {
    if (!j->m) j->m = (models *)malloc(sizeof(models));
    int nb = (c->w + 7) / 8 + 2;
    if (!j->fl[0]) j->fl[0] = (uint8_t *)malloc(nb);
    if (!j->fl[1]) j->fl[1] = (uint8_t *)malloc(nb);
    return (j->m && j->fl[0] && j->fl[1]) ? 0 : -1;
}

static void code_plane(codec *c, slice_job *j, int pl, int inter) {
    const int enc = c->is_enc;
    const int w = c->pw[pl];
    const int ry0 = pl ? j->y0 >> 1 : j->y0, ry1 = pl ? j->y1 >> 1 : j->y1;
    const int step = c->step, pk = pl ? 1 : 0;
    const uint8_t *src = j->src[pl], *prev = j->prev[pl];
    uint8_t *rec = j->rec[pl], *sym = j->sym[pl];
    symmodel *sm = &j->m->sm[pk + (inter ? 2 : 0)];

    if (!inter) {
        for (int y = ry0; y < ry1; y++) {
            const int yl = y - ry0;
            for (int x = 0; x < w; x++) {
                const int i = y * w + x;
                int a = x > 0 ? rec[i - 1] : (yl > 0 ? rec[i - w] : 128);
                int b = yl > 0 ? rec[i - w] : a;
                int cc = (x > 0 && yl > 0) ? rec[i - w - 1] : b;
                int pred = med(a, b, cc);
                int ctx = bucket_tab[(x > 0 ? sym[i - 1] : 0) + (yl > 0 ? sym[i - w] : 0)];
                uint8_t s;
                if (enc) {
                    int e = src[i] - pred, q;
                    if (step == 1) { q = (int8_t)(uint8_t)e; rec[i] = (uint8_t)(pred + q); }
                    else { q = quant(e, step); rec[i] = clamp8(pred + q * step); }
                    s = zz8(q);
                    enc_sym(&j->enc, sm, ctx, s);
                } else {
                    s = dec_sym(&j->dec, sm, ctx);
                    int q = unzz8(s);
                    rec[i] = step == 1 ? (uint8_t)(pred + q) : clamp8(pred + q * step);
                }
                sym[i] = s;
            }
        }
        return;
    }

    /* ---- delta temporal por bloques con skip ---- */
    const int blk = pl ? 8 : 16;
    const int nbx = (w + blk - 1) / blk;
    uint8_t *fa = j->fl[0], *fc = j->fl[1];
    memset(fa, 0, (size_t)nbx + 1);
    memset(sym + (size_t)ry0 * w, 0, (size_t)(ry1 - ry0) * w);

    for (int by = ry0; by < ry1; by += blk) {
        const int bh = (ry1 - by) < blk ? (ry1 - by) : blk;
        for (int bx = 0, bi = 0; bx < w; bx += blk, bi++) {
            const int bw = (w - bx) < blk ? (w - bx) : blk;
            const int ctxf = (bi > 0 ? fc[bi - 1] : 0) | (by > ry0 ? (fa[bi] << 1) : 0);
            uint16_t *pf = &j->m->bf[pk][ctxf];
            int any;
            if (enc) {
                any = 0;
                if (step == 1) {
                    for (int yy = 0; yy < bh && !any; yy++) {
                        size_t row = (size_t)(by + yy) * w + bx;
                        if (memcmp(src + row, prev + row, (size_t)bw) != 0) any = 1;
                    }
                } else {
                    const int thr = step - ((step * 3) >> 3);
                    for (int yy = 0; yy < bh && !any; yy++) {
                        size_t row = (size_t)(by + yy) * w + bx;
                        for (int xx = 0; xx < bw; xx++) {
                            int d = src[row + xx] - prev[row + xx];
                            if (d < 0) d = -d;
                            if (d >= thr) { any = 1; break; }
                        }
                    }
                }
                rce_bit(&j->enc, pf, any);
            } else {
                any = rcd_bit(&j->dec, pf);
            }
            fc[bi] = (uint8_t)any;
            j->nblocks++;
            if (!any) {
                for (int yy = 0; yy < bh; yy++) {
                    size_t row = (size_t)(by + yy) * w + bx;
                    memcpy(rec + row, prev + row, (size_t)bw);
                }
                continue;
            }
            j->nchanged++;
            for (int yy = 0; yy < bh; yy++) {
                const int y = by + yy;
                for (int xx = 0; xx < bw; xx++) {
                    const int x = bx + xx, i = y * w + x;
                    int ctx = bucket_tab[(x > 0 ? sym[i - 1] : 0) + (y > ry0 ? sym[i - w] : 0)];
                    uint8_t s;
                    if (enc) {
                        int e = src[i] - prev[i], q;
                        if (step == 1) { q = (int8_t)(uint8_t)e; rec[i] = (uint8_t)(prev[i] + q); }
                        else { q = quant(e, step); rec[i] = clamp8(prev[i] + q * step); }
                        s = zz8(q);
                        enc_sym(&j->enc, sm, ctx, s);
                    } else {
                        s = dec_sym(&j->dec, sm, ctx);
                        int q = unzz8(s);
                        rec[i] = step == 1 ? (uint8_t)(prev[i] + q) : clamp8(prev[i] + q * step);
                    }
                    sym[i] = s;
                }
            }
        }
        uint8_t *t = fa; fa = fc; fc = t;
    }
}

static void slice_run(void *ctx, int idx) {
    codec *c = (codec *)ctx;
    slice_job *j = &c->jobs[idx];
    models_init(j->m);
    if (c->is_enc) rce_init(&j->enc); else rcd_init(&j->dec, j->in, j->inlen);
    j->nblocks = j->nchanged = 0;
    for (int pl = 0; pl < 3; pl++) code_plane(c, j, pl, !c->is_key);
    if (c->is_enc) rce_flush(&j->enc);
}

typedef struct { codec *c; int count, stride; } slice_group;

static void slice_group_run(void *ctx, int worker) {
    slice_group *g = (slice_group *)ctx;
    for (int i = worker; i < g->count; i += g->stride) slice_run(g->c, i);
}

static int prepare_jobs(codec *c, const uint8_t *src_yuv, int nslices) {
    uint8_t *cur = c->buf[c->cur], *prev = c->buf[1 - c->cur];
    for (int k = 0; k < nslices; k++) {
        slice_job *j = &c->jobs[k];
        if (slice_alloc(c, j) != 0) return -1;
        j->y0 = k * c->slice_rows;
        j->y1 = j->y0 + c->slice_rows < c->h ? j->y0 + c->slice_rows : c->h;
        for (int p = 0; p < 3; p++) {
            j->src[p] = src_yuv ? src_yuv + c->off[p] : NULL;
            j->rec[p] = cur + c->off[p];
            j->prev[p] = prev + c->off[p];
            j->sym[p] = c->sym[p];
        }
    }
    return 0;
}

/* ===================================================================== codificador */
struct orn_venc { codec c; };

orn_venc *orn_venc_new(int w, int h, int step, int gop, int threads) {
    orn_venc *e = (orn_venc *)calloc(1, sizeof *e);
    if (!e) return NULL;
    if (threads <= 0) threads = orn_cpu_count();
    if (threads > 8) threads = 8;
    if (codec_init(&e->c, w, h, step, threads) != 0) { free(e); return NULL; }
    e->c.is_enc = 1;
    e->c.gop = gop < 1 ? 1 : gop;
    int rows = (h + threads - 1) / threads;
    rows = (rows + 15) & ~15;
    if (rows < 64) rows = 64;
    e->c.slice_rows = rows;
    e->c.nslices = (h + rows - 1) / rows;
    return e;
}

int orn_venc_encode(orn_venc *e, const uint8_t *yuv, int *is_key, const uint8_t **out, size_t *out_len) {
    codec *c = &e->c;
    int key = !c->have_prev || (c->index % (uint64_t)c->gop) == 0;
    if (prepare_jobs(c, yuv, c->nslices) != 0) return -1;

    if (!key) {
        c->is_key = 0;
        par_for(c->nslices, slice_run, c);
        long blocks = 0, changed = 0;
        for (int k = 0; k < c->nslices; k++) { blocks += c->jobs[k].nblocks; changed += c->jobs[k].nchanged; }
        if (blocks > 0 && changed * 100 > blocks * 65) key = 1;   /* cambio de escena */
    }
    if (key) { c->is_key = 1; par_for(c->nslices, slice_run, c); }

    size_t total = 2 + 4 * (size_t)c->nslices;
    for (int k = 0; k < c->nslices; k++) total += c->jobs[k].enc.len;
    if (total > c->out_cap) { c->out_cap = total + total / 4; c->out = (uint8_t *)realloc(c->out, c->out_cap); if (!c->out) return -1; }
    wr16(c->out, (uint16_t)c->slice_rows);
    size_t pos = 2 + 4 * (size_t)c->nslices;
    for (int k = 0; k < c->nslices; k++) {
        wr32(c->out + 2 + 4 * k, (uint32_t)c->jobs[k].enc.len);
        memcpy(c->out + pos, c->jobs[k].enc.buf, c->jobs[k].enc.len);
        pos += c->jobs[k].enc.len;
    }

    uint8_t *cur = c->buf[c->cur];
    if (c->step == 1) c->mse = 0;
    else {
        double s = 0;
        size_t n = (size_t)c->w * c->h;
        for (size_t i = 0; i < n; i++) { int d = yuv[i] - cur[i]; s += d * d; }
        c->mse = s / (double)n;
    }
    c->cur = 1 - c->cur; c->have_prev = 1; c->index++;
    *is_key = key; *out = c->out; *out_len = total;
    return 0;
}
double orn_venc_last_mse(const orn_venc *e) { return e->c.mse; }
void orn_venc_free(orn_venc *e) { if (e) { codec_free(&e->c); free(e); } }

/* ===================================================================== decodificador */
struct orn_vdec { codec c; };

orn_vdec *orn_vdec_new(int w, int h, int step, int threads) {
    orn_vdec *d = (orn_vdec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    if (threads <= 0) threads = orn_cpu_count();
    if (threads > 8) threads = 8;
    if (codec_init(&d->c, w, h, step, threads) != 0) { free(d); return NULL; }
    d->c.is_enc = 0;
    return d;
}

int orn_vdec_decode(orn_vdec *d, int is_key, const uint8_t *payload, size_t len, const uint8_t **yuv) {
    codec *c = &d->c;
    if (!is_key && !c->have_prev) return -1;
    if (len < 6) return -1;
    int sr = rd16(payload);
    if (sr < 16 || (sr & 15)) return -1;
    int n = (c->h + sr - 1) / sr;
    if (n < 1 || n > MAXS || len < 2 + 4 * (size_t)n) return -1;
    c->slice_rows = sr; c->nslices = n;
    if (prepare_jobs(c, NULL, n) != 0) return -1;

    size_t pos = 2 + 4 * (size_t)n;
    for (int k = 0; k < n; k++) {
        uint32_t sl = rd32(payload + 2 + 4 * k);
        if (sl > len - pos) return -1;
        c->jobs[k].in = payload + pos; c->jobs[k].inlen = sl;
        pos += sl;
    }
    c->is_key = is_key ? 1 : 0;
    int workers = n < c->nthreads ? n : c->nthreads;
    slice_group group = {c, n, workers};
    par_for(workers, slice_group_run, &group);
    *yuv = c->buf[c->cur];
    c->cur = 1 - c->cur; c->have_prev = 1;
    return 0;
}
void orn_vdec_free(orn_vdec *d) { if (d) { codec_free(&d->c); free(d); } }

/* ===================================================================== audio ORA2 */
typedef struct { uint16_t nb[24][32]; uint16_t mant[24][4]; } aint_models;
static void aint_init(aint_models *m) { uint16_t *p = (uint16_t *)m; for (size_t i = 0; i < sizeof *m / 2; i++) p[i] = 2048; }

static void aint_enc(rc_enc *e, aint_models *m, int *ctx, uint32_t z) {
    int nbits = 0; for (uint32_t t = z; t; t >>= 1) nbits++;
    uint16_t *tr = m->nb[*ctx]; int node = 1;
    for (int i = 4; i >= 0; i--) { int b = (nbits >> i) & 1; rce_bit(e, &tr[node], b); node = (node << 1) | b; }
    if (nbits >= 2) {
        int mb = nbits - 1, node2 = 1;
        for (int k = mb - 1, n = 0; k >= 0; k--, n++) {
            int b = (z >> k) & 1;
            if (n < 2) { rce_bit(e, &m->mant[nbits][node2], b); node2 = (node2 << 1) | b; }
            else { uint16_t p = 2048; rce_bit(e, &p, b); }
        }
    }
    *ctx = nbits > 23 ? 23 : nbits;
}
static int aint_dec(rc_dec *d, aint_models *m, int *ctx, uint32_t *z) {
    uint16_t *tr = m->nb[*ctx]; int node = 1;
    for (int i = 0; i < 5; i++) node = (node << 1) | rcd_bit(d, &tr[node]);
    int nbits = node & 31;
    if (nbits > 23) return -1;
    uint32_t v = nbits ? 1 : 0;
    if (nbits >= 2) {
        int mb = nbits - 1, node2 = 1;
        for (int n = 0; n < mb; n++) {
            int b;
            if (n < 2) { b = rcd_bit(d, &m->mant[nbits][node2]); node2 = (node2 << 1) | b; }
            else { uint16_t p = 2048; b = rcd_bit(d, &p); }
            v = (v << 1) | (uint32_t)b;
        }
    }
    *z = v; *ctx = nbits;
    return 0;
}

static inline int apred(const int32_t *x, int n, int order) {
    int x1 = n >= 1 ? x[n - 1] : 0, x2 = n >= 2 ? x[n - 2] : 0;
    return order == 0 ? 0 : order == 1 ? x1 : 2 * x1 - x2;
}

int orn_audio_encode(int ch, int shift, const int16_t *pcm, int frames, uint8_t **out, size_t *out_len) {
    if (ch < 1 || ch > 8 || shift < 0 || shift > 8 || frames < 0) return -1;
    int32_t *x[8] = {0};
    for (int c = 0; c < ch; c++) {
        x[c] = (int32_t *)malloc(sizeof(int32_t) * (size_t)(frames ? frames : 1));
        if (!x[c]) { for (int k = 0; k < c; k++) free(x[k]); return -1; }
        for (int i = 0; i < frames; i++) x[c][i] = (int32_t)pcm[(size_t)i * ch + c] >> shift;
    }
    if (ch == 2) for (int i = 0; i < frames; i++) { int l = x[0][i], r = x[1][i]; x[0][i] = (l + r) >> 1; x[1][i] = l - r; }

    rc_enc e; memset(&e, 0, sizeof e); rce_init(&e);
    uint8_t orders[8];
    aint_models *am = (aint_models *)malloc(sizeof *am);
    int rc = 0;
    for (int c = 0; c < ch && !rc; c++) {
        long long best = -1; int bo = 0;
        for (int o = 0; o <= 2; o++) {
            long long cost = 0;
            for (int n = 0; n < frames; n++) { int r = x[c][n] - apred(x[c], n, o); cost += r < 0 ? -r : r; }
            if (best < 0 || cost < best) { best = cost; bo = o; }
        }
        orders[c] = (uint8_t)bo;
        aint_init(am);
        int ctx = 0;
        for (int n = 0; n < frames; n++) {
            int32_t r = x[c][n] - apred(x[c], n, bo);
            uint32_t z = ((uint32_t)r << 1) ^ (uint32_t)(r >> 31);
            if (z >= (1u << 23)) { rc = -1; break; }
            aint_enc(&e, am, &ctx, z);
        }
    }
    rce_flush(&e);
    free(am);
    for (int c = 0; c < ch; c++) free(x[c]);
    if (rc) { free(e.buf); return -1; }
    size_t total = 4 + (size_t)ch + e.len;
    uint8_t *o = (uint8_t *)malloc(total);
    if (!o) { free(e.buf); return -1; }
    wr32(o, (uint32_t)frames);
    memcpy(o + 4, orders, (size_t)ch);
    memcpy(o + 4 + ch, e.buf, e.len);
    free(e.buf);
    *out = o; *out_len = total;
    return 0;
}

int orn_audio_decode(int ch, int shift, const uint8_t *p, size_t len, int16_t **pcm, int *frames_out) {
    if (ch < 1 || ch > 8 || shift < 0 || shift > 8 || len < 4 + (size_t)ch) return -1;
    uint32_t fr = rd32(p);
    if (fr > 10000000u) return -1;
    int frames = (int)fr;
    int32_t *x[8] = {0};
    rc_dec d; rcd_init(&d, p + 4 + ch, len - 4 - (size_t)ch);
    aint_models *am = (aint_models *)malloc(sizeof *am);
    int rc = 0;
    for (int c = 0; c < ch; c++) { x[c] = (int32_t *)malloc(sizeof(int32_t) * (size_t)(frames ? frames : 1)); if (!x[c]) rc = -1; }
    for (int c = 0; c < ch && !rc; c++) {
        int order = p[4 + c];
        if (order > 2) { rc = -1; break; }
        aint_init(am);
        int ctx = 0;
        for (int n = 0; n < frames; n++) {
            uint32_t z;
            if (aint_dec(&d, am, &ctx, &z) != 0) { rc = -1; break; }
            int32_t r = (int32_t)(z >> 1) ^ -(int32_t)(z & 1);
            x[c][n] = apred(x[c], n, order) + r;
        }
    }
    free(am);
    int16_t *o = NULL;
    if (!rc) {
        if (ch == 2) for (int i = 0; i < frames; i++) {
            int m = x[0][i], s = x[1][i], sum = (m * 2) | (s & 1);
            x[0][i] = (sum + s) >> 1; x[1][i] = (sum - s) >> 1;
        }
        o = (int16_t *)malloc(sizeof(int16_t) * (size_t)(frames ? frames : 1) * ch);
        if (!o) rc = -1;
        else {
            int bias = shift ? 1 << (shift - 1) : 0;
            for (int c = 0; c < ch; c++) for (int i = 0; i < frames; i++) {
                int v = x[c][i] * (1 << shift) + bias;
                o[(size_t)i * ch + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            }
        }
    }
    for (int c = 0; c < ch; c++) free(x[c]);
    if (rc) { free(o); return -1; }
    *pcm = o; *frames_out = frames;
    return 0;
}

/* ===================================================================== lector de contenedor */
struct orn_reader {
    orn_io io; orn_header hdr; int64_t pos;
    char **mk, **mv; int nmeta;
    int64_t *ixf, *ixo; int nidx;
};

static int rd_exact(orn_reader *r, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf; size_t got = 0;
    while (got < n) { int64_t k = r->io.read(r->io.ctx, p + got, n - got); if (k <= 0) break; got += (size_t)k; }
    r->pos += (int64_t)got;
    return got == n ? 0 : -1;
}

int orn_reader_seek(orn_reader *r, int64_t off) { if (r->io.seek(r->io.ctx, off) != 0) return -1; r->pos = off; return 0; }
void orn_reader_rewind(orn_reader *r) { orn_reader_seek(r, ORN_HEADER_SIZE); }
void orn_chunk_free(orn_chunk *c) { free(c->data); c->data = NULL; }

int orn_reader_next(orn_reader *r, orn_chunk *c, unsigned skip) {
    uint8_t h[ORN_CHUNK_HEADER_SIZE];
    memset(c, 0, sizeof *c);
    if (rd_exact(r, h, sizeof h) != 0) return 0;
    c->type = rd32(h); c->len = rd32(h + 4); c->pts = (int64_t)rd64(h + 8);
    uint32_t crc = rd32(h + 16);
    if ((r->io.size > 0 && (int64_t)c->len > r->io.size - r->pos) || c->len > 0x20000000u) return -1;
    int do_skip = (c->type == ORN_CH_AUDIO && (skip & ORN_SKIP_AUDIO)) ||
                  ((c->type == ORN_CH_VKEY || c->type == ORN_CH_VDELTA) && (skip & ORN_SKIP_VIDEO));
    if (do_skip) { return orn_reader_seek(r, r->pos + c->len) == 0 ? 1 : -1; }
    c->data = (uint8_t *)malloc(c->len ? c->len : 1);
    if (!c->data) return -1;
    if (rd_exact(r, c->data, c->len) != 0 || orn_crc32(c->data, c->len) != crc) { orn_chunk_free(c); return -1; }
    return 1;
}

static int net_string(const uint8_t *p, size_t n, size_t *pos, char **out) {
    uint32_t len = 0; int sh = 0;
    for (;;) { if (*pos >= n) return -1; uint8_t b = p[(*pos)++]; len |= (uint32_t)(b & 0x7F) << sh; if (!(b & 0x80)) break; sh += 7; if (sh > 28) return -1; }
    if (*pos + len > n) return -1;
    *out = (char *)malloc(len + 1); memcpy(*out, p + *pos, len); (*out)[len] = 0; *pos += len;
    return 0;
}

orn_reader *orn_reader_open(orn_io io, orn_header *hdr) {
    uint8_t hb[ORN_HEADER_SIZE];
    orn_reader *r = (orn_reader *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->io = io;
    if (io.seek(io.ctx, 0) != 0) { free(r); return NULL; }
    if (rd_exact(r, hb, sizeof hb) != 0 || orn_header_parse(hb, &r->hdr) != 0) { free(r); return NULL; }

    orn_chunk c;
    if (orn_reader_next(r, &c, 0) == 1) {
        if (c.type == ORN_CH_META && c.len >= 2) {
            int n = rd16(c.data); size_t q = 2;
            r->mk = (char **)calloc((size_t)n + 1, sizeof(char *)); r->mv = (char **)calloc((size_t)n + 1, sizeof(char *));
            for (int i = 0; i < n; i++) {
                if (net_string(c.data, c.len, &q, &r->mk[r->nmeta]) != 0) break;
                if (net_string(c.data, c.len, &q, &r->mv[r->nmeta]) != 0) { free(r->mk[r->nmeta]); r->mk[r->nmeta] = NULL; break; }
                r->nmeta++;
            }
        }
        orn_chunk_free(&c);
    }
    if (r->hdr.index_offset > 0 && (io.size <= 0 || (int64_t)r->hdr.index_offset < io.size) &&
        orn_reader_seek(r, (int64_t)r->hdr.index_offset) == 0 && orn_reader_next(r, &c, 0) == 1) {
        if (c.type == ORN_CH_INDEX && c.len >= 4) {
            uint32_t n = rd32(c.data);
            if ((uint64_t)n * 16 + 4 <= c.len) {
                r->ixf = (int64_t *)malloc(sizeof(int64_t) * (n ? n : 1)); r->ixo = (int64_t *)malloc(sizeof(int64_t) * (n ? n : 1));
                for (uint32_t i = 0; i < n; i++) { r->ixf[i] = (int64_t)rd64(c.data + 4 + 16 * i); r->ixo[i] = (int64_t)rd64(c.data + 12 + 16 * i); }
                r->nidx = (int)n;
            }
        }
        orn_chunk_free(&c);
    }
    orn_reader_rewind(r);
    if (hdr) *hdr = r->hdr;
    return r;
}

const char *orn_reader_meta(const orn_reader *r, const char *key) {
    for (int i = 0; i < r->nmeta; i++) if (strcmp(r->mk[i], key) == 0) return r->mv[i];
    return NULL;
}
int orn_reader_meta_count(const orn_reader *r, int i, const char **k, const char **v) {
    if (i < 0 || i >= r->nmeta) return r->nmeta;
    *k = r->mk[i]; *v = r->mv[i];
    return r->nmeta;
}
int orn_reader_index_count(const orn_reader *r) { return r->nidx; }
void orn_reader_index_get(const orn_reader *r, int i, int64_t *f, int64_t *o) { *f = r->ixf[i]; *o = r->ixo[i]; }
void orn_reader_close(orn_reader *r) {
    if (!r) return;
    for (int i = 0; i < r->nmeta; i++) { free(r->mk[i]); free(r->mv[i]); }
    free(r->mk); free(r->mv); free(r->ixf); free(r->ixo); free(r);
}

/* ===================================================================== escritor de contenedor */
#ifdef _WIN32
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#define FSEEK fseeko
#define FTELL ftello
#endif

struct orn_writer {
    FILE *f; orn_header hdr;
    int64_t *kf, *ko; size_t nk, cap;
    int64_t la_off, la_start, la_len;   /* último bloque de audio escrito */
};

static void put_net_string(uint8_t **p, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    uint8_t tmp[5]; int k = 0; size_t v = n;
    while (v >= 0x80) { tmp[k++] = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
    tmp[k++] = (uint8_t)v;
    if (*len + (size_t)k + n > *cap) { *cap = (*len + k + n) * 2; *p = (uint8_t *)realloc(*p, *cap); }
    memcpy(*p + *len, tmp, (size_t)k); *len += (size_t)k;
    memcpy(*p + *len, s, n); *len += n;
}

int orn_writer_chunk(orn_writer *w, uint32_t type, int64_t pts, const uint8_t *data, size_t len) {
    int64_t pos = FTELL(w->f);
    if (type == ORN_CH_AUDIO && len >= 4) { w->la_off = pos; w->la_start = pts; w->la_len = (int64_t)rd32(data); }
    if (type == ORN_CH_VKEY) {
        if (w->nk == w->cap) { w->cap = w->cap ? w->cap * 2 : 256; w->kf = (int64_t *)realloc(w->kf, w->cap * 8); w->ko = (int64_t *)realloc(w->ko, w->cap * 8); }
        int64_t start = pos;
        if (w->hdr.has_audio && w->la_off >= 0 && w->hdr.fps_num) {
            int64_t t = pts * (int64_t)w->hdr.fps_den * (int64_t)w->hdr.sample_rate / (int64_t)w->hdr.fps_num;
            if (t >= w->la_start && t < w->la_start + w->la_len) start = w->la_off;   /* el bloque de audio que cubre este keyframe */
        }
        w->kf[w->nk] = pts; w->ko[w->nk] = start; w->nk++;
    }
    uint8_t h[ORN_CHUNK_HEADER_SIZE];
    wr32(h, type); wr32(h + 4, (uint32_t)len); wr64(h + 8, (uint64_t)pts); wr32(h + 16, orn_crc32(data, len));
    if (fwrite(h, 1, sizeof h, w->f) != sizeof h) return -1;
    if (len && fwrite(data, 1, len, w->f) != len) return -1;
    return 0;
}

orn_writer *orn_writer_open(const char *path, const orn_header *hdr, int nmeta, const char **keys, const char **vals) {
    orn_writer *w = (orn_writer *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->hdr = *hdr; w->hdr.version = ORN_VERSION; w->la_off = -1;
    w->f = fopen(path, "wb+");
    if (!w->f) { free(w); return NULL; }
    uint8_t hb[ORN_HEADER_SIZE]; orn_header_serialize(&w->hdr, hb);
    fwrite(hb, 1, sizeof hb, w->f);
    if (nmeta > 0) {
        uint8_t *buf = NULL; size_t len = 2, cap = 256;
        buf = (uint8_t *)malloc(cap); wr16(buf, (uint16_t)nmeta);
        for (int i = 0; i < nmeta; i++) { put_net_string(&buf, &len, &cap, keys[i]); put_net_string(&buf, &len, &cap, vals[i]); }
        orn_writer_chunk(w, ORN_CH_META, 0, buf, len);
        free(buf);
    }
    return w;
}

int orn_writer_finish(orn_writer *w, uint64_t frames, uint64_t audio_samples) {
    int64_t idx_off = FTELL(w->f);
    size_t len = 4 + w->nk * 16;
    uint8_t *buf = (uint8_t *)malloc(len);
    wr32(buf, (uint32_t)w->nk);
    for (size_t i = 0; i < w->nk; i++) { wr64(buf + 4 + 16 * i, (uint64_t)w->kf[i]); wr64(buf + 12 + 16 * i, (uint64_t)w->ko[i]); }
    int rc = orn_writer_chunk(w, ORN_CH_INDEX, 0, buf, len);
    free(buf);
    w->hdr.frame_count = frames; w->hdr.audio_samples = audio_samples; w->hdr.index_offset = (uint64_t)idx_off;
    uint8_t hb[ORN_HEADER_SIZE]; orn_header_serialize(&w->hdr, hb);
    FSEEK(w->f, 0, SEEK_SET);
    if (fwrite(hb, 1, sizeof hb, w->f) != sizeof hb) rc = -1;
    if (fclose(w->f) != 0) rc = -1;
    free(w->kf); free(w->ko); free(w);
    return rc;
}
void orn_writer_abort(orn_writer *w) { if (!w) return; fclose(w->f); free(w->kf); free(w->ko); free(w); }
