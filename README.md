# o3ds-rec — 旧3DSでキャプチャ基板なしにゲームを録画する

旧3DS（初期型 / LL / 2DS）では NTR 系の Wi-Fi 配信が 3〜7fps しか出ない。原因は 3DS 側で JPEG 圧縮する CPU 時間。
このプロジェクトは**圧縮をやめて、変化した部分だけを SD カードにロスレスで書き、PC で動画に変換する**。

詳しい分析と計画: [docs/plan.md](docs/plan.md) / ファイル形式: [docs/format.md](docs/format.md)

## 構成

```
core/            録画コア（標準Cのみ。3DS実機・PCテスト共通）
tools/convert/   3dr2mp4.py … .3dr → MP4 / FFV1 変換（PC）
tools/sdbench/   sdbench.3dsx … 旧3DSのSD書き込み速度・録画パイプライン計測
test/            往復テスト（Cで録画 → Pythonで復元 → バイト一致と向きを確認）
```

## 使い方（現時点＝フェーズ0）

### PC側テスト
```sh
bash test/run_test.sh            # 要: gcc, python3 + numpy, ffmpeg
```

### 実機ベンチ
1. devkitPro（3ds-dev）を入れて `make -C tools/sdbench`、または GitHub Actions の成果物を取得
2. `sdbench.3dsx` を SD の `/3ds/` に置き Homebrew Launcher から起動
3. `sd:/o3ds-rec/bench_result.txt` に結果が残る

### 変換
```sh
python3 tools/convert/3dr2mp4.py REC_0001.3dr                 # 上下画面 60fps MP4
python3 tools/convert/3dr2mp4.py REC_0001.3dr --layout top --scale 3   # 1200x720 ドット拡大
python3 tools/convert/3dr2mp4.py REC_0001.3dr --lossless      # 編集用 FFV1 可逆
```

## ライセンス
フェーズ1で NTR-HR（GPL-3.0）をフォークするため、本リポジトリも GPL-3.0 とする予定。
