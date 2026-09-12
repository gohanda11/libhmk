# 調査レポート: mass-work/libhmk `dual-ball` ブランチの変更内容

- 調査日: 2026-09-11 (UTC)
- 対象リモート追跡ブランチ: `mass-work/dual-ball`
- 分岐点 (merge-base): `8937794` "fix(split): add 150us inter-frame gap to stop 1-byte RX FIFO overruns"
- フォーク追加コミット (2件のみ):
  - `b78dc53` "Add dual-ball support to split60he" (2026-09-06, Author: mass-work <masatokondou0114@gmail.com>)
  - `da067a3` "Add dual PMW3610 trackball support" (2026-09-09, 同Author)
- 差分対象 (事実: `git diff --stat 8937794..mass-work/dual-ball` の結果と一致):
  - `include/pointing_device.h`
  - `keyboards/split60he/keyboard.json`
  - `scripts/make.py`
  - `scripts/schema/keyboard.py`
  - `scripts/validate.py`
  - `src/layout.c`
  - `src/pointing_device.c`
  - `src/sensors/pmw3610.c`
  - `src/split.c`
- 調査方法: 読み取り専用 `git show b78dc53` / `git show da067a3` / `git diff 8937794..mass-work/dual-ball -- <file>` + `git show mass-work/dual-ball:<path>` / `git show 8937794:<path>` による対照。ソース編集・ビルド・fetch/commit 等は未実施。
- 表記: 【事実】= diff/show で確認した内容。【推測】= コードコメント・挙動からの推論で明示する。

---

## 1. 各コミットの目的と変更内容の要約

### 1-1. `b78dc53` "Add dual-ball support to split60he" 【事実】

- `--stat` は7ファイル: `include/pointing_device.h`, `keyboards/split60he/keyboard.json`, `scripts/make.py`, `scripts/schema/keyboard.py`, `scripts/validate.py`, `src/pointing_device.c`, `src/split.c`。
- 目的は「両半身に各1個ずつセンサーを置けるようにする足場」を作ること。フル機能ではなく最小配管。
- 具体的内容:
  - `keyboard.json` の `pointing_device.side` に `"both"` を許可し、`split60he` の値を `"right"` → `"both"` に変更。
  - ビルドフラグ `POINTING_DEVICE_SIDE_BOTH` を新設 (`scripts/make.py`)。
  - マクロ `POINTING_DEVICE_ON_THIS_HALF` に加え `POINTING_DEVICE_ON_REMOTE_HALF` を新設 (`include/pointing_device.h`)。`SIDE_BOTH` 時は両方 `true`。単一 (`left`/`right`) 時は `REMOTE = !THIS` と定義。
  - `src/pointing_device.c` の2箇所 (`pointing_device_init()` の起動時リレー条件、`pointing_device_set_config()` のランタイムリレー条件) を `!POINTING_DEVICE_ON_THIS_HALF` → `POINTING_DEVICE_ON_REMOTE_HALF` に置換。
  - `src/split.c` の3箇所 (`split_queue_pointing_config_from_eeconfig()`、`split_master_task()` 内2箇所) を同様に `POINTING_DEVICE_ON_THIS_HALF` 否定 → `POINTING_DEVICE_ON_REMOTE_HALF` 肯定に置換。
- この時点ではレポート合成・感度調整・レイヤ連動などのロジックは無い。`BOTH` でもマスターの合成は従来通り `total_dx = local_dx + remote_dx` の単純加算になる【事実: b78dc53 時点の `src/pointing_device.c` には `dual_ball_*` が存在しないことを `git show b78dc53:src/pointing_device.c` 相当の差分で確認】。
- したがって `b78dc53` 単体は「両半身ともローカルセンサーを初期化・ポーリングし、既存 `SPLIT_FRAME_POINTING` でマスターに送り単純加算する」状態を作る中間段階と位置づけられる【推測: 意図の解釈。コード上はそう動作する】。

### 1-2. `da067a3` "Add dual PMW3610 trackball support" 【事実】

- `--stat` は6ファイル: `include/pointing_device.h`, `keyboards/split60he/keyboard.json`, `src/layout.c`, `src/pointing_device.c` (+309/-21 の大規模追加), `src/sensors/pmw3610.c`, `src/split.c`。
- 目的はデュアルボールの実機能 (独立カーソル/スクロール振り分け + 個別感度 + 永続化 + スレーブ連携 + 左センサー反転 + PMW3610 force-awake) の実装。
- 具体的内容:
  - `include/pointing_device.h` に感度調整API (`pointing_device_adjust_sensitivity()`, `pointing_device_adjust_sensitivity_local()`) と定数群 (`POINTING_SENSITIVITY_MIN/MAX/STEP`, `POINTING_SENS_ACTION_*` 8種, `POINTING_SENS_SPLIT_COMMAND_BASE 0x20`) を追加。`SPLIT_KEYBOARD && POINTING_DEVICE_SIDE_BOTH` 時のみ有効。
  - `src/pointing_device.c` に約250行の `SIDE_BOTH` 専用ブロックを追加: `dual_ball_sensitivity_config_t` 永続化、スケーリング (`dual_ball_scale_axis()` 等)、サイド別カーソル加算 (`pointing_device_add_side_cursor()`)・スクロール送信 (`pointing_device_send_side_scroll()`)、レイヤ別振り分け (`pointing_device_side_is_scroll()`)、タスク分岐、マスター適用+リレー (`pointing_device_adjust_sensitivity()`)。
  - `src/layout.c` に Layer3 専用感度キー横取り (`layout_try_pointing_sensitivity_key()` + `pointing_sensitivity_consumed` ビットマップ) を追加。
  - `src/split.c` に感度リレー受信 (`SPLIT_FRAME_CONTROL` の `0x20..0x27` を `slave_pointing_sens_action_pending` に退避→タスク末尾で `pointing_device_adjust_sensitivity_local()` を遅延実行) を追加。
  - `src/sensors/pmw3610.c` で `PMW3610_REG_PERFORMANCE` への書き値を `0x0D` → `0xFD` に変更 (2箇所)。
  - `keyboards/split60he/keyboard.json` で `auto_mouse_layer: 2 → -1`, `scroll_layer: 3 → -1` に無効化 (従来の全体制御から新・サイド別制御へ切替)。

---

## 2. ファイルごとの具体的な変更点

### 2-1. `include/pointing_device.h` 【事実】

- `b78dc53` 分:
  - `SPLIT_KEYBOARD` 配下に `POINTING_DEVICE_SIDE_BOTH` 分岐を最優先で追加:
    - `#define POINTING_DEVICE_ON_THIS_HALF (true)`
    - `#define POINTING_DEVICE_ON_REMOTE_HALF (true)`
  - 既存 `SPLIT_HANDEDNESS_USB` / `split_is_left()` 分岐にも `POINTING_DEVICE_ON_REMOTE_HALF` を追加定義。単一時は `REMOTE = !THIS` (例: `SIDE_LEFT` + USB handedness なら `THIS = split_is_master()`, `REMOTE = !split_is_master()`)。
  - 非スプリット時は `THIS=true`, `REMOTE=false`。
- `da067a3` 分:
  - `#if defined(SPLIT_KEYBOARD) && defined(POINTING_DEVICE_SIDE_BOTH)` ガードで以下を追加:
    - `#define POINTING_SENSITIVITY_MIN 10`, `MAX 200`, `STEP 10` — 単位はコメント上「percentages」。
    - `#define POINTING_SENS_ACTION_L_PTR_DOWN 0` 〜 `R_SCROLL_UP 7`, `COUNT 8` の8アクション。
    - `#define POINTING_SENS_SPLIT_COMMAND_BASE 0x20` — スプリット制御コマンド予約域。
    - `void pointing_device_adjust_sensitivity(uint8_t action);` — 「マスターで適用・永続化しスレーブへリレー」 (ヘッダコメントまま)。
    - `void pointing_device_adjust_sensitivity_local(uint8_t action);` — 「ローカルのみ適用・永続化。スレーブがリレー受信後に使用」。
- 上記以外 ( `pointing_device_init/task/get_local_delta/restore/add_remote/apply_local/set_config/get_config/reload_config` 宣言) の変更なし。

### 2-2. `src/pointing_device.c` 【事実】

差分が最大。`b78dc53` 分は2行の条件置換のみ、`da067a3` 分が本質。

#### (a) `b78dc53` による条件置換 (単一構成と等価)

- `pointing_device_init()`:
  - 変更前: `if (split_is_master() && !POINTING_DEVICE_ON_THIS_HALF)`
  - 変更後: `if (split_is_master() && POINTING_DEVICE_ON_REMOTE_HALF)`
- `pointing_device_set_config()`:
  - 変更前: `if (!POINTING_DEVICE_ON_THIS_HALF)`
  - 変更後: `if (POINTING_DEVICE_ON_REMOTE_HALF)`
- 単一 (`left`/`right`) 時は `REMOTE == !THIS` なので意味は同一。`BOTH` 時のみ「マスターもリモート(スレーブ)もセンサーあり」としてリレーするようになる。

#### (b) `da067a3` によるデュアルボール実装

1. **インクルード追加**: `#include "wear_leveling.h"` を追加。
2. **感度永続化構造体** (`SPLIT_KEYBOARD && SIDE_BOTH` 限定):
   - `typedef struct __attribute__((packed)) { uint16_t magic; uint8_t version; uint8_t left_pointer; uint8_t left_scroll; uint8_t right_pointer; uint8_t right_scroll; uint8_t checksum; } dual_ball_sensitivity_config_t;` — 計8バイト。
   - `#define DUAL_BALL_SENS_MAGIC 0xB47B`, `VERSION 1`, `DUAL_BALL_SENS_ADDR ((uint32_t)(WL_VIRTUAL_SIZE - sizeof(...)))`。
   - `_Static_assert(sizeof(eeconfig_t)+sizeof(dual_ball_sensitivity_config_t) <= WL_VIRTUAL_SIZE, ...)` で重なり防止。
   - `static dual_ball_sensitivity_config_t dual_ball_sens;` がRAM保持。
   - 関数群: `dual_ball_sens_checksum()` ( `0xA5 ^ magic_L ^ magic_H ^ version ^ 4値` )、`dual_ball_sens_value_valid()` (10..200)、`dual_ball_sens_valid()` (magic+version+範囲+checksum)、`dual_ball_sens_defaults()` (left_pointer=50, left_scroll=50, right_pointer=70, right_scroll=70)、`dual_ball_sens_save()` (`wear_leveling_write()`)、`dual_ball_sens_load()` (`wear_leveling_read()` + 不正時はdefaults+save)。
3. **感度調整**:
   - `dual_ball_adjust_value(value, increase)` — ±10して10..200にクランプ。
   - `dual_ball_apply_sensitivity_action(action)` — 8アクションを対応メンバへのポインタに解決し加減算、変化時のみsave。
   - `pointing_device_adjust_sensitivity_local(action)` — 範囲チェック (`>=COUNT` は無視) 後 local apply。
   - `pointing_device_adjust_sensitivity(action)` — local apply後、`split_is_master()` なら `(uint8_t)(POINTING_SENS_SPLIT_COMMAND_BASE + action)` を `split_send_control_command()` でリレー。
4. **サイド別ルーティング**:
   - `pointing_device_side_is_scroll(is_left, layer)`: `layer==0` なら `return is_left` (左=scroll, 右=cursor)、`layer==1` なら `return !is_left` (逆)、それ以外 `false` (両方cursor)。コメント「Layer 0: left=scroll, right=cursor / Layer 1: left=cursor, right=scroll / Other: both=cursor」通り。
   - `dual_pointer_scale_rem[2][2]`, `dual_scroll_scale_rem[2][2]` (サイド×軸の剰余保持)。
   - `dual_ball_scale_axis(value, scale, *remainder)`: `total = value*scale + *rem; out = total/100; *rem = total - out*100;` の百分率スケーリング (低速保全)。
   - `dual_ball_pointer_scale(is_left)` / `dual_ball_scroll_scale(is_left)` で対応感度取得。
   - `pointing_device_add_side_cursor(is_left, dx, dy, *cursor_x, *cursor_y)` — スケール後カーソル合算。
   - `pointing_device_send_side_scroll(is_left, dx, dy)` — スケール→サイド別アキュムレータ (`dual_scroll_acc_left/right_x/y`) 加算→ `POINTING_DEVICE_SCROLL_DIVISOR` (既定32) で `pan_ticks/wheel_ticks` 化 (`wheel = -*acc_y/div`) → `hid_mouse_scroll()` + `hid_send_mouse_report()` を127ずつ分割送信。既存 `pointing_device_send_scroll()` と同型だがサイド別。
5. **ライフサイクル**:
   - `pointing_device_init()` 先頭で `dual_ball_sens_load(); // Step5 init`。
   - `pointing_device_reload_config()` 先頭で `dual_ball_sens_load(); // Step5 reload`。
6. **ポーリング頑健化**:
   - 変更前: `if (!pmw3610_initialized) return;` + `if (!enabled){clear;return;}` (ON_THIS_HALF内)。
   - 変更後: `if (pmw3610_initialized && enabled)` ガード内に `pmw3610_read_motion()` のみ。未初期化でもreturnせずマスター合成へ進める。
   - BOTH時は左半身なら生dx,dy反転 `if (split_is_left()){dx=-dx;dy=-dy;}`。
7. **マスター合成分岐**:
   - `split_is_master()`配下を `#if SIDE_BOTH`/`#else`で分岐。
   - BOTH: local/remoteを退避クリア→各サイドscrollなら`send_side_scroll`、違えば`add_side_cursor`で合算→`pointing_device_send_hid(cursor)`のみ。旧AML/scroll呼び出し無し。
   - 非BOTH: 従来通り `total=local+remote`→scroll判定→AML→send。コメント削減のみ。

### 2-3. `src/sensors/pmw3610.c` 【事実】
- 2箇所 `0x0D` → `0xFD`: `pmw3610_set_enabled(true)`内ウェイクアップ後と`pmw3610_init()`内初期設定。対象レジスタは`PMW3610_REG_PERFORMANCE(0x11)`。
- コメント: `keep the normal 4 ms tracking (0x0D) and set FORCE_AWAKE (0xF0)`。ヘッダに0xFDの定義無し。
- 【推測】`0xFD=0x0D|0xF0`で常時覚醒狙い。省電力への影響は要実測。差分のインデント崩れあり。
### 2-4. `src/split.c` 【事実】
- プロトコル定義(`split_protocol.h`/`split.h`)差分無し。バージョン・フレーム型不変。
- b78dc53分3箇所: `split_queue_pointing_config_from_eeconfig()`と`split_master_task()`内2箇所を `!ON_THIS_HALF`→`ON_REMOTE_HALF`に置換。単一時は等価、BOTH時のみ常時リモート有り扱い。
- da067a3分:
  - `static uint8_t slave_pointing_sens_action_pending=0xFF;`(BOTH限定)追加。
  - `split_slave_task()`の`SPLIT_FRAME_CONTROL`受信で`RECALIBRATE(0x01)`以外に`0x20..0x27`なら`pending=command-0x20`を格納。
  - タスク末尾で`if(pending!=0xFF){action=pending;pending=0xFF;pointing_device_adjust_sensitivity_local(action);}`遅延永続化。
  - スレーブ送信(`POINTING_DEVICE_ON_THIS_HALF`なら`get_local_delta()`→`SPLIT_FRAME_POINTING`)はコード不変だがBOTH定義で両半身有効化。
  - `split_send_control_command()`/`split_send_pointing_config()`自体不変。

### 2-5. `src/layout.c` 【事実】
- 目的: Layer3上の特定キーで感度調整を発火させるため。キーコード新設せずレイヤ+キー番号で横取り。
- `static bitmap_t pointing_sensitivity_consumed[]=MAKE_BITMAP(NUM_KEYS);`
- `layout_try_pointing_sensitivity_key(key)`: layer!=3ならfalse。14:A→L_PTR_DOWN,15:S→L_PTR_UP,16:D→L_SCROLL_DOWN,17:F→L_SCROLL_UP,46:J→R_PTR_DOWN,47:K→R_PTR_UP,48:L→R_SCROLL_DOWN,50:Quote→R_SCROLL_UP。それ以外false。ヒット時はbitmap set+`pointing_device_adjust_sensitivity(action)`+true。
- `layout_register()`冒頭で横取り時return、`layout_unregister()`冒頭でconsumed時クリア後return。
- keymap.json変更無し(Layer3は全`_______`)。物理キー番号で発火する。

### 2-6. `scripts/schema/keyboard.py`/`validate.py`/`make.py` 【事実】
- `keyboard.py`: `side:Literal[left,right]`→`Literal[left,right,both]`。コメント`both means one sensor on each half`追記。単数→配列化無し。`pointing_device`は単一オブジェクト、`pins`も単一セットのまま。
- `make.py`: 先頭に`if pd.side==both:define(POINTING_DEVICE_SIDE_BOTH)`追加、以下elif/elseでLEFT/RIGHT。BOTH時はLEFT/RIGHT未定義。ピン/CPI/angle/AML/scroll生成は単一pdから従来通り。両半身同一ピン前提。
- `validate.py`: `if pd.side==side`→`if (pd.side==side or pd.side==both)`。BOTHなら左右両半身検査に同一ピン加算。

### 2-7. `keyboards/split60he/keyboard.json` 【事実】
- `side:right`→`both`(b78dc53)。`auto_mouse_layer:2→-1`、`scroll_layer:3→-1`(da067a3で無効化)。
- 他(`pins:cs A13/sck A12/mosi PF6/miso PF6/irq PF7`,cpi800,angle0,swap false/invert_x false/invert_y true,split uart1/A9/1MHz/handedness pin C6等)不変。両半身同一配線前提。
## 3. 設計上の特徴

### 3-1. 独立2デバイスか既存1台枠拡張か
- 【事実】後者: 既存1台枠(単一pmw3610インスタンス+単一pointing_config_t+単一SPLIT_FRAME_POINTING+単一HID)を両半身で再利用しマスター合成。
  - 根拠: ドライバ新インスタンス無し。各半身が同一PMW3610ピン定義で自センサー駆動。スキーマ単数維持。新フレーム無し。HID単一ストリーム(カーソル合算、スクロールはサイド別即時送信)。
- 【事実】2台区別は論理サイド(is_left)+レイヤ+感度4値のみ。対称配線仮定。
- 【推測】実装量・互換維持と引換えに半身別CPI/回転/ピン個別化は放棄。左反転+invert_y共通設定で吸収意図と読めるが汎用性低。

### 3-2. EEPROM/動的設定
- 【事実】`eeconfig_t`/`EECONFIG_VERSION 0x0109`/`pointing_config_t`不変。既存動線(load/apply/set/reload+POINTING_CONFIGリレー)維持。BOTHでは常時リレー。
- 【事実】新感度4値(left_pointer/left_scroll/right_pointer/right_scroll、既定50/50/70/70[%])はeeconfig外。`wear_leveling_read/write(DUAL_BALL_SENS_ADDR)`直叩き。ADDR=WL_VIRTUAL_SIZE-8。split60he(16384)なら末尾8バイト。magic 0xB47B+version1+範囲10..200+checksum(0xA5起点XOR)検証。不正時はdefaults+save。
- 【事実】マスターは即save+CONTROLリレー、スレーブはpending退避→タスク末尾でlocal適用+save。両半身各々不揮発保持(既存のマスターのみ保持方針と異なる)。init/reloadでload先行。raw HID/hmkconfから不可視。
### 3-3. 操作モデル
- 【事実】`pointing_device_side_is_scroll()`: Layer0=左scroll/右cursor、Layer1=逆、他=両cursor。`layout_get_current_layer()`(マスター権威)判定。
- 【事実】旧SCROLL_LAYER/AML機構はBOTH分岐内に呼出し無し、かつsplit60heでは-1無効化のため不使用。BOTHで有効化しても無視される。
- 【事実】感度調整はLayer3+キー番号8点ハードコード。register/unregister横取りのため押下中は本来キーコード発火せず。

## 4. 互換性・リスク

### 4-1. スプリットプロトコル
- 【事実】`SPLIT_PROTOCOL_VERSION 2`不変。フレーム型(0x00..0x06)、poll flags、payload構造体不変。新感度は既存CONTROLの未使用域0x20..0x27流用。enumはRECALIBRATE 0x01のまま。
- 【事実】旧スレーブは0x20..を未知として読捨て(remaining--のみ)。新旧混在でも線路互換は保たれる設計。ただし実質はBOTHコンパイルフラグで振舞い変化のため両半身同一ビルド前提。split60heは単一ファーム+handedness pin運用のため通常問題なし。

### 4-2. 単一構成への影響
- 【事実】side left/right時は生成・検証・条件式いずれも等価(REMOTE==!THIS)。layout/感度/slave_pendingはBOTHガードでコンパイル外。
- 【事実】唯一の例外はpmw3610 0xFD化がガード無しで全構成波及する点。

### 4-3. リスク・注意点
1. PMW3610 0xFD常時覚醒が全構成波及。省電力・発熱・電流増の可能性。要実測。インデント崩れあり。
2. 左一律反転の仮定。他盤流用・angle併用時の二重反転リスク。
3. レイヤ・キー番号ハードコード。split60he専用。他盤でBOTH有効化時誤動作。
4. AML/scroll実質廃止。BOTH+AML併用は無視。ドキュメント乖離。
5. EEPROM末尾8バイト予約の将来衝突。Static_assertで検出可だが消耗・移行未検証。調整毎write。
6. スレーブ遅延saveの電源断不整合(マスターのみ新値残存)可能性。
7. 同一ピン使い回し前提。左右別GPIO構成は表現不可。
8. HID合成順(scroll→cursor固定)。影響軽微見込みも未検証。
9. BOTH移行時感度はdefaults(左50/右70)初期化。非対称既定の意図不明。eeconfig旧AML/scroll値残留も紛らわしい。
10. スタイル: pmw3610崩れ、空行重複、孤立Step5コメント、define連番等。upstreamマージ時衝突要因。

## 付録
- `git log --oneline 8937794..mass-work/dual-ball`→2件、`git diff --stat`→9ファイル454+/41-に一致確認。
- `git show b78dc53`/`da067a3`、`git diff 8937794..mass-work/dual-ball -- <file>`、`git show <rev>:<path>`対照。eeconfig/split_protocolはgrepで不変確認。
- 不明点: 0xFD実機効果、50vs70根拠、Step5工程表所在、同時操作体感遅延(ビルド・フラッシュ禁止のため未検証)。
