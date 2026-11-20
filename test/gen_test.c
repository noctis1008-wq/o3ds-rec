/*
 * ホスト(PC)用テスト: 実機のキャプチャを模した合成フレームを rec_core でエンコードし
 *   out.3dr             … 録画ファイル
 *   expect/sS_NNNNNN.bin … エンコード時点の正解バッファ（パック済み）
 * を出力する。3dr2mp4.py --dump-raw の出力と一致すれば往復OK。
 */
#include "../core/rec_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TOP_LINES 400
#define BOT_LINES 320
#define TICKS_PER_60HZ (REC_TICK_HZ / 60)

static void put_px(uint8_t *fb, uint32_t pitch, uint8_t fmt, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    /* 画面座標(x,y) → フレームバッファ: ライン x、ライン内 239-y */
    uint8_t *p = fb + (size_t)x * pitch + (size_t)(REC_LINE_PX - 1 - y) * rec_bpp(fmt);
    switch (fmt) {
    case REC_FMT_RGBA8: p[0] = 0xff; p[1] = b; p[2] = g; p[3] = r; break;
    case REC_FMT_BGR8: p[0] = b; p[1] = g; p[2] = r; break;
    default: {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    }
    }
}

static void draw(uint8_t *fb, uint32_t pitch, uint8_t fmt, int lines, int t)
{
    for (int x = 0; x < lines; ++x)
        for (int y = 0; y < 240; ++y)
            put_px(fb, pitch, fmt, x, y, (uint8_t)(x * 255 / lines), (uint8_t)(y * 255 / 240), 64);
    /* 動く四角 */
    int sx = (t * 3) % (lines - 40);
    for (int x = sx; x < sx + 40; ++x)
        for (int y = 100; y < 140; ++y)
            put_px(fb, pitch, fmt, x, y, 255, 255, 255);
    /* 向き確認用: 画面左上に赤、右下に青 */
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            put_px(fb, pitch, fmt, x, y, 255, 0, 0);
            put_px(fb, pitch, fmt, lines - 1 - x, 239 - y, 0, 0, 255);
        }
}

static int g_pack, g_565;

static void dump_expect(const uint8_t *fb, uint32_t pitch, uint8_t fmt, int lines, int screen, uint32_t no)
{
    char path[64];
    snprintf(path, sizeof path, "expect/s%d_%06u.bin", screen, no);
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < lines; ++i) {
        const uint8_t *l = fb + (size_t)i * pitch;
        if (g_565 && (fmt == REC_FMT_RGBA8 || fmt == REC_FMT_BGR8)) {
            int bo = fmt == REC_FMT_RGBA8 ? 1 : 0, bp = (int)rec_bpp(fmt);
            for (int k = 0; k < REC_LINE_PX; ++k) {
                const uint8_t *p = l + k * bp + bo;
                uint16_t v = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
                fwrite(&v, 2, 1, f);
            }
        } else if (g_pack && fmt == REC_FMT_RGBA8)
            for (int k = 0; k < REC_LINE_PX; ++k)
                fwrite(l + k * 4 + 1, 1, 3, f);
        else
            fwrite(l, 1, REC_LINE_PX * rec_bpp(fmt), f);
    }
    fclose(f);
}

/* 出力をメモリに貯め、最後に（SPLIT 指定時は2ファイルに分けて）書き出す */
static uint8_t *g_stream;
static size_t g_len, g_cap;
static void put(const void *p, size_t n)
{
    if (g_len + n > g_cap) {
        g_cap = (g_len + n) * 2;
        g_stream = realloc(g_stream, g_cap);
    }
    memcpy(g_stream + g_len, p, n);
    g_len += n;
}

int main(void)
{
    mkdir("expect", 0755);
    int hashed = getenv("HASH") != NULL;
    static uint64_t top_hash[TOP_LINES], bot_hash[BOT_LINES];
    const uint32_t top_pitch = 240 * 4 + 64; /* ストライドに余白がある場合も試す */
    const uint32_t bot_pitch = 240 * 3;
    uint8_t *top[2], *bot[2];
    for (int i = 0; i < 2; ++i) {
        top[i] = calloc(TOP_LINES, top_pitch);
        bot[i] = calloc(BOT_LINES, bot_pitch);
    }
    uint8_t *buf = malloc(rec_frame_max_size(TOP_LINES, REC_FMT_RGBA8) + 65536);

    rec_screen_state st_top, st_bot;
    rec_screen_init(&st_top, REC_SCREEN_TOP_LEFT, TOP_LINES, 120);
    rec_screen_init(&st_bot, REC_SCREEN_BOTTOM, BOT_LINES, 120);
    g_pack = getenv("PACK") != NULL;
    st_top.pack_rgba8 = (uint8_t)g_pack;
    g_565 = getenv("RGB565") != NULL;
    st_top.to_rgb565 = st_bot.to_rgb565 = (uint8_t)g_565;

    uint64_t tick0 = 123456789ull;
    uint8_t hdr[REC_FILE_HEADER_SIZE];
    rec_write_file_header(hdr, tick0, 0);

    size_t total = 0, raw_total = 0;
    int written_frames = 0;
    for (int f = 0; f < 300; ++f) { /* 60Hzで5秒分キャプチャ */
        uint64_t tick = tick0 + (uint64_t)f * TICKS_PER_60HZ + 1000;
        int cur = hashed ? 0 : (f & 1); /* HASH: 旧3DSと同じくバッファ1枚 */
        /* 前半 RGB565 / 後半 RGBA8（途中で形式が変わるゲームを想定） */
        uint8_t tfmt = f < 150 ? REC_FMT_RGB565 : REC_FMT_RGBA8;
        int game_frame = f / 2; /* ゲームは30fps: 2回に1回は同じ絵 */
        memset(top[cur], 0xcc, (size_t)TOP_LINES * top_pitch); /* 余白にゴミ */
        draw(top[cur], top_pitch, tfmt, TOP_LINES, game_frame);
        size_t n = hashed ? rec_encode_frame_hashed(&st_top, top[cur], top_pitch, tfmt, tick, buf, top_hash)
                          : rec_encode_frame(&st_top, top[cur], f ? top[cur ^ 1] : NULL, top_pitch, tfmt, tick, buf);
        if (n) {
            put(buf, n);
            dump_expect(top[cur], top_pitch, tfmt, TOP_LINES, 0, st_top.frame_count - 1);
            written_frames++;
        }
        total += n;
        raw_total += (size_t)TOP_LINES * 240 * rec_bpp(tfmt);

        /* 下画面は1秒ごとにだけ変わる */
        draw(bot[cur], bot_pitch, REC_FMT_BGR8, BOT_LINES, (f / 60) * 20);
        n = hashed ? rec_encode_frame_hashed(&st_bot, bot[cur], bot_pitch, REC_FMT_BGR8, tick + 10, buf, bot_hash)
                   : rec_encode_frame(&st_bot, bot[cur], f ? bot[cur ^ 1] : NULL, bot_pitch, REC_FMT_BGR8, tick + 10, buf);
        if (n) {
            put(buf, n);
            dump_expect(bot[cur], bot_pitch, REC_FMT_BGR8, BOT_LINES, 1, st_bot.frame_count - 1);
        }
        total += n;
        raw_total += (size_t)BOT_LINES * 240 * 3;

        /* 音声: 32728Hz ステレオ、60Hzごとに ~545 サンプル */
        int16_t pcm[546 * 2];
        int nframes = (f % 3 == 2) ? 546 : 545;
        for (int i = 0; i < nframes; ++i) {
            double s = sin(2 * M_PI * 440 * (f * 545.47 + i) / 32728.0);
            pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(s * 8000);
        }
        n = rec_write_audio(buf, tick + 20, 32728, 2, pcm, (uint32_t)nframes);
        put(buf, n);
    }
    put(buf, rec_write_eos(buf, tick0 + 300ull * TICKS_PER_60HZ));

    /* SPLIT: 256KB 境界で2ファイルに分け、最後に事前確保ぶんのゼロを付ける（3DS の書き方と同じ） */
    size_t cut = getenv("SPLIT") ? (g_len / 2) / (256 * 1024) * (256 * 1024) : g_len;
    FILE *out = fopen("out.3dr", "wb");
    fwrite(hdr, 1, sizeof hdr, out);
    fwrite(g_stream, 1, cut, out);
    fclose(out);
    if (cut < g_len) {
        out = fopen("out.part2.3dr", "wb");
        fwrite(hdr, 1, sizeof hdr, out);
        fwrite(g_stream + cut, 1, g_len - cut, out);
        static uint8_t zeros[300000];
        fwrite(zeros, 1, sizeof zeros, out);
        fclose(out);
        printf("split at %zu bytes\n", cut);
    }
    printf("top frames written=%d  video bytes=%zu (raw would be %zu, %.1f%%)\n", written_frames, total,
           raw_total, 100.0 * total / raw_total);
    return 0;
}
