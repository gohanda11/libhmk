# アドバイザー検査結果: pipeline/01_report.md の検証

- 検査日: 2026-09-11
- 検査方法: 読み取り専用 git コマンドのみ (`git log`, `git show`, `git diff`, `git show --numstat/--stat`)。ソース編集・ビルド・git 書き込み操作は未実施。

## 判定: 合格（軽微な数値誤記1件あり、修正推奨）

レポートの【事実】主張は diff / show でほぼ全て裏付けられた。根拠なき断定、差分ファイルの記述漏れ、範囲逸脱は認められなかった。唯一、`da067a3` の `src/pointing_device.c` の増減行数の記述に誤りがある（後述 F-1）。

---

## 検証した主張と結果の一覧

| # | 主張（レポート該当箇所） | 検証コマンド | 結果 |
|---|---|---|---|
| 1 | 追加コミットは `b78dc53` / `da067a3` の2件のみ（L6-8） | `git log --oneline 8937794..mass-work/dual-ball` | ✅ 一致。日付 (2026-09-06 / 2026-09-09)・Author も一致 |
| 2 | 差分は9ファイル、454+/41-（L9-18, 付録） | `git diff --stat 8937794..mass-work/dual-ball` | ✅ 9ファイル 454 insertions / 41 deletions で完全一致 |
| 3 | `b78dc53` --stat は7ファイル（L28） | `git show --stat b78dc53` | ✅ 一致（7ファイル 25+/15-） |
| 4 | `da067a3` --stat は6ファイル（L41） | `git show --stat da067a3` | ✅ ファイル一覧は一致（増減行数は F-1 参照） |
| 5 | `SPLIT_PROTOCOL_VERSION 2` 不変、プロトコルヘッダ差分なし（L124, L168） | `git diff 8937794..mass-work/dual-ball -- include/split_protocol.h include/split.h` → 差分なし。両 rev で `SPLIT_PROTOCOL_VERSION 2` | ✅ 一致 |
| 6 | `eeconfig_t` / `EECONFIG_VERSION 0x0109` 不変（L157） | `git diff ... -- include/eeconfig.h` → 差分なし。両 rev で `0x0109` | ✅ 一致 |
| 7 | `dual_ball_sensitivity_config_t` は8バイトで `WL_VIRTUAL_SIZE` 末尾（L90-92, L158） | `git diff ... -- src/pointing_device.c` | ✅ `uint16_t + uint8_t×6` (packed) = 8バイト。`DUAL_BALL_SENS_ADDR = WL_VIRTUAL_SIZE - sizeof(...)` と `_Static_assert` の存在を確認。magic `0xB47B` / version 1 / 既定 50/50/70/70 / checksum 0xA5 起点 XOR も一致 |
| 8 | split60he の `WL_VIRTUAL_SIZE` = 16384（L158） | `git show mass-work/dual-ball:keyboards/split60he/keyboard.json` | ✅ `"virtual_size": 16384` を確認 |
| 9 | pmw3610.c `0x0D → 0xFD` 2箇所、コンパイルガード無し、全構成波及（L120-122, L173） | `git diff ... -- src/sensors/pmw3610.c` | ✅ `pmw3610_set_enabled(true)` 内と `pmw3610_init()` 内の2箇所。`#if` ガード無し、0xFD の define 定義無し。インデント崩れの指摘も diff 上確認 |
| 10 | layout.c キー番号ハードコード 14-17, 46-48, 50 とアクション対応（L136） | `git diff ... -- src/layout.c` | ✅ 14:L_PTR_DOWN / 15:L_PTR_UP / 16:L_SCROLL_DOWN / 17:L_SCROLL_UP / 46:R_PTR_DOWN / 47:R_PTR_UP / 48:R_SCROLL_DOWN / 50:R_SCROLL_UP で完全一致。guard は `POINTING_DEVICE_ENABLED && SPLIT_KEYBOARD && POINTING_DEVICE_SIDE_BOTH` |
| 11 | `b78dc53` の条件置換: pointing_device.c 2箇所・split.c 3箇所（L34-35, L76-84, L125） | `git show b78dc53 -- src/pointing_device.c` / `git diff ... -- src/split.c` | ✅ 一致。`ON_REMOTE_HALF` 化、単一時は `REMOTE == !THIS` で等価という説明もヘッダ diff と整合 |
| 12 | `b78dc53` 時点に `dual_ball_*` 非存在（L36） | `git show b78dc53:src/pointing_device.c` で grep | ✅ 0件 |
| 13 | include/pointing_device.h の BOTH 分岐・感度API・定数群（L55-70） | `git diff ... -- include/pointing_device.h` | ✅ `MIN 10/MAX 200/STEP 10`、アクション 0-7 + COUNT 8、`SPLIT_COMMAND_BASE 0x20`、2関数プロトタイプ、guard 条件すべて一致 |
| 14 | split.c: スレーブ側 `0x20..0x27` 退避→タスク末尾遅延適用（L47, L126-131） | `git diff ... -- src/split.c` | ✅ `slave_pointing_sens_action_pending = 0xFF` 初期値、`RECALIBRATE` 以外の `0x20..0x27` 格納、タスク末尾の遅延 `pointing_device_adjust_sensitivity_local()` を確認 |
| 15 | keyboard.json: side right→both、AML 2→-1、scroll 3→-1、ピン等不変（L146-147） | `git diff ... -- keyboards/split60he/keyboard.json` | ✅ 一致 |
| 16 | make.py / validate.py / keyboard.py の各変更（L141-143） | `git diff ... -- scripts/` | ✅ 3ファイルとも記述通り（BOTH 時は LEFT/RIGHT 未定義、validate は `side == side or side == "both"`） |

## 修正が必要な指摘

### F-1【軽微・数値誤記】L41: `src/pointing_device.c (+341/-11相当の大規模追加)`
- **何が**: `da067a3` の `src/pointing_device.c` の増減を「+341/-11相当」と記載。
- **なぜおかしいか**: `git show --numstat da067a3` では **+309/-21**（`--stat` 表示 330 行）。全範囲 `8937794..mass-work/dual-ball` の numstat でも +314/-27。どちらとも一致しない。
- **どう修正すべきか**: 「+309/-21」に訂正（「相当」と曖昧にせず numstat の実値を記載）。本質的な内容（大規模追加であること）には影響しない。

## 指摘なしと判断した観点

- **根拠なき断定**: 【事実】表記の項目は全て diff/show で再現確認できた。【推測】表記（0xFD の意図、50 vs 70 の根拠等）は適切に推測と明示されており問題なし。
- **見落とし**: `git diff --stat` の9ファイル全てに §2 の対応セクションがあり、記述漏れなし。pmw3610.c のインデント崩れ・`split.c` の "Step5" 孤立コメント等の細部も言及済み。
- **範囲逸脱**: §3・§4 は調査レポートの分析・リスク欄として妥当な範囲。依頼外の内容（無関係ブランチ、ビルド結果の断定等）は含まれない。実機効果・体感遅延は「未検証」と明示されており誠実。
