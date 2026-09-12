# 調査指示: mass-work/libhmk フォーク dual-ball ブランチの変更内容

## 背景

このワークスペースは Hall-effect キーボードファームウェア `libhmk` の git リポジトリ。
GitHub 上のフォーク `https://github.com/mass-work/libhmk` の `dual-ball` ブランチを、リモート追跡ブランチ `mass-work/dual-ball` として fetch 済み。

- 分岐点（merge-base）: コミット `8937794`（"fix(split): add 150us inter-frame gap to stop 1-byte RX FIFO overruns"）
- フォーク側で追加されたコミットは 2 つのみ:
  - `b78dc53` "Add dual-ball support to split60he"
  - `da067a3` "Add dual PMW3610 trackball support"
- 差分対象ファイル（`git diff --stat 8937794..mass-work/dual-ball` の結果）:
  - include/pointing_device.h
  - keyboards/split60he/keyboard.json
  - scripts/make.py
  - scripts/schema/keyboard.py
  - scripts/validate.py
  - src/layout.c
  - src/pointing_device.c
  - src/sensors/pmw3610.c
  - src/split.c

## 調査してほしいこと

`git show b78dc53`、`git show da067a3`、`git diff 8937794..mass-work/dual-ball -- <file>` などを使い、フォークで加えられた変更を正確に調査すること。

1. **各コミットの目的と変更内容の要約**（2 コミットそれぞれ）。
2. **ファイルごとの具体的な変更点**:
   - `include/pointing_device.h`: どんな構造体・定数・関数宣言が追加/変更されたか
   - `src/pointing_device.c`: デュアルトラックボールをどう実現しているか（2 台目センサーの初期化・ポーリング・レポート合成の方法）
   - `src/sensors/pmw3610.c`: センサードライバへの変更
   - `src/split.c`: スプリット通信プロトコルへの変更（パケット形式、バージョン等）
   - `src/layout.c`: なぜ layout に変更が入ったか
   - `scripts/schema/keyboard.py` / `scripts/validate.py` / `scripts/make.py`: keyboard.json スキーマや生成コードへの変更（`pointing_device` が単数→配列になった等）
   - `keyboards/split60he/keyboard.json`: 設定上の変更
3. **設計上の特徴**: 2 台目トラックボールを「独立した 2 つ目のポインティングデバイス」として扱うのか、既存の 1 台枠組みを拡張したのか。EEPROM 設定（eeconfig）や動的設定の扱いはどうか。
4. **互換性・リスク**: スプリットプロトコルのバージョン変更の有無、既存の単一トラックボール構成への影響。

## 出力

- 日本語で、調査レポートを `pipeline/01_report.md` に書き出すこと（ファイル変更禁止の権限でもレポート作成のため Write は許可されている想定。だめなら stdout に出力）。
- レポートは上記 1〜4 の構成で、具体的な関数名・構造体名・ファイル名を引用すること。
- 推測と事実を区別すること。

## 禁止事項

- ソースコードの編集・ビルド・git 操作（fetch/commit 等）は禁止。読み取り専用の git コマンド（show/diff/log）は使用可。
