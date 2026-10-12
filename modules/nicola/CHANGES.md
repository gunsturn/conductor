# Conductor での変更履歴（取込元との差分）

取込元と原本の SHA-256 は [VENDORED.md](VENDORED.md) を参照。

## 2026-10-10

- `src/behavior_nicola.c`
  - 親指キーの押下・解放処理を `nc_thumb_pressed` / `nc_thumb_released` に共通化（判定ロジックは不変）。
  - 新 behavior `zmk,behavior-nicola-thumb`（`&ncl` / `&ncr`）を追加。第1引数が単独タップで送出するキー、第2引数が単独タップで離脱するレイヤー（`NC_KEEP_LAYER`=255 で離脱なし）。左右は DT の `side`。
  - 未確定文字の推定ガードを追加。かな送出後は `nc_pending` を立て、Enter / KP Enter / Esc / LANG2 の押下で下ろす。`nc_pending` 中はレイヤー離脱を伴う単独タップ（英数）を抑止し、`guard-repeat-ms` 以内の2度目のタップは通す。
  - `&nc` の `pending-guard`・`guard-repeat-ms` プロパティを追加。
- `dts/behaviors/nicola.dtsi`: `ncl` / `ncr` ノードと `NC_KEEP_LAYER` を追加。
- `dts/bindings/behaviors/zmk,behavior-nicola-thumb.yaml`: 新規。
- `dts/bindings/behaviors/zmk,behavior-nicola.yaml`: 2プロパティ追加。
- 未変更: `CMakeLists.txt`、`Kconfig`、`include/`、`src/config_console.c`、`src/ime_settle.c`、`LICENSE`、`README.md`（取込元の説明のまま）。

## 2026-10-10（2回目）

- `src/behavior_nicola.c` の `nmap`: 小書きかな・促音のローマ字を `l` 接頭から `x` 接頭へ変更（ぁぃぅぇぉ＝xa/xi/xu/xe/xo、ゃゅょ＝xya/xyu/xyo、っ＝xtu）。macOS の日本語IMは `l` 接頭を受け付けず「ｌつ」になるため。Google日本語入力・MS-IME・ATOK は `x` も受け付ける。

## 2026-10-10（3回目）

- `src/behavior_nicola.c`: 未確定文字の推定ガードに無操作タイムアウトを追加。かな送出後に見たキー押下（自分のローマ字出力・変換のSpace・矢印など）のたびに `nc_pending_ts` を更新し、英数の単独タップ時に最後の活動から `guard-timeout-ms` を超えていれば `nc_pending` を下ろして抑止しない。Backspaceでの全消し・マウスクリック・アプリ切替・自動確定など、Enter/Esc/LANG2 を経ない確定の後に英数が飲まれ続ける問題への対策。
- `dts/bindings/behaviors/zmk,behavior-nicola.yaml`: `guard-timeout-ms`（既定3000、0で無効）を追加。

## 2026-10-12

- `src/behavior_nicola.c`: 未確定かな数の推定を追加。かな送出で `nc_pending_n` を +1、Backspace/Delete の押下で -1 し、0 になれば推定を解除する（Backspaceで全文を消した直後の英数が1回で通る）。Space/変換を見た後の最初の Backspace は変換取消として数えない。変換中に次のかなを送ると前の文節は確定したとみなして数え直す。修飾キー付きの Backspace/Delete と、`guard-bs-hold-ms` 以上押し続けた Backspace/Delete（OSのオートリピート）は全消しとみなして解除する。解除処理は `nc_pending_clear` に集約。
- `dts/bindings/behaviors/zmk,behavior-nicola.yaml`: `guard-bs-hold-ms`（既定500、0で無効）を追加。
