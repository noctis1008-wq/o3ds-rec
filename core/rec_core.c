#include "rec_core.h"

#include <string.h>

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static void put_chunk_header(uint8_t *p, const char type[4], uint32_t payload_len, uint64_t tick)
{
    memcpy(p, type, 4);
    put_u32(p + 4, payload_len);
    put_u64(p + 8, tick);
}

void rec_screen_init(rec_screen_state *st, uint8_t screen, uint16_t lines, uint32_t key_interval)
{
    memset(st, 0, sizeof(*st));
    st->screen = screen;
    st->lines = lines;
    st->key_interval = key_interval;
    st->format = 0xff;
}

size_t rec_write_file_header(uint8_t *out, uint64_t start_tick, uint32_t flags)
{
    memset(out, 0, REC_FILE_HEADER_SIZE);
    memcpy(out, REC_MAGIC, 4);
    put_u16(out + 4, REC_VERSION);
    put_u16(out + 6, REC_FILE_HEADER_SIZE);
    put_u32(out + 8, REC_TICK_HZ);
    put_u32(out + 12, flags);
    put_u64(out + 16, start_tick);
    return REC_FILE_HEADER_SIZE;
}

uint64_t rec_line_hash(const uint8_t *line, uint32_t line_bytes)
{
    /* 2系統の 32bit ハッシュを組み合わせて 64bit にする（ARM11 で軽い演算だけ） */
    const uint32_t *p = (const uint32_t *)(const void *)line;
    uint32_t n = line_bytes / 4u;
    uint32_t a = 0x811c9dc5u, b = 0x9e3779b9u;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = p[i];
        a = (a ^ w) * 0x01000193u;
        b = ((b << 5) | (b >> 27)) + (w ^ (i * 0x85ebca6bu));
    }
    return ((uint64_t)a << 32) | b;
}

static size_t encode_common(rec_screen_state *st, const uint8_t *cur, const uint8_t *prev,
                            uint64_t *line_hash, uint32_t pitch, uint8_t format, uint64_t tick,
                            uint8_t *out)
{
    const uint32_t src_line_bytes = REC_LINE_PX * rec_bpp(format);
    const int to565 = st->to_rgb565 && (format == REC_FMT_RGBA8 || format == REC_FMT_BGR8);
    const int pack = !to565 && st->pack_rgba8 && format == REC_FMT_RGBA8;
    const uint8_t out_format = to565 ? REC_FMT_RGB565 : pack ? REC_FMT_BGR8 : format;
    /* 入力1ピクセル内の B,G,R のバイト位置（RGBA8: A,B,G,R / BGR8: B,G,R） */
    const uint32_t sbpp = rec_bpp(format), boff = format == REC_FMT_RGBA8 ? 1u : 0u;
    const uint32_t line_bytes = REC_LINE_PX * rec_bpp(out_format);
    const uint32_t bm_bytes = rec_bitmap_bytes(st->lines);

    int key = (!prev && !line_hash) || !st->have_prev || format != st->format ||
              (st->key_interval && st->since_key >= st->key_interval);

    uint8_t *hdr = out;
    uint8_t *vh = out + REC_CHUNK_HEADER_SIZE;
    uint8_t *bitmap = vh + REC_VFRM_HEADER_SIZE;
    uint8_t *data = bitmap + bm_bytes;
    uint8_t *w = data;

    memset(bitmap, 0, bm_bytes);

    for (uint32_t i = 0; i < st->lines; ++i) {
        const uint8_t *c = cur + (size_t)i * pitch;
        if (line_hash) {
            uint64_t h = rec_line_hash(c, src_line_bytes);
            int same = line_hash[i] == h;
            line_hash[i] = h;
            if (!key && same)
                continue;
        } else if (!key && memcmp(c, prev + (size_t)i * pitch, src_line_bytes) == 0) {
            continue;
        }
        bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
        if (to565) {
            for (uint32_t k = 0; k < REC_LINE_PX; ++k) {
                uint32_t v = ((uint32_t)(c[boff + 2] >> 3) << 11) | ((uint32_t)(c[boff + 1] >> 2) << 5) |
                             (uint32_t)(c[boff] >> 3);
                w[0] = (uint8_t)v;
                w[1] = (uint8_t)(v >> 8);
                w += 2;
                c += sbpp;
            }
        } else if (pack) {
            /* メモリ上 A,B,G,R → B,G,R */
            for (uint32_t k = 0; k < REC_LINE_PX; ++k) {
                w[0] = c[1];
                w[1] = c[2];
                w[2] = c[3];
                w += 3;
                c += 4;
            }
        } else {
            memcpy(w, c, line_bytes);
            w += line_bytes;
        }
    }

    if (w == data && !key) {
        /* 変化なし: 書かない（変換時は直前フレームを保持して時間を埋める） */
        return 0;
    }

    vh[0] = st->screen;
    vh[1] = out_format;
    put_u16(vh + 2, st->lines);
    put_u16(vh + 4, (uint16_t)line_bytes);
    put_u16(vh + 6, key ? REC_FRAME_KEY : 0);
    put_u32(vh + 8, st->frame_count);

    uint32_t payload = (uint32_t)(w - vh);
    put_chunk_header(hdr, "VFRM", payload, tick);

    st->format = format;
    st->have_prev = 1;
    st->frame_count++;
    st->since_key = key ? 1 : st->since_key + 1;
    return (size_t)(w - out);
}

size_t rec_encode_frame(rec_screen_state *st, const uint8_t *cur, const uint8_t *prev,
                        uint32_t pitch, uint8_t format, uint64_t tick, uint8_t *out)
{
    return encode_common(st, cur, prev, NULL, pitch, format, tick, out);
}

size_t rec_encode_frame_hashed(rec_screen_state *st, const uint8_t *cur, uint32_t pitch,
                               uint8_t format, uint64_t tick, uint8_t *out, uint64_t *line_hash)
{
    return encode_common(st, cur, NULL, line_hash, pitch, format, tick, out);
}

size_t rec_write_audio(uint8_t *out, uint64_t tick, uint32_t sample_rate, uint16_t channels,
                       const int16_t *samples, uint32_t sample_frames)
{
    uint32_t data_bytes = sample_frames * channels * 2u;
    uint8_t *p = out + REC_CHUNK_HEADER_SIZE;
    put_u32(p, sample_rate);
    put_u16(p + 4, channels);
    put_u16(p + 6, 1); /* 1 = PCM16LE */
    /* 3DS(ARM)もx86もリトルエンディアンなのでそのままコピー */
    memcpy(p + 8, samples, data_bytes);
    put_chunk_header(out, "AUDI", 8 + data_bytes, tick);
    return REC_CHUNK_HEADER_SIZE + 8 + data_bytes;
}

size_t rec_write_eos(uint8_t *out, uint64_t tick)
{
    put_chunk_header(out, "EOS ", 0, tick);
    return REC_CHUNK_HEADER_SIZE;
}
