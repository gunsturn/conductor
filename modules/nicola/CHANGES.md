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
