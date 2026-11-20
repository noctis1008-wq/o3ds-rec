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

## 録画のしかた（フェーズ1・実機テスト中）

録画機能は NTR-HR のフォーク [noctis1008-wq/ntr-hr](https://github.com/noctis1008-wq/ntr-hr/tree/o3ds-rec)（`o3ds-rec` ブランチ）に入っている。

1. [Releases](https://github.com/noctis1008-wq/ntr-hr/releases/latest) の `BootNTRSelector-o3ds-rec.3dsx` を SD の `/3ds/` に置く
   （Checkpoint の GitHub ダウンローダーなら「o3ds-rec NTR (SD recording)」）
2. Wi-Fi に接続した状態で、Homebrew Launcher から BootNTRSelector を起動して NTR を読み込む
3. ゲームを起動し、X+Y で NTR メニュー → Remote Play → **SD Recording: On** → Apply
4. 録画を終えるときは同じ場所で **Paused** にして Apply（ファイルが閉じられる）
5. `sd:/o3ds-rec/REC_日付_時刻.3dr` を PC にコピーして `3dr2mp4.py` で変換

- 上画面のみ・音声なし（フェーズ2で対応予定）
- 24bit 色のゲームも RGB565 で記録（30fps 優先）
- 拡張メモリを使うゲーム（Mode3 版が必要なもの）は対象外

## フェーズ0のツール

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
