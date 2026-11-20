/*
 * o3ds-rec : 旧3DS向け SD録画コア（プラットフォーム非依存）
 *
 * 3DS実機（libctru / NTR-HR の nwm プロセス）でも PC（テスト）でも
 * そのままビルドできるよう、標準Cのみで書いている。
 *
 * 方針: JPEG等の重い圧縮は旧3DSのCPU(268MHz)では間に合わないため、
 *       「前フレームから変化したライン（画面の縦1列=240px）だけを無圧縮で書く」
 *       行差分方式にする。CPU負荷は memcmp + memcpy 程度。
 *
 * フォーマット仕様: docs/format.md
 */
#ifndef O3DS_REC_CORE_H
#define O3DS_REC_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REC_MAGIC "3DRC"
#define REC_VERSION 1
/* ファイル先頭ヘッダ。512B にして以降の書き込みを SD のセクタ境界に揃える */
#define REC_FILE_HEADER_SIZE 512
#define REC_CHUNK_HEADER_SIZE 16
#define REC_VFRM_HEADER_SIZE 12

/* 3DS ARM11 のシステムティック周波数 */
#define REC_TICK_HZ 268111856u

/* 1ライン = 240px（3DSのフレームバッファは90度回転して格納されている） */
#define REC_LINE_PX 240

enum rec_screen {
    REC_SCREEN_TOP_LEFT = 0,
    REC_SCREEN_BOTTOM = 1,
    REC_SCREEN_TOP_RIGHT = 2,
};

/* GSP のフレームバッファ形式（GPU_FB_*_FMT の下位3bit）と同じ値 */
enum rec_pixel_format {
    REC_FMT_RGBA8 = 0,
    REC_FMT_BGR8 = 1,
    REC_FMT_RGB565 = 2,
    REC_FMT_RGB5A1 = 3,
    REC_FMT_RGBA4 = 4,
};

enum rec_frame_flags {
    REC_FRAME_KEY = 1, /* 全ラインを含む（途中から再生・破損復旧用） */
};

/* 1画面ぶんのエンコーダ状態 */
typedef struct {
    uint8_t screen;      /* rec_screen */
    uint8_t format;      /* rec_pixel_format（直前フレームと変わったら自動でキーフレーム） */
    uint16_t lines;      /* 上画面 400 / 下画面 320 */
    uint32_t frame_count;
    uint32_t key_interval; /* 何フレームごとにキーフレームを入れるか（0=最初だけ） */
    uint32_t since_key;
    uint8_t have_prev;
    uint8_t pack_rgba8; /* 1: RGBA8 のアルファを捨てて BGR8 で保存（容量 3/4） */
    uint8_t to_rgb565;  /* 1: RGBA8 / BGR8 を RGB565 に変換して保存（容量 1/2〜2/3、既定） */
} rec_screen_state;

static inline uint32_t rec_bpp(uint8_t format)
{
    return format == REC_FMT_RGBA8 ? 4u : format == REC_FMT_BGR8 ? 3u : 2u;
}

/* ビットマップ部のバイト数 */
static inline uint32_t rec_bitmap_bytes(uint16_t lines) { return (lines + 7u) / 8u; }

/* 1フレームの最大出力サイズ（出力バッファ確保用） */
static inline size_t rec_frame_max_size(uint16_t lines, uint8_t format)
{
    /* 3DS用: 確保は REC_FMT_RGBA8 で計算しておけば全形式に足りる */
    return REC_CHUNK_HEADER_SIZE + REC_VFRM_HEADER_SIZE + rec_bitmap_bytes(lines) +
           (size_t)lines * REC_LINE_PX * rec_bpp(format);
}

void rec_screen_init(rec_screen_state *st, uint8_t screen, uint16_t lines, uint32_t key_interval);

/* ファイル先頭ヘッダ（REC_FILE_HEADER_SIZE バイト）を書く。戻り値は書いたバイト数 */
size_t rec_write_file_header(uint8_t *out, uint64_t start_tick, uint32_t flags);

/*
 * 1フレームをエンコードする。
 *   cur   : 今回キャプチャしたフレームバッファ（ライン間隔 = pitch バイト）
 *   prev  : 前回キャプチャしたフレームバッファ（同じ pitch）。NULL ならキーフレーム
 *   out   : 出力先（rec_frame_max_size 以上）
 * 戻り値: 書いたバイト数。前フレームと全く同じなら 0（何も書かない）
 */
size_t rec_encode_frame(rec_screen_state *st, const uint8_t *cur, const uint8_t *prev,
                        uint32_t pitch, uint8_t format, uint64_t tick, uint8_t *out);

/*
 * 前フレームのバッファを持たずに差分を取る版（旧3DSはメモリが足りないため）。
 *   line_hash : ライン数ぶんの配列。最後に「書いた」フレームの各ラインのハッシュ。
 *               この関数が更新する。最初は中身は何でもよい（初回はキーフレームになる）
 * 戻り値は rec_encode_frame と同じ。
 * 書き込み先に空きがなくフレームを捨てる場合は、この関数を呼ばないこと
 * （呼ぶと line_hash が進み、次のフレームの差分が欠ける）。
 */
size_t rec_encode_frame_hashed(rec_screen_state *st, const uint8_t *cur, uint32_t pitch,
                               uint8_t format, uint64_t tick, uint8_t *out, uint64_t *line_hash);

/* 1ラインのハッシュ（line_bytes は 4 の倍数、line は 4 バイト境界） */
uint64_t rec_line_hash(const uint8_t *line, uint32_t line_bytes);

/* 音声チャンク（PCM16LE）。戻り値は書いたバイト数 */
size_t rec_write_audio(uint8_t *out, uint64_t tick, uint32_t sample_rate, uint16_t channels,
                       const int16_t *samples, uint32_t sample_frames);

/* 終端チャンク */
size_t rec_write_eos(uint8_t *out, uint64_t tick);

#ifdef __cplusplus
}
#endif
#endif
