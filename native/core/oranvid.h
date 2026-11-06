/* oranvid.h — núcleo nativo de OranVid v2 (C99, sin dependencias externas).
 * Códec de video ORC2 + audio ORA2 + contenedor .oranvid. Usado por el CLI nativo y el plugin de VLC. */
#ifndef ORANVID_H
#define ORANVID_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define ORN_VERSION 2
#define ORN_HEADER_SIZE 64
#define ORN_CHUNK_HEADER_SIZE 20
#define ORN_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ORN_CH_META ORN_FOURCC('M', 'E', 'T', 'A')
#define ORN_CH_AUDIO ORN_FOURCC('A', 'U', 'D', 'B')
#define ORN_CH_VKEY ORN_FOURCC('V', 'I', 'D', 'K')
#define ORN_CH_VDELTA ORN_FOURCC('V', 'I', 'D', 'P')
#define ORN_CH_INDEX ORN_FOURCC('I', 'N', 'D', 'X')

typedef struct {
    uint16_t version;
    int has_video, has_audio;
    uint32_t width, height, fps_num, fps_den;
    uint64_t frame_count;
    uint32_t sample_rate;
    uint16_t channels;
    uint8_t video_codec, audio_codec, quant_step, audio_shift;
    uint16_t gop;
    uint64_t audio_samples, index_offset;
} orn_header;

uint32_t orn_crc32(const uint8_t *data, size_t n);
int orn_header_parse(const uint8_t b[ORN_HEADER_SIZE], orn_header *h);        /* 0 = ok */
void orn_header_serialize(const orn_header *h, uint8_t b[ORN_HEADER_SIZE]);
int orn_cpu_count(void);

/* ------------------------------------------------------------------ video (ORC2) */
/* Los frames son I420 contiguos: Y (w*h), U (w/2*h/2), V. w y h deben ser pares. */
#define ORN_FRAME_SIZE(w, h) ((size_t)(w) * (h) * 3 / 2)

typedef struct orn_venc orn_venc;
typedef struct orn_vdec orn_vdec;

orn_venc *orn_venc_new(int w, int h, int step, int gop, int threads);
/* Codifica un frame I420. 'out' pertenece al codificador y es válido hasta la siguiente llamada. 0 = ok. */
int orn_venc_encode(orn_venc *e, const uint8_t *yuv, int *is_key, const uint8_t **out, size_t *out_len);
double orn_venc_last_mse(const orn_venc *e);
void orn_venc_free(orn_venc *e);

orn_vdec *orn_vdec_new(int w, int h, int step, int threads);
/* Decodifica; *yuv apunta al frame I420 interno (válido hasta la siguiente llamada). 0 = ok. */
int orn_vdec_decode(orn_vdec *d, int is_key, const uint8_t *payload, size_t len, const uint8_t **yuv);
void orn_vdec_free(orn_vdec *d);

/* ------------------------------------------------------------------ audio (ORA2) */
/* pcm intercalado s16. Devuelve buffer malloc'd (liberar con free). */
int orn_audio_encode(int channels, int shift, const int16_t *pcm, int frames, uint8_t **out, size_t *out_len);
int orn_audio_decode(int channels, int shift, const uint8_t *payload, size_t len, int16_t **pcm, int *frames);

/* ------------------------------------------------------------------ contenedor */
typedef struct {
    void *ctx;
    int64_t (*read)(void *ctx, void *buf, size_t n); /* bytes leídos, 0 en EOF */
    int (*seek)(void *ctx, int64_t abs_offset);      /* 0 = ok */
    int64_t size;                                    /* tamaño total (<=0 si se desconoce) */
} orn_io;

typedef struct orn_reader orn_reader;
typedef struct { uint32_t type; int64_t pts; uint8_t *data; uint32_t len; } orn_chunk;

#define ORN_SKIP_AUDIO 1u
#define ORN_SKIP_VIDEO 2u

orn_reader *orn_reader_open(orn_io io, orn_header *hdr);   /* NULL si no es .oranvid */
/* 1 = chunk leído, 0 = fin, -1 = error/CRC. chunk.data se libera con orn_chunk_free. */
int orn_reader_next(orn_reader *r, orn_chunk *c, unsigned skip_mask);
void orn_chunk_free(orn_chunk *c);
int orn_reader_seek(orn_reader *r, int64_t offset);
void orn_reader_rewind(orn_reader *r);
const char *orn_reader_meta(const orn_reader *r, const char *key);
int orn_reader_meta_count(const orn_reader *r, int i, const char **key, const char **val);
int orn_reader_index_count(const orn_reader *r);
void orn_reader_index_get(const orn_reader *r, int i, int64_t *frame, int64_t *offset);
void orn_reader_close(orn_reader *r);

typedef struct orn_writer orn_writer;
orn_writer *orn_writer_open(const char *path, const orn_header *hdr, int nmeta, const char **keys, const char **vals);
int orn_writer_chunk(orn_writer *w, uint32_t type, int64_t pts, const uint8_t *data, size_t len);
int orn_writer_finish(orn_writer *w, uint64_t frames, uint64_t audio_samples); /* cierra el archivo */
void orn_writer_abort(orn_writer *w);

#ifdef __cplusplus
}
#endif
#endif
