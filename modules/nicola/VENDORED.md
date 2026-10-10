# 取込元（vendored）

- 取込元: https://github.com/AutoFor/cornix-oyayubi の `modules/nicola/`
- 取込元コミット: `0ac6814a9523d9839a254113cf4ec485e56b82c2`（2026-09-19T00:13:21Z）
- 取込日: 2026-10-10
- ライセンス: GPL-2.0-or-later（同梱 `LICENSE`）。ロジックの原典は eswai/qmk_firmware `keyboards/crkbd/keymaps/nicola/nicola.c`、モジュール構造は eswai/zmk-naginata。
- 取込元に対する独自変更は `modules/nicola/CHANGES.md` に記録する。

## 取込時点の原本 SHA-256

| ファイル | SHA-256 |
| --- | --- |
| CMakeLists.txt | a1c25c0de3d98a7aa4417ba6697e226ef325d4739b4b5d91288481b958f9b3ce |
| Kconfig | ad4a34a452af20497fe6766cc6d5bfa945ac6f82d166c27b2559853cb081611f |
| LICENSE | 8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643 |
| README.md | 67293257f9d1ce0fc4e15e606f3af4f1e52658788d91f24e406f5d298f57f990 |
| dts/behaviors/nicola.dtsi | d88ec6bdd6d89475c6de5c151bf4031c58cd8e179113128faad2af1a47bd0dbd |
| dts/bindings/behaviors/zmk,behavior-ime-settle.yaml | 5b6e5b66a77b8830b68422c9c3470c995ace2942d9cf7551d8f70cd9cceaddf9 |
| dts/bindings/behaviors/zmk,behavior-nicola.yaml | b531b2ad891d792130c964e8f327d198bb2aa336cd6a2d5f0b31291e2a60411d |
| include/zmk_nicola/config.h | dbe39481978b32accf9018ede15227cf6f4abce20ad979241f8c37bdeac2e067 |
| src/behavior_nicola.c | f8402096628d7286ed7772ec237de4749c58ce35e88ece5cf0f8043ee60bf4b1 |
| src/config_console.c | a3fb44a94c8f40a6dc0795284431b874ac16c09eddb2761ccb18a887f07f308f |
| src/ime_settle.c | 3219c3a4e729a39019904bbb0a8f81a0bc35ac8876c5019f7b4f26eee64b3f69 |
