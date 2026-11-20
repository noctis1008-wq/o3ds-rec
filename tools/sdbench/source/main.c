/*
 * o3ds-rec SDベンチ（.3dsx / Homebrew Launcher から起動）
 *
 * 画面表示は英語（libctru 標準コンソールは日本語フォント非対応のため）。
 *
 * 旧3DS実機で次を計測し、結果を sdmc:/o3ds-rec/bench_result.txt に保存する。
 *   A. SDへの連続書き込み速度（書き込み単位ごと）
 *   B. 録画パイプライン1フレームあたりの時間（差分検出+詰め直し / SD書き込み）
 *      → 「全ラインが毎フレーム変わる最悪ケース」で何fps録れるか
 *
 * 計測中はゲームが動いていないので、これは上限値。実際の録画では
 * ゲームとCPU・SDを取り合うぶん下がる（docs/plan.md 参照）。
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rec_core.h"

#define DIR_PATH "/o3ds-rec"
#define BENCH_FILE DIR_PATH "/bench.bin"
#define RESULT_FILE "sdmc:" DIR_PATH "/bench_result.txt"
#define TOTAL_BYTES (48u * 1024u * 1024u)

static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    gspWaitForVBlank();
}

static double ms_since(u64 t0) { return (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11; }

static bool open_bench_file(FS_Archive sdmc, Handle *file)
{
    FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, BENCH_FILE));
    Result rc = FSUSER_OpenFile(file, sdmc, fsMakePath(PATH_ASCII, BENCH_FILE),
                                FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) {
        logf_("  OpenFile failed %08lX\n", rc);
        return false;
    }
    return true;
}

/* A. 書き込み単位ごとの連続書き込み速度 */
typedef struct {
    u32 chunk;    /* 1回の書き込みサイズ */
    u32 total;    /* 合計 */
    bool prealloc; /* 先に FSFILE_SetSize でファイルを確保しておく */
    const char *label;
} write_case;

static void bench_write(FS_Archive sdmc)
{
    static const write_case cases[] = {
        {64 * 1024, TOTAL_BYTES, false, "64KB"},
        {256 * 1024, TOTAL_BYTES, false, "256KB"},
        {1024 * 1024, TOTAL_BYTES, false, "1MB"},
        {1024 * 1024, TOTAL_BYTES, true, "1MB+prealloc"},
        {256 * 1024, TOTAL_BYTES, true, "256KB+prealloc"},
        /* v0.1 の [B] が極端に遅かった原因の確認: 512B境界に揃わない書き込み */
        {192040, 4u * 1024 * 1024, false, "187KB unaligned"},
    };
    u8 *buf = linearAlloc(1024 * 1024);
    if (!buf) {
        logf_("  alloc failed\n");
        return;
    }
    for (u32 i = 0; i < 1024 * 1024; ++i)
        buf[i] = (u8)(i * 2654435761u >> 24);

    logf_("[A] SD sequential write\n");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const write_case *wc = &cases[c];
        Handle f;
        if (!open_bench_file(sdmc, &f))
            break;
        u64 t0 = svcGetSystemTick();
        if (wc->prealloc)
            FSFILE_SetSize(f, wc->total);
        u64 off = 0;
        double worst = 0;
        while (off + wc->chunk <= wc->total) {
            u32 written = 0;
            u64 t1 = svcGetSystemTick();
            Result rc = FSFILE_Write(f, &written, off, buf, wc->chunk, 0);
            double d = ms_since(t1);
            if (d > worst)
                worst = d;
            if (R_FAILED(rc) || written != wc->chunk) {
                logf_("  Write failed %08lX\n", rc);
                break;
            }
            off += wc->chunk;
        }
        FSFILE_Flush(f);
        double ms = ms_since(t0);
        FSFILE_Close(f);
        logf_("  %-15s %5.2f MB/s (worst %.0f ms)\n", wc->label,
              (double)off / (1024.0 * 1024.0) / (ms / 1000.0), worst);
    }
    linearFree(buf);
}

/*
 * B. 録画パイプライン（最悪ケース: 毎フレーム全ライン変化）
 * v0.2: エンコード結果を 1MB のステージングバッファに貯め、
 *       512B境界に揃った 1MB 単位でだけ書く（実装予定の書き込みスレッドと同じ方式）
 */
#define STAGE_BLOCK (1024u * 1024u)

static void bench_pipeline(FS_Archive sdmc, u8 fmt, int pack, bool prealloc, const char *name)
{
    const u16 lines = 400;
    const u32 pitch = REC_LINE_PX * rec_bpp(fmt);
    const int frames = 150;
    const size_t frame_max = rec_frame_max_size(lines, REC_FMT_RGBA8);
    u8 *fb[2] = {linearAlloc(lines * pitch), linearAlloc(lines * pitch)};
    u8 *stage = linearAlloc(STAGE_BLOCK + frame_max);
    Handle f = 0;
    bool opened = false;
    if (!fb[0] || !fb[1] || !stage || !(opened = open_bench_file(sdmc, &f))) {
        logf_("  setup failed\n");
        goto done;
    }
    if (prealloc)
        FSFILE_SetSize(f, 64u * 1024 * 1024);

    rec_screen_state st;
    rec_screen_init(&st, REC_SCREEN_TOP_LEFT, lines, 0);
    st.pack_rgba8 = (u8)pack;

    double enc_ms = 0, write_ms = 0, same_ms = 0, worst = 0;
    u64 off = 0, bytes = 0;
    size_t fill = 0;
    for (int i = 0; i < frames; ++i) {
        u8 *cur = fb[i & 1], *prev = fb[(i & 1) ^ 1];
        /* 全ラインを少しずつ変える（ゲームのカメラが動いている状態） */
        memset(cur, (u8)(i * 7 + 1), lines * pitch);

        u64 t0 = svcGetSystemTick();
        size_t n = rec_encode_frame(&st, cur, i ? prev : NULL, pitch, fmt, t0, stage + fill);
        enc_ms += ms_since(t0);
        fill += n;
        bytes += n;

        if (fill >= STAGE_BLOCK) {
            u64 t1 = svcGetSystemTick();
            u32 written = 0;
            FSFILE_Write(f, &written, off, stage, STAGE_BLOCK, 0);
            double d = ms_since(t1);
            write_ms += d;
            if (d > worst)
                worst = d;
            off += STAGE_BLOCK;
            fill -= STAGE_BLOCK;
            memmove(stage, stage + STAGE_BLOCK, fill);
        }

        /* 変化なしフレームの判定コスト（30fpsゲームを60Hzで見ると半分はこれ） */
        u64 t2 = svcGetSystemTick();
        (void)rec_encode_frame(&st, cur, cur, pitch, fmt, t2, stage + fill);
        same_ms += ms_since(t2);
    }

    double per = (enc_ms + write_ms) / frames;
    logf_("  %-12s%s %3luKB/f diff %.1f wr %.1f ms => %.1f fps (worst %.0f)\n", name,
          prealloc ? "+P" : "  ", (unsigned long)(bytes / frames >> 10), enc_ms / frames,
          write_ms / frames, 1000.0 / per, worst);
    (void)same_ms;
done:
    if (opened) {
        FSFILE_Flush(f);
        FSFILE_Close(f);
    }
    if (fb[0]) linearFree(fb[0]);
    if (fb[1]) linearFree(fb[1]);
    if (stage) linearFree(stage);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    bool is_new = false;
    APT_CheckNew3DS(&is_new);

    FS_Archive sdmc;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        printf("Cannot open SD. Press START.\n");
        goto wait_exit;
    }
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, DIR_PATH), 0);
    g_log = fopen(RESULT_FILE, "w");

    logf_("o3ds-rec SD bench v0.2  model: %s\n", is_new ? "New 3DS" : "Old 3DS");
    logf_("(needs ~200MB free on SD, takes 1-2 min)\n\n");

    bench_write(sdmc);

    logf_("\n[B] Record pipeline, top 400x240, worst case\n");
    bench_pipeline(sdmc, REC_FMT_RGB565, 0, false, "RGB565");
    bench_pipeline(sdmc, REC_FMT_RGB565, 0, true, "RGB565");
    bench_pipeline(sdmc, REC_FMT_BGR8, 0, true, "BGR8");
    bench_pipeline(sdmc, REC_FMT_RGBA8, 1, true, "RGBA8>BGR8");

    FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, BENCH_FILE));
    FSUSER_CloseArchive(sdmc);
    logf_("\nDone. Saved: %s\nPress START to exit.\n", RESULT_FILE);
    if (g_log)
        fclose(g_log);

wait_exit:
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
