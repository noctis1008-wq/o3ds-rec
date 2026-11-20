#!/usr/bin/env python3
"""
3dr2mp4 — o3ds-rec の録画ファイル(.3dr) を動画に変換する PC 側ツール

使い方:
  python3 3dr2mp4.py REC_0001.3dr                       # 上下画面を縦に並べて 60fps MP4
  python3 3dr2mp4.py REC_0001.3dr --layout top --scale 3   # 上画面のみ 1200x720（ドット感そのまま拡大）
  python3 3dr2mp4.py REC_0001.3dr --lossless            # FFV1 可逆（編集用マスター）
  python3 3dr2mp4.py REC_0001.3dr REC_0001.part2.3dr    # 4GB分割ファイルを順に連結

必要: Python 3.9+, numpy, ffmpeg（PATH上）
フォーマット仕様: docs/format.md
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import wave
from dataclasses import dataclass
from typing import BinaryIO, Iterator

import numpy as np

MAGIC = b"3DRC"
FILE_HDR = 32
CHUNK_HDR = 16
VFRM_HDR = 12
LINE_PX = 240
SCREEN_TOP_L, SCREEN_BOT, SCREEN_TOP_R = 0, 1, 2
BPP = {0: 4, 1: 3, 2: 2, 3: 2, 4: 2}


@dataclass
class Chunk:
    type: bytes
    tick: int
    payload: bytes


def _read_header(f: BinaryIO, path: str) -> tuple[int, int]:
    h = f.read(FILE_HDR)
    if len(h) < FILE_HDR or h[:4] != MAGIC:
        sys.exit(f"エラー: {path} は .3dr ファイルではありません")
    version, hdr_size, tick_hz, _flags, start_tick = struct.unpack("<HHIIQ", h[4:24])
    if version != 1:
        sys.exit(f"エラー: 未対応のバージョン {version}")
    f.seek(hdr_size)
    return tick_hz, start_tick


class PartStream:
    """分割ファイルを、各ファイルのヘッダを飛ばして1本のバイト列として読む。
    （3DS 側は 256KB 単位でデータを書き、4GB 手前で次のファイルに切り替えるので、
      チャンクがファイルの境目をまたぐことがある）"""

    def __init__(self, paths: list[str]) -> None:
        self.paths = list(paths)
        self.f: BinaryIO | None = None
        self.tick_hz = 0
        self.start_tick = 0
        self._next()

    def _next(self) -> bool:
        if self.f:
            self.f.close()
            self.f = None
        if not self.paths:
            return False
        p = self.paths.pop(0)
        self.f = open(p, "rb")
        hz, st = _read_header(self.f, p)
        if not self.tick_hz:
            self.tick_hz, self.start_tick = hz, st
        return True

    def read(self, n: int) -> bytes:
        out = b""
        while len(out) < n and self.f:
            d = self.f.read(n - len(out))
            if not d:
                if not self._next():
                    break
                continue
            out += d
        return out


def read_chunks(paths: list[str]) -> tuple[int, int, Iterator[Chunk]]:
    """複数の分割ファイルを先頭ヘッダ検証つきで順に読む"""
    stream = PartStream(paths)

    def gen() -> Iterator[Chunk]:
        while True:
            h = stream.read(CHUNK_HDR)
            if len(h) < CHUNK_HDR:
                if h.strip(b"\0"):
                    print("警告: 末尾に不完全なチャンクがあります（録画中の電源断など）", file=sys.stderr)
                break
            ctype, length, tick = h[:4], *struct.unpack("<IQ", h[4:])
            if ctype == b"\0\0\0\0":
                break  # 事前確保した領域の残り（ここでデータ終わり）
            payload = stream.read(length)
            if len(payload) < length:
                print("警告: 末尾のチャンクが途中で切れています。そこまでを変換します", file=sys.stderr)
                break
            yield Chunk(ctype, tick, payload)

    return stream.tick_hz, stream.start_tick, gen()


class ScreenDecoder:
    """行差分を適用してフレームバッファ（ネイティブ形式・パック済み）を復元する"""

    def __init__(self) -> None:
        self.buf: np.ndarray | None = None  # (lines, line_bytes) uint8
        self.format = 0
        self.frame_no = -1

    def apply(self, payload: bytes) -> int:
        screen, fmt, lines, line_bytes, flags, frame_no = struct.unpack("<BBHHHI", payload[:VFRM_HDR])
        bm_len = (lines + 7) // 8
        bitmap = np.frombuffer(payload, np.uint8, bm_len, VFRM_HDR)
        changed = np.flatnonzero(np.unpackbits(bitmap, bitorder="little")[:lines])
        data = np.frombuffer(payload, np.uint8, len(changed) * line_bytes, VFRM_HDR + bm_len)
        if self.buf is None or self.buf.shape != (lines, line_bytes) or fmt != self.format:
            if not flags & 1:
                raise ValueError("キーフレームより前に差分フレームがあります")
            self.buf = np.zeros((lines, line_bytes), np.uint8)
        self.format = fmt
        self.buf[changed] = data.reshape(len(changed), line_bytes)
        self.frame_no = frame_no
        return screen

    def rgb(self) -> np.ndarray:
        """画面の向き（横長）の RGB24 画像 (240, lines, 3) を返す"""
        assert self.buf is not None
        px = self.buf.reshape(self.buf.shape[0], LINE_PX, BPP[self.format])
        rgb = to_rgb(px, self.format)
        # フレームバッファは90度回転: 1ライン=画面の縦1列、ライン内の先頭ピクセルが画面の一番下
        return np.ascontiguousarray(np.flipud(rgb.transpose(1, 0, 2)))


def _expand(v: np.ndarray, bits: int) -> np.ndarray:
    v = v.astype(np.uint16)
    return ((v << (8 - bits)) | (v >> (2 * bits - 8) if bits >= 4 else 0)).astype(np.uint8)


def to_rgb(px: np.ndarray, fmt: int) -> np.ndarray:
    if fmt == 0:  # RGBA8: メモリ上 A,B,G,R
        return px[..., [3, 2, 1]]
    if fmt == 1:  # BGR8: メモリ上 B,G,R
        return px[..., [2, 1, 0]]
    v = px[..., 0].astype(np.uint16) | (px[..., 1].astype(np.uint16) << 8)
    if fmt == 2:  # RGB565
        r, g, b = _expand(v >> 11, 5), _expand((v >> 5) & 63, 6), _expand(v & 31, 5)
    elif fmt == 3:  # RGB5A1
        r, g, b = _expand(v >> 11, 5), _expand((v >> 6) & 31, 5), _expand((v >> 1) & 31, 5)
    elif fmt == 4:  # RGBA4
        r, g, b = _expand(v >> 12, 4), _expand((v >> 8) & 15, 4), _expand((v >> 4) & 15, 4)
    else:
        raise ValueError(f"未対応のピクセル形式 {fmt}")
    return np.stack([r, g, b], axis=-1)


def compose(layout: str, top: np.ndarray | None, bot: np.ndarray | None) -> np.ndarray:
    blank_top = np.zeros((240, 400, 3), np.uint8)
    blank_bot = np.zeros((240, 320, 3), np.uint8)
    t = top if top is not None else blank_top
    b = bot if bot is not None else blank_bot
    if layout == "top":
        return t
    if layout == "bottom":
        return b
    canvas = np.zeros((480, t.shape[1], 3), np.uint8)
    canvas[:240, : t.shape[1]] = t
    x = (t.shape[1] - b.shape[1]) // 2
    canvas[240:, x : x + b.shape[1]] = b
    return canvas


def main() -> None:
    ap = argparse.ArgumentParser(description=".3dr → 動画変換")
    ap.add_argument("inputs", nargs="+", help=".3dr ファイル（分割ファイルは順番に並べる）")
    ap.add_argument("-o", "--output", help="出力ファイル名（既定: 入力名.mp4 / --lossless 時 .mkv）")
    ap.add_argument("--fps", type=float, default=60.0, help="出力フレームレート（既定 60）")
    ap.add_argument("--layout", choices=["both", "top", "bottom"], default="both")
    ap.add_argument("--scale", type=int, default=1, help="整数倍拡大（ニアレストネイバー）")
    ap.add_argument("--crf", type=int, default=12, help="x264 の画質（小さいほど高画質）")
    ap.add_argument("--lossless", action="store_true", help="FFV1 で完全可逆出力")
    ap.add_argument("--dump-raw", help="（テスト用）復元したネイティブバッファを書き出すディレクトリ")
    args = ap.parse_args()

    tick_hz, start_tick, chunks = read_chunks(args.inputs)
    out = args.output or os.path.splitext(args.inputs[0])[0] + (".mkv" if args.lossless else ".mp4")

    decoders = {SCREEN_TOP_L: ScreenDecoder(), SCREEN_BOT: ScreenDecoder()}
    have = {SCREEN_TOP_L: False, SCREEN_BOT: False}
    frame_period = 1.0 / args.fps
    next_out = 0
    ff = None
    size = None
    audio_tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    audio_tmp.close()
    wav = None
    audio_written = 0
    audio_rate = 0
    end_t = 0.0
    n_in = 0

    def current_image() -> np.ndarray:
        top = decoders[SCREEN_TOP_L].rgb() if have[SCREEN_TOP_L] else None
        bot = decoders[SCREEN_BOT].rgb() if have[SCREEN_BOT] else None
        return compose(args.layout, top, bot)

    def emit_until(t: float) -> None:
        """時刻 t の直前までの出力フレームを、現在の画面内容で埋める"""
        nonlocal next_out, ff, size
        if not any(have.values()):
            return
        img = None
        while next_out * frame_period < t:
            if img is None:
                img = current_image()
                if ff is None:
                    size = (img.shape[1], img.shape[0])
                    ff = start_ffmpeg(args, size, out)
            ff.stdin.write(img.tobytes())
            next_out += 1

    for ch in chunks:
        t = (ch.tick - start_tick) / tick_hz
        end_t = max(end_t, t)
        if ch.type == b"VFRM":
            emit_until(t)  # このフレームが表示される前までは前の画面
            screen = struct.unpack_from("<B", ch.payload)[0]
            if screen == SCREEN_TOP_R:
                continue  # 立体視の右目は今は使わない
            decoders[screen].apply(ch.payload)
            have[screen] = True
            n_in += 1
            if args.dump_raw:
                d = decoders[screen]
                os.makedirs(args.dump_raw, exist_ok=True)
                with open(os.path.join(args.dump_raw, f"s{screen}_{d.frame_no:06d}.bin"), "wb") as f:
                    f.write(d.buf.tobytes())
        elif ch.type == b"AUDI":
            rate, chans, afmt = struct.unpack_from("<IHH", ch.payload)
            if afmt != 1:
                continue
            if wav is None:
                wav = wave.open(audio_tmp.name, "wb")
                wav.setnchannels(chans)
                wav.setsampwidth(2)
                wav.setframerate(rate)
                audio_rate = rate
            # ティックに合わせて無音で位置合わせ（取りこぼし対策）
            want = int(round(t * audio_rate))
            if want > audio_written + audio_rate // 10:
                wav.writeframes(b"\0" * 2 * chans * (want - audio_written))
                audio_written = want
            data = ch.payload[8:]
            wav.writeframes(data)
            audio_written += len(data) // (2 * chans)
        elif ch.type == b"EOS ":
            pass

    emit_until(end_t + frame_period)
    if ff is None:
        sys.exit("エラー: 映像フレームが1枚もありません")
    ff.stdin.close()
    ff.wait()
    if wav is not None:
        wav.close()
        mux_audio(out, audio_tmp.name, args.lossless)
    else:
        os.replace(out + ".video" + os.path.splitext(out)[1], out)
    os.unlink(audio_tmp.name)
    print(f"完了: {out}  入力 {n_in} フレーム → 出力 {next_out} フレーム ({next_out / args.fps:.1f} 秒)")


def start_ffmpeg(args, size, out) -> subprocess.Popen:
    w, h = size
    vf = []
    if args.scale > 1:
        vf.append(f"scale=iw*{args.scale}:ih*{args.scale}:flags=neighbor")
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", f"{w}x{h}", "-r", str(args.fps), "-i", "-"]
    if vf:
        cmd += ["-vf", ",".join(vf)]
    if args.lossless:
        cmd += ["-c:v", "ffv1", "-level", "3", "-pix_fmt", "bgr0"]
    else:
        cmd += ["-c:v", "libx264", "-preset", "medium", "-crf", str(args.crf), "-pix_fmt", "yuv420p"]
    cmd.append(out + ".video" + os.path.splitext(out)[1])
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def mux_audio(out: str, wav_path: str, lossless: bool) -> None:
    video = out + ".video" + os.path.splitext(out)[1]
    acodec = ["-c:a", "flac"] if lossless else ["-c:a", "aac", "-b:a", "192k"]
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", video, "-i", wav_path,
                    "-c:v", "copy", *acodec, out], check=True)
    os.unlink(video)


if __name__ == "__main__":
    main()
