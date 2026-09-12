# アドバイザー検査: 調査レポートの検証

ワークスペースの `pipeline/01_report.md` は、git フォーク `mass-work/dual-ball` ブランチの変更内容を調査したレポート。分岐点は `8937794`、フォーク側の追加コミットは `b78dc53` と `da067a3` の2件。

あなたの仕事:
1. `pipeline/01_report.md` を読む。
2. レポート内の【事実】とされた主張を、読み取り専用 git コマンド（`git show b78dc53`、`git show da067a3`、`git diff 8937794..mass-work/dual-ball -- <file>`、`git show <rev>:<path>`）で**実際に検証**する。少なくとも以下の重要主張を確認:
   - 追加コミットが2件のみであること
   - `SPLIT_PROTOCOL_VERSION` が不変であること
   - `eeconfig_t` / `EECONFIG_VERSION` が不変であること
   - `dual_ball_sensitivity_config_t` が8バイトで `WL_VIRTUAL_SIZE` 末尾に置かれること
   - pmw3610.c の `0x0D → 0xFD` 変更にコンパイルガードが無いこと（全構成に波及するか）
   - layout.c のキー番号ハードコード（14-17, 46-48, 50）の記述が正確か
3. 以下を指摘する:
   - 根拠なき断定（diff で確認できない【事実】表記）
   - 見落とし（差分ファイルに記述漏れがないか、`git diff --stat` と照合）
   - 範囲逸脱（依頼外の内容）
4. 検査結果を `pipeline/02_advisor_check.md` に書き出す。形式:
   - 合格/不合格の判定
   - 検証した主張と結果の一覧
   - 修正が必要な指摘（あれば具体的に）

禁止: ソース編集、ビルド、git 書き込み操作。
