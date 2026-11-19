#!/usr/bin/env bash
# 往復テスト: Cエンコーダ → .3dr → Python変換 → バイト一致・向き・動画出力を確認
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
cd "$WORK"
gcc -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -o gen "$HERE/gen_test.c" "$HERE/../core/rec_core.c" -lm
./gen
${PACK:+echo "(RGBA8→BGR8 パック有効)"}
python3 "$HERE/../tools/convert/3dr2mp4.py" out.3dr -o both.mp4 --dump-raw got
n=0
for f in expect/*.bin; do
  cmp -s "$f" "got/$(basename "$f")" || { echo "不一致: $f"; exit 1; }
  n=$((n+1))
done
echo "バイト一致: $n バッファ"
python3 "$HERE/../tools/convert/3dr2mp4.py" out.3dr -o top.mkv --layout top --lossless
# 可逆出力の1フレーム目で向きを確認（左上=赤、右下=青）
ffmpeg -loglevel error -i top.mkv -frames:v 1 -f rawvideo -pix_fmt rgb24 f0.rgb
python3 - <<'EOF'
import numpy as np
img = np.fromfile("f0.rgb", np.uint8).reshape(240, 400, 3)
assert tuple(img[0, 0]) == (255, 0, 0), img[0, 0]
assert tuple(img[239, 399]) == (0, 0, 255), img[239, 399]
print("向きOK: 左上", img[0, 0], "右下", img[239, 399])
EOF
ffprobe -loglevel error -show_entries stream=codec_name,width,height,r_frame_rate,duration -of compact both.mp4
echo "OK ($WORK)"
