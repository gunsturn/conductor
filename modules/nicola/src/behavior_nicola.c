/*
 * zmk-nicola: NICOLA (親指シフト) behavior for ZMK
 *
 * ロジックは eswai/qmk_firmware keyboards/crkbd/keymaps/nicola/nicola.c
 * (Copyright 2018-2019 eswai, GPL-2.0-or-later) のタイマーレス同時押し判定を移植。
 * モジュール構造は eswai/zmk-naginata の behavior 方式に倣う。
 *
 * 原典からの拡張:
 *  - 連続シフト(オプション, continuous-shift): 親指キーの物理押下状態を別管理し、
 *    オンの場合は解決後も押下中はシフトを維持する (既定オフ)
 *  - 親指キー単独タップ: そのキー自身を送出 (既定: 左=Space / 右=変換(INT4)、DTで変更可)。
 *    ただし同時打鍵判定に関与した押下(成立/不成立問わず)は単独タップを出さない
 *    (判定失敗のたびにSpace/変換=IME変換が漏れて表示を壊すのを防ぐ)
 *  - Ctrl/Alt/GUI 押下中は素のキーコードを送出（ショートカット素通し）
 *  - ロールオーバー: 次の文字キー押下時は直前の文字だけ確定し、新しい文字は
 *    自分の判定窓を持って保留に残す (やまぶきRの後判定と同じ扱い)
 *
 * Conductor (gunsturn/conductor) での変更 (2026-10-10、詳細は modules/nicola/CHANGES.md):
 *  - 親指シフトキー専用 behavior &ncl / &ncr: 単独タップで送出するキーと、単独タップで離脱する
 *    レイヤーを binding の引数で指定する (IME 切替とレイヤー切替を 1 キーで揃えるため)
 *  - 未確定文字の推定ガード: かなを送出してから Enter / Esc / 英数の通過を見るまでは IME 上に
 *    未確定文字が残っていると推定し、左親指単独タップの英数 (IME オフ) を抑止する。
 *    guard-repeat-ms 以内の 2 度目のタップは意図的とみなして通す
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DT_DRV_COMPAT zmk_behavior_nicola

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <zmk_nicola/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---- 動作ログのストリーム出力 (Web設定ツールの表示用、set log 1 で有効・永続) ---- */
static bool nc_log_on = false;
static nc_log_sink_t nc_log_sink = NULL;

void nc_cfg_set_log_sink(nc_log_sink_t sink) { nc_log_sink = sink; }

/* 全行を "NC <ms> <msg>" 形式にする。<ms> は起動からの経過msで、再起動すると0に
 * 巻き戻るため、PC側に保存したログ上でもクラッシュ/再起動の発生位置が特定できる。
 * 呼び出し側のメッセージには "NC " を付けないこと。 */
static void nc_emit(const char *fmt, ...) {
    char msg[84];
    char buf[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(buf, sizeof(buf) - 2, "NC %u %s", (unsigned)k_uptime_get_32(), msg);
    LOG_INF("%s", buf);
    if (nc_log_on && nc_log_sink != NULL) {
        strcat(buf, "\n");
        nc_log_sink(buf);
    }
}

/* 親指シフトキー (DTの left-thumb / right-thumb で変更可、同時打鍵の判定対象)。
 * 物理位置はkeymap上のbinding位置そのもの (ZMK Studioで移動可能)。 */
static uint32_t nc_lthumb = SPACE;
static uint32_t nc_rthumb = INT4;

/* 単独タップ時に送出するキーコード (既定は nc_lthumb/nc_rthumb と同じ値だが、
 * `set lthumb`/`set rthumb` で判定対象キーとは独立にランタイム変更・永続化できる。
 * 焼き直さずにタップ出力(Space/変換/Alt/Winなど)だけを差し替えたいケース向け)。 */
static uint32_t nc_lthumb_tap = SPACE;
static uint32_t nc_rthumb_tap = INT4;

/* NICOLA 配列テーブル (JIS / IMEローマ字入力想定) — nicola.c の nmap を移植 */
struct nc_map {
    uint32_t key;
    const char *t; /* 単独 */
    const char *l; /* 左親指シフト */
    const char *r; /* 右親指シフト */
};

static const struct nc_map nmap[] = {
    {Q, ".", "xa", ""},      {W, "ka", "e", "ga"},    {E, "ta", "ri", "da"},
    {R, "ko", "xya", "go"},  {T, "sa", "re", "za"},

    {Y, "ra", "pa", "yo"},   {U, "ti", "di", "ni"},   {I, "ku", "gu", "ru"},
    {O, "tu", "du", "ma"},   {P, ",", "pi", "xe"},

    {A, "u", "wo", "vu"},    {S, "si", "a", "zi"},    {D, "te", "na", "de"},
    {F, "ke", "xyu", "ge"},  {G, "se", "mo", "ze"},

    {H, "ha", "ba", "mi"},   {J, "to", "do", "o"},    {K, "ki", "gi", "no"},
    {L, "i", "po", "xyo"},   {SEMI, "nn", "", "xtu"},

    {Z, ".", "xu", ""},      {X, "hi", "-", "bi"},    {C, "su", "ro", "zu"},
    {V, "hu", "ya", "bu"},   {B, "he", "xi", "be"},

    {N, "me", "pu", "nu"},   {M, "so", "zo", "yu"},   {COMMA, "ne", "pe", "mu"},
    {DOT, "ho", "bo", "wa"}, {SLASH, "/", "?", "xo"},

    /* NICOLA-J規格 D11: @キー 単独=、(読点)。シフト面(゛)はローマ字入力では表現不能のため未割当 */
    {LBKT, ",", "", ""},
};

/* ---- 設定 ---- */
struct behavior_nicola_config {
    int32_t timeout_ms;
    int32_t range_pct;
    int32_t cont;
    uint32_t lthumb;
    uint32_t rthumb;
    int32_t guard;           /* Conductor拡張: pending-guard */
    int32_t guard_repeat_ms; /* Conductor拡張: guard-repeat-ms */
    int32_t guard_timeout_ms; /* Conductor拡張: guard-timeout-ms */
    int32_t guard_bs_hold_ms; /* Conductor拡張: guard-bs-hold-ms */
    int32_t shift_release_ms; /* Conductor拡張: shift-release-ms */
};

/* 連続シフト (DTの continuous-shift, 既定オフ)。オンの場合、親指キーを
 * 押している間は後続の文字キーにもシフトをかけ続ける */
static bool nc_cont = false;

/* 同時打鍵判定窓 (ms)。文字キー押下から親指キー押下までがこの時間以内なら
 * 同時打鍵とみなし、その瞬間に判定・出力する。DTの timeout-ms で設定。
 * 親指キー先行(連続シフト)には適用しない。 */
static int32_t nc_timeout_ms = 130;

/* 判定方式: 0=ms方式(固定窓・先判定・即出力) 1=%方式(やまぶきR正本・後判定)。
 * 実行時に set mode で切替可能、フラッシュに保存される */
static int32_t nc_mode = 0;

/* %方式: 同時打鍵と判定される時間範囲 (%, 1-100)。やまぶきR正本:
 * 文字キー押下=0、離す(or次キー)=100として、前半◯%以内の親指押下を同時とみなす */
static int32_t nc_range_pct = 65;

/* ---- 状態 ---- */
#define NC_BUF_MAX 8

static uint32_t nc_buf[NC_BUF_MAX]; /* 未解決の文字キー */
static int64_t nc_last_chr_ts;      /* 最後の文字キー押下時刻 */

/* %方式: 文字キー保留中に親指キーが押されたときの判定待ち状態。
 * 判定はインターバル終端(文字キーrelease/次キー押下)で行う */
static uint8_t nc_judge;    /* 0=なし 1=左親指 2=右親指 */
static int64_t nc_judge_ts; /* 判定待ち親指キーの押下時刻 */
static int nc_chrcount;             /* 文字キーのカウンタ */
static int nc_keycount;             /* 親指キーも含めたカウンタ */
static bool nc_l_held, nc_r_held;   /* 親指キーの物理押下状態 */
static bool nc_l_used, nc_r_used;   /* この押下中に同時打鍵が成立したか */
/* この押下が保留文字の同時打鍵判定に関与したか (成立/不成立を問わない)。
 * 関与した親指は「シフトのつもりで押された」ものなので、離しても単独タップ
 * (Space/変換)を出さない。出すとIMEで変換が走り、ミス表示をさらに壊す */
static bool nc_l_judged, nc_r_judged;

/* ---- Conductor拡張: 連続シフト中の親指の離し遅れ ----
 * 「ょ(右親指)+う」のように、シフトした文字の次に単独面の文字を打つとき、人の指は
 * 親指を離すより先に次の文字キーを押しがちで、そのまま即時シフト確定すると「ゔ」になる。
 * 親指がすでにこの押下で文字をシフト済み (used) の状態で次の文字が押されたら即時確定せず
 * 保留し、文字押下から shift-release-ms 以内にその親指が離されたら「離し遅れ」とみなして
 * 単独面で確定する。それより長く重なれば本当の連続シフトとして確定する。
 * 文字キー自身の解放や次のキー押下で親指がまだ押されていればシフト面で確定する。 */
static bool nc_deferred;                 /* 連続シフト中の文字を保留中 */
static int32_t nc_shift_release_ms = 0; /* DT shift-release-ms (0=無効=即時確定) */

/* ---- Conductor拡張: 未確定文字の推定ガード ----
 * かなを送出してから Enter/Esc (または英数キーの通過) を見るまでは、IME上に未確定文字が
 * 残っていると推定し、左親指単独タップ (英数=IMEオフ) を抑止する。キーボードはIMEの状態を
 * 直接は知れないので、自分が送ったキーからの推定である。guard-repeat-ms 以内に同じ単独
 * タップを繰り返した場合は意図的とみなして通す。 */
static bool nc_pending;                 /* かな送出後、Enter/Esc未検出 */
static int64_t nc_pending_ts;           /* 推定中の最後のキー活動時刻 (無操作判定の基準) */
static int64_t nc_guard_ts;             /* 直近で抑止した単独タップの時刻 (0=なし) */
static bool nc_guard_on = true;         /* DT pending-guard */
static int32_t nc_guard_repeat_ms = 1000; /* DT guard-repeat-ms */
/* 推定の自動解除: 最後のキー活動からこの時間 (ms) 無操作なら未確定文字は消えたとみなす。
 * Backspaceでの全消し・マウスでの確定・アプリ切替など、キーボードから見えない確定の
 * 後に英数が飲まれ続けるのを防ぐ。0 で無効 (Enter/Esc/LANG2 のみで解除)。 */
static int32_t nc_guard_timeout_ms = 3000; /* DT guard-timeout-ms */
/* 未確定かな数の推定: かな送出で +1、Backspace/Delete の押下で -1、0 になれば推定解除。
 * Space/変換を見た後の最初の Backspace は変換の取消であって削除ではないので数えない。
 * 変換中に次のかなを送ると前の文節は確定するので数え直す。修飾キー付きの Backspace
 * (単語/行削除) と、guard-bs-hold-ms 以上の長押し (OSのオートリピート) は全消しとみなす。 */
static int nc_pending_n;                /* 推定の未確定かな数 */
static bool nc_converting;              /* Space/変換を見た後 */
static int64_t nc_bs_down_ts;           /* Backspace/Delete の押下時刻 (0=押されていない) */
static int32_t nc_guard_bs_hold_ms = 500; /* DT guard-bs-hold-ms */
#define NC_NO_LAYER 0xFF /* &ncl/&ncr の param2: 単独タップでレイヤーを離脱しない */

/* 修飾キー押下中に素通ししたキー (releaseも素通しするため記録) */
static uint32_t nc_raw[NC_BUF_MAX];
static int nc_raw_n;

/* 連続シフトを考慮した実効シフト状態。連続シフトオフの場合、
 * 同時打鍵成立済み(used)の親指は以降の文字にはシフトをかけない */
static bool nc_l_eff(void) { return nc_l_held && (nc_cont || !nc_l_used); }
static bool nc_r_eff(void) { return nc_r_held && (nc_cont || !nc_r_used); }

static void nc_recount(void) {
    nc_keycount = (nc_l_eff() ? 1 : 0) + (nc_r_eff() ? 1 : 0) + nc_chrcount;
}

static const struct nc_map *nc_find(uint32_t key) {
    for (size_t i = 0; i < ARRAY_SIZE(nmap); i++) {
        if (nmap[i].key == key) {
            return &nmap[i];
        }
    }
    return NULL;
}

/* ログ用: キーの単独面ローマ字を識別名として返す */
static const char *nc_name(uint32_t key) {
    const struct nc_map *m = nc_find(key);
    return m ? m->t : "?";
}

static void nc_tap(uint32_t encoded, int64_t ts) {
    raise_zmk_keycode_state_changed_from_encoded(encoded, true, ts);
    raise_zmk_keycode_state_changed_from_encoded(encoded, false, ts);
}

/* 解決済みローマ字列をHID送出 */
static void nc_send_romaji(const char *s, int64_t ts) {
    for (; *s != '\0'; s++) {
        uint32_t kc;
        switch (*s) {
        case '-':
            kc = MINUS;
            break;
        case ',':
            kc = COMMA;
            break;
        case '.':
            kc = DOT;
            break;
        case '/':
            kc = SLASH;
            break;
        case '?':
            kc = LS(SLASH); /* JIS: Shift+/ = ? */
            break;
        default:
            if (*s < 'a' || *s > 'z') {
                continue;
            }
            kc = A + (*s - 'a');
            break;
        }
        nc_tap(kc, ts);
    }
}

/* バッファの文字キーを指定シフト面で確定する。
 * 原典の ncl_type() + ncl_clear() に相当するが、親指キーが物理的に
 * 押されている間はシフト状態(keycount=1)を維持する(連続シフト)。 */
static void nc_pending_clear(const char *why) {
    if (nc_pending) {
        nc_emit("pending cleared: %s", why);
    }
    nc_pending = false;
    nc_pending_n = 0;
    nc_converting = false;
    nc_guard_ts = 0;
}

static void nc_type_with(int64_t ts, bool lsh, bool rsh) {
    for (int i = 0; i < nc_chrcount; i++) {
        const struct nc_map *m = nc_find(nc_buf[i]);
        if (m == NULL) {
            continue;
        }
        const char *out = lsh ? m->l : (rsh ? m->r : m->t);
        nc_emit("out '%s' plane=%c (key '%s')", out, lsh ? 'L' : (rsh ? 'R' : '-'), m->t);
        nc_send_romaji(out, ts);
        if (*out != '\0') {
            if (nc_converting) { /* 変換中の新しい入力は前の文節を確定させる */
                nc_pending_n = 0;
                nc_converting = false;
            }
            if (nc_pending_n < 1000) {
                nc_pending_n++;
            }
            nc_pending = true; /* IME上に未確定文字が生じたと推定 */
            nc_pending_ts = ts;
        }
    }
    nc_chrcount = 0;
    nc_deferred = false;
    nc_keycount = (nc_l_eff() ? 1 : 0) + (nc_r_eff() ? 1 : 0);
}

/* 現在の実効シフト状態で確定 */
static void nc_type(int64_t ts) { nc_type_with(ts, nc_l_eff(), nc_r_eff()); }

/* %方式: 判定待ちの同時打鍵をインターバル終端 end_ts で確定する。
 * やまぶきR正本: 文字キー押下(0)〜終端(文字キーrelease/次キー押下=100)のうち、
 * 親指キー押下位置が range-pct% 以内なら同時打鍵、外れたら単独打ちで確定。
 * 親指キーが先に離されていても判定は終端まで持ち越す(正本準拠)。 */
static void nc_judge_resolve(int64_t end_ts) {
    if (nc_judge == 0) {
        return;
    }
    const uint8_t judge = nc_judge;
    nc_judge = 0;
    if (nc_chrcount == 0) {
        return;
    }
    const int64_t total = end_ts - nc_last_chr_ts;
    const int64_t pos = nc_judge_ts - nc_last_chr_ts;
    const bool simul = (total <= 0) || (pos * 100 <= total * nc_range_pct);
    nc_emit("judge %cthumb: pos=%dms / total=%dms = %d%% (range=%d%%) -> %s",
            judge == 1 ? 'L' : 'R', (int32_t)pos, (int32_t)total,
            total > 0 ? (int32_t)(pos * 100 / total) : 0, nc_range_pct,
            simul ? "SIMUL" : "LATE(single)");
    if (simul) {
        nc_type_with(end_ts, judge == 1, judge == 2);
        if (judge == 1) {
            nc_l_used = true;
        } else {
            nc_r_used = true;
        }
        nc_recount();
    } else {
        nc_type_with(end_ts, false, false);
    }
}

/* 保留中をすべて吐き出す (判定待ちがあれば判定してから) */
static void nc_flush(int64_t ts) {
    if (nc_judge != 0) {
        nc_judge_resolve(ts);
    } else {
        nc_type(ts);
    }
}

static void nc_reset(void) {
    nc_chrcount = 0;
    nc_keycount = 0;
    nc_l_held = nc_r_held = false;
    nc_l_used = nc_r_used = false;
    nc_l_judged = nc_r_judged = false;
    nc_raw_n = 0;
    nc_judge = 0;
    nc_deferred = false;
}

static bool nc_mods_active(void) {
    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    return mods != 0;
}

/* ---- Conductor拡張: 親指キー処理の共通化 (&nc の親指キーと &ncl/&ncr の両方から呼ぶ) ---- */

static void nc_thumb_pressed(bool left, int64_t ts) {
    bool *held = left ? &nc_l_held : &nc_r_held;
    bool *used = left ? &nc_l_used : &nc_r_used;
    bool *judged = left ? &nc_l_judged : &nc_r_judged;
    const char side = left ? 'L' : 'R';
    const uint8_t judge_id = left ? 1 : 2;

    if (nc_chrcount > 0) {
        *judged = true; /* 保留文字と競合: この押下は単独タップにしない */
        if (nc_mode == 1) { /* %方式: 判定はインターバル終端まで保留 */
            if (nc_judge == 0) {
                nc_judge = judge_id;
                nc_judge_ts = ts;
                nc_emit("%cthumb dn: +%dms after char, judge pending", side,
                        (int32_t)(ts - nc_last_chr_ts));
            }
        } else { /* ms方式: 窓を外れた保留文字は先に単独打ちで確定 */
            const int32_t dt = (int32_t)(ts - nc_last_chr_ts);
            if (dt > nc_timeout_ms) {
                nc_emit("%cthumb dn: dt=%dms > win=%dms -> LATE (type single first)", side, dt,
                        nc_timeout_ms);
                nc_type(ts);
            } else {
                nc_emit("%cthumb dn: dt=%dms <= win=%dms -> SIMUL", side, dt, nc_timeout_ms);
            }
        }
    } else {
        nc_emit("%cthumb dn (no pending char)", side);
    }
    *held = true;
    *used = false;
    nc_keycount++;
    if (nc_mode == 0 && nc_keycount > 1) { /* ms方式: 保留文字と同時打鍵成立 */
        nc_type(ts);
        *used = true;
        nc_recount();
    }
}

/* 単独タップの送出。leave_layer != NC_NO_LAYER のタップ (=かなモードを抜ける英数) には
 * 未確定文字の推定ガードを適用する。 */
static void nc_solo_tap(int64_t ts, uint32_t tap_kc, uint32_t leave_layer) {
    const bool leaves = leave_layer != NC_NO_LAYER;
    if (leaves && nc_guard_on && nc_pending && nc_guard_timeout_ms > 0 &&
        (ts - nc_pending_ts) > nc_guard_timeout_ms) {
        nc_emit("pending expired: %dms idle > %dms", (int32_t)(ts - nc_pending_ts),
                nc_guard_timeout_ms);
        nc_pending_clear("idle timeout");
    }
    if (leaves && nc_guard_on && nc_pending) {
        if (nc_guard_ts != 0 && (ts - nc_guard_ts) <= nc_guard_repeat_ms) {
            nc_emit("solo tap: repeated within %dms -> pass through", nc_guard_repeat_ms);
        } else {
            nc_guard_ts = ts;
            nc_emit("solo tap: suppressed (pending kana); tap again within %dms to force",
                    nc_guard_repeat_ms);
            return;
        }
    }
    nc_guard_ts = 0;
    nc_tap(tap_kc, ts);
    if (leaves) {
        nc_pending_clear("leave layer");
        nc_emit("leave kana layer %u", leave_layer);
        zmk_keymap_layer_deactivate((zmk_keymap_layer_id_t)leave_layer);
    }
}

static void nc_thumb_released(bool left, int64_t ts, uint32_t tap_kc, uint32_t leave_layer) {
    bool *held = left ? &nc_l_held : &nc_r_held;
    bool *used = left ? &nc_l_used : &nc_r_used;
    bool *judged = left ? &nc_l_judged : &nc_r_judged;
    const char side = left ? 'L' : 'R';
    const bool tap = !*used && !*judged && nc_chrcount == 0;

    nc_emit("%cthumb up (used=%d judged=%d pend=%d)%s", side, (int)*used, (int)*judged,
            nc_chrcount, tap ? " -> solo tap" : "");
    if (nc_deferred && nc_chrcount > 0 && *held && *used) {
        const int32_t ov = (int32_t)(ts - nc_last_chr_ts);
        if (ov > nc_shift_release_ms) {
            nc_emit("%cthumb up: overlap %dms > %dms -> continuous shift, commit shifted", side,
                    ov, nc_shift_release_ms);
            nc_type(ts); /* この親指はまだ held なのでシフト面で確定 */
        } else {
            nc_emit("%cthumb up: overlap %dms <= %dms -> late release, keep '%s' unshifted", side,
                    ov, nc_shift_release_ms, nc_name(nc_buf[0]));
            nc_deferred = false; /* 文字は保留のまま。自身の解放/次キーで単独面確定 */
        }
    }
    *held = false;
    *used = false;
    *judged = false;
    if (tap) {
        nc_solo_tap(ts, tap_kc, leave_layer);
    }
    /* 親指releaseでは保留文字を確定しない (正本: 区間終端は文字release/次キー押下) */
    nc_keycount = ((left ? nc_r_eff() : nc_l_eff()) ? 1 : 0) + nc_chrcount;
}

/* Enter / Esc / 英数(LANG2) の押下を見たら未確定文字は消えたと推定する。
 * 自分が送出したローマ字もここを通るが、Enter/Esc/LANG2 は含まれない。
 * Backspace/Delete は未確定かな数を 1 減らし、0 で解除 (変換取消・修飾付き・長押しは上記参照)。
 * Space/変換は変換開始とみなす。それ以外の押下 (候補選択の矢印、自分のローマ字出力など) は
 * 「まだIMEを操作中」とみなし、無操作判定の基準時刻を更新する。 */
static int nc_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const bool is_del = ev->keycode == HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE ||
                        ev->keycode == HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD;
    if (!ev->state) {
        /* release: Backspace/Delete の長押しはオートリピートで残りも消えたとみなす */
        if (is_del && nc_bs_down_ts != 0) {
            const int64_t held = ev->timestamp - nc_bs_down_ts;
            nc_bs_down_ts = 0;
            if (nc_pending && nc_guard_bs_hold_ms > 0 && held >= nc_guard_bs_hold_ms) {
                nc_emit("delete held %dms >= %dms", (int32_t)held, nc_guard_bs_hold_ms);
                nc_pending_clear("long delete");
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (nc_pending) {
        nc_pending_ts = ev->timestamp;
    }
    if (is_del) {
        nc_bs_down_ts = ev->timestamp;
        if (!nc_pending) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (nc_mods_active()) {
            nc_pending_clear("modified delete");
        } else if (nc_converting) {
            nc_converting = false; /* 変換の取消。文字は減らない */
            nc_emit("delete during conversion -> cancel (n=%d)", nc_pending_n);
        } else {
            nc_pending_n--;
            nc_emit("delete -> n=%d", nc_pending_n);
            if (nc_pending_n <= 0) {
                nc_pending_clear("all deleted");
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    switch (ev->keycode) {
    case HID_USAGE_KEY_KEYBOARD_RETURN_ENTER:
    case HID_USAGE_KEY_KEYPAD_ENTER:
    case HID_USAGE_KEY_KEYBOARD_ESCAPE:
    case HID_USAGE_KEY_KEYBOARD_LANG2:
        nc_pending_clear("commit/cancel key");
        break;
    case HID_USAGE_KEY_KEYBOARD_SPACEBAR:
    case HID_USAGE_KEY_KEYBOARD_INTERNATIONAL4: /* 変換 */
        if (nc_pending && !nc_mods_active()) {
            nc_converting = true;
        }
        break;
    default:
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_nicola, nc_keycode_listener);
ZMK_SUBSCRIPTION(behavior_nicola, zmk_keycode_state_changed);

static int on_nicola_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    const uint32_t key = binding->param1;
    const int64_t ts = event.timestamp;

    /* Ctrl/Alt/GUI/Shift 押下中はNICOLA処理せず素通し */
    if (nc_mods_active()) {
        nc_flush(ts); /* 未解決分を吐き出してから */
        if (nc_raw_n < NC_BUF_MAX) {
            nc_raw[nc_raw_n++] = key;
        }
        raise_zmk_keycode_state_changed_from_encoded(key, true, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (key == nc_lthumb) {
        nc_thumb_pressed(true, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == nc_rthumb) {
        nc_thumb_pressed(false, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        if (nc_judge != 0) { /* %方式: 次キー押下 = 前の文字のインターバル終端 */
            nc_judge_resolve(ts);
        } else if (nc_chrcount > 0) {
            /* ロールオーバー: 直前の文字だけ確定する。新しい文字は即確定せず
             * 保留に残し、自分の判定窓(親指の後到着)を持たせる */
            nc_emit("dn '%s': rollover -> commit prev char", nc_name(key));
            nc_type(ts);
        }
        nc_chrcount = 0;
        nc_buf[nc_chrcount++] = key;
        nc_emit("dn '%s' pend=%d L=%d R=%d", nc_name(key), nc_chrcount, (int)nc_l_held,
                (int)nc_r_held);
        nc_last_chr_ts = ts;
        nc_recount();
        if (nc_l_eff() || nc_r_eff()) { /* 親指先行/連続シフト: 有効な親指があれば即時シフト確定 */
            const bool reused = nc_cont && nc_shift_release_ms > 0 &&
                                ((nc_l_held && nc_l_used) || (nc_r_held && nc_r_used));
            if (reused) { /* 連続シフト2文字目以降: 親指の離し遅れを見るため保留 */
                nc_deferred = true;
                nc_emit("dn '%s': thumb already used -> defer (release window %dms)",
                        nc_name(key), nc_shift_release_ms);
            } else {
                nc_type(ts);
                if (nc_l_held) {
                    nc_l_used = true;
                }
                if (nc_r_held) {
                    nc_r_used = true;
                }
                nc_recount();
            }
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* テーブル外のキーは素通し。%方式の判定保留中(nc_judge!=0)なら、この打鍵が
     * インターバル終端(=別キー押下)に当たるので nc_flush で確定させてから吐く */
    nc_flush(ts);
    if (nc_raw_n < NC_BUF_MAX) {
        nc_raw[nc_raw_n++] = key;
    }
    raise_zmk_keycode_state_changed_from_encoded(key, true, ts);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_nicola_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const uint32_t key = binding->param1;
    const int64_t ts = event.timestamp;

    /* 素通し中のキーはreleaseも素通し */
    for (int i = 0; i < nc_raw_n; i++) {
        if (nc_raw[i] == key) {
            for (int j = i; j < nc_raw_n - 1; j++) {
                nc_raw[j] = nc_raw[j + 1];
            }
            nc_raw_n--;
            raise_zmk_keycode_state_changed_from_encoded(key, false, ts);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    if (key == nc_lthumb) {
        nc_thumb_released(true, ts, nc_lthumb_tap, NC_NO_LAYER);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == nc_rthumb) {
        nc_thumb_released(false, ts, nc_rthumb_tap, NC_NO_LAYER);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        /* 確定するのは保留中の文字本人のreleaseのみ。ロールオーバーで
         * 既に確定済みの文字のreleaseは無視する
         * (やまぶきR正本: 区間終端は「その文字を放す」か「別のキーを押す」) */
        if (nc_chrcount > 0 && nc_buf[0] == key) {
            if (nc_judge != 0) { /* %方式: 文字キーrelease = インターバル終端で判定 */
                nc_emit("up '%s' -> interval end", nc_name(key));
                nc_judge_resolve(ts);
            } else { /* 単独打鍵: releaseで確定 */
                nc_emit("up '%s' -> commit (own release)", nc_name(key));
                nc_type(ts);
            }
        } else {
            nc_emit("up '%s' (already committed)", nc_name(key));
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_nicola_init(const struct device *dev) {
    const struct behavior_nicola_config *cfg = dev->config;
    nc_timeout_ms = CLAMP(cfg->timeout_ms, 1, 1000);
    nc_range_pct = CLAMP(cfg->range_pct, 1, 100);
    nc_cont = cfg->cont != 0;
    nc_lthumb = cfg->lthumb;
    nc_rthumb = cfg->rthumb;
    nc_lthumb_tap = cfg->lthumb; /* set lthumb で永続化されていればこの後settingsで上書きされる */
    nc_rthumb_tap = cfg->rthumb;
    nc_guard_on = cfg->guard != 0;
    nc_guard_repeat_ms = CLAMP(cfg->guard_repeat_ms, 100, 10000);
    nc_guard_timeout_ms = cfg->guard_timeout_ms <= 0 ? 0 : CLAMP(cfg->guard_timeout_ms, 500, 60000);
    nc_guard_bs_hold_ms = cfg->guard_bs_hold_ms <= 0 ? 0 : CLAMP(cfg->guard_bs_hold_ms, 100, 5000);
    nc_shift_release_ms = cfg->shift_release_ms <= 0 ? 0 : CLAMP(cfg->shift_release_ms, 1, 500);
    nc_reset();
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* ZMK Studio 用パラメータ一覧。ラベルは「単独 左親指 右親指 (物理キー)」。
 * nmap の全キー + 親指シフトキー2つを網羅すること
 * (リストにない値は Studio が不明値として扱うため)。 */
#define NC_VAL(_label, _key)                                                                       \
    { .display_name = _label, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = _key }

static const struct behavior_parameter_value_metadata param_values[] = {
    /* 上段 */
    NC_VAL("。 ぁ − (Q)", Q),
    NC_VAL("か え が (W)", W),
    NC_VAL("た り だ (E)", E),
    NC_VAL("こ ゃ ご (R)", R),
    NC_VAL("さ れ ざ (T)", T),
    NC_VAL("ら ぱ よ (Y)", Y),
    NC_VAL("ち ぢ に (U)", U),
    NC_VAL("く ぐ る (I)", I),
    NC_VAL("つ づ ま (O)", O),
    NC_VAL("、 ぴ ぇ (P)", P),
    /* 中段 */
    NC_VAL("う を ゔ (A)", A),
    NC_VAL("し あ じ (S)", S),
    NC_VAL("て な で (D)", D),
    NC_VAL("け ゅ げ (F)", F),
    NC_VAL("せ も ぜ (G)", G),
    NC_VAL("は ば み (H)", H),
    NC_VAL("と ど お (J)", J),
    NC_VAL("き ぎ の (K)", K),
    NC_VAL("い ぽ ょ (L)", L),
    NC_VAL("ん − っ (;)", SEMI),
    /* 下段 */
    NC_VAL("。 ぅ − (Z)", Z),
    NC_VAL("ひ ー び (X)", X),
    NC_VAL("す ろ ず (C)", C),
    NC_VAL("ふ や ぶ (V)", V),
    NC_VAL("へ ぃ べ (B)", B),
    NC_VAL("め ぷ ぬ (N)", N),
    NC_VAL("そ ぞ ゆ (M)", M),
    NC_VAL("ね ぺ む (,)", COMMA),
    NC_VAL("ほ ぼ わ (.)", DOT),
    NC_VAL("／ ？ ぉ (/)", SLASH),
    NC_VAL("、 − − (@)", LBKT),
    /* 親指シフトキー */
    NC_VAL("左親指シフト (Space)", SPACE),
    NC_VAL("右親指シフト (変換)", INT4),
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

/* ---- 実行時設定 (USBシリアル設定コンソール / 永続化) ---- */

void nc_cfg_get(struct nc_settings *out) {
    out->timeout_ms = nc_timeout_ms;
    out->range_pct = nc_range_pct;
    out->mode = nc_mode;
    out->cont = nc_cont;
    out->log = nc_log_on;
    out->lthumb_tap = nc_lthumb_tap;
    out->rthumb_tap = nc_rthumb_tap;
}

int nc_cfg_set(const char *key, int32_t value) {
    if (strcmp(key, "timeout") == 0) {
        nc_timeout_ms = CLAMP(value, 1, 1000);
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/tmo", &nc_timeout_ms, sizeof(nc_timeout_ms));
#endif
        nc_emit("cfg: timeout=%dms (saved)", nc_timeout_ms);
        return 0;
    }
    if (strcmp(key, "range") == 0) {
        nc_range_pct = CLAMP(value, 1, 100);
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/rng", &nc_range_pct, sizeof(nc_range_pct));
#endif
        nc_emit("cfg: range=%d%% (saved)", nc_range_pct);
        return 0;
    }
    if (strcmp(key, "mode") == 0) {
        nc_mode = value != 0 ? 1 : 0;
        nc_judge = 0; /* 方式切替時は判定待ちを破棄 */
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/mod", &nc_mode, sizeof(nc_mode));
#endif
        nc_emit("cfg: mode=%s (saved)", nc_mode ? "pct(yamabuki)" : "ms");
        return 0;
    }
    if (strcmp(key, "cont") == 0) {
        nc_cont = value != 0;
#if IS_ENABLED(CONFIG_SETTINGS)
        uint8_t v = nc_cont ? 1 : 0;
        settings_save_one("nicola/cnt", &v, sizeof(v));
#endif
        nc_emit("cfg: cont=%d (saved)", (int)nc_cont);
        return 0;
    }
    if (strcmp(key, "log") == 0) { /* 動作ログのストリーム (永続: 次回起動後も有効) */
        nc_log_on = value != 0;
#if IS_ENABLED(CONFIG_SETTINGS)
        uint8_t v = nc_log_on ? 1 : 0;
        settings_save_one("nicola/log", &v, sizeof(v));
#endif
        nc_emit("cfg: log=%d (saved)", (int)nc_log_on);
        return 0;
    }
    if (strcmp(key, "lthumb") == 0) {
        nc_lthumb_tap = (uint32_t)value;
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/lta", &nc_lthumb_tap, sizeof(nc_lthumb_tap));
#endif
        nc_emit("cfg: lthumb tap=%u (saved)", nc_lthumb_tap);
        return 0;
    }
    if (strcmp(key, "rthumb") == 0) {
        nc_rthumb_tap = (uint32_t)value;
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/rta", &nc_rthumb_tap, sizeof(nc_rthumb_tap));
#endif
        nc_emit("cfg: rthumb tap=%u (saved)", nc_rthumb_tap);
        return 0;
    }
    return -EINVAL;
}

void nc_cfg_reset(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_delete("nicola/tmo");
    settings_delete("nicola/rng");
    settings_delete("nicola/mod");
    settings_delete("nicola/cnt");
    settings_delete("nicola/lta");
    settings_delete("nicola/rta");
    settings_delete("nicola/log");
#endif
    nc_emit("cfg: saved settings cleared");
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int nicola_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (settings_name_steq(name, "tmo", NULL) && len == sizeof(nc_timeout_ms)) {
        read_cb(cb_arg, &nc_timeout_ms, len);
        nc_timeout_ms = CLAMP(nc_timeout_ms, 1, 1000);
        nc_emit("cfg loaded: timeout=%dms", nc_timeout_ms);
        return 0;
    }
    if (settings_name_steq(name, "rng", NULL) && len == sizeof(nc_range_pct)) {
        read_cb(cb_arg, &nc_range_pct, len);
        nc_range_pct = CLAMP(nc_range_pct, 1, 100);
        nc_emit("cfg loaded: range=%d%%", nc_range_pct);
        return 0;
    }
    if (settings_name_steq(name, "mod", NULL) && len == sizeof(nc_mode)) {
        read_cb(cb_arg, &nc_mode, len);
        nc_mode = nc_mode != 0 ? 1 : 0;
        nc_emit("cfg loaded: mode=%s", nc_mode ? "pct" : "ms");
        return 0;
    }
    if (settings_name_steq(name, "cnt", NULL) && len == 1) {
        uint8_t v;
        read_cb(cb_arg, &v, 1);
        nc_cont = v != 0;
        nc_emit("cfg loaded: cont=%d", (int)nc_cont);
        return 0;
    }
    if (settings_name_steq(name, "lta", NULL) && len == sizeof(nc_lthumb_tap)) {
        read_cb(cb_arg, &nc_lthumb_tap, len);
        nc_emit("cfg loaded: lthumb tap=%u", nc_lthumb_tap);
        return 0;
    }
    if (settings_name_steq(name, "rta", NULL) && len == sizeof(nc_rthumb_tap)) {
        read_cb(cb_arg, &nc_rthumb_tap, len);
        nc_emit("cfg loaded: rthumb tap=%u", nc_rthumb_tap);
        return 0;
    }
    if (settings_name_steq(name, "log", NULL) && len == 1) {
        uint8_t v;
        read_cb(cb_arg, &v, 1);
        nc_log_on = v != 0;
        nc_emit("cfg loaded: log=%d", (int)nc_log_on);
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(nicola, "nicola", NULL, nicola_settings_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

static const struct behavior_driver_api behavior_nicola_driver_api = {
    .binding_pressed = on_nicola_pressed,
    .binding_released = on_nicola_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define NC_INST(n)                                                                                 \
    static const struct behavior_nicola_config behavior_nicola_config_##n = {                      \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
        .range_pct = DT_INST_PROP(n, range_pct),                                                   \
        .cont = DT_INST_PROP(n, continuous_shift),                                                 \
        .lthumb = DT_INST_PROP_OR(n, left_thumb, SPACE),                                           \
        .rthumb = DT_INST_PROP_OR(n, right_thumb, INT4),                                           \
        .guard = DT_INST_PROP(n, pending_guard),                                                   \
        .guard_repeat_ms = DT_INST_PROP(n, guard_repeat_ms),                                       \
        .guard_timeout_ms = DT_INST_PROP(n, guard_timeout_ms),                                     \
        .guard_bs_hold_ms = DT_INST_PROP(n, guard_bs_hold_ms),                                     \
        .shift_release_ms = DT_INST_PROP(n, shift_release_ms),                                     \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_nicola_init, NULL, NULL, &behavior_nicola_config_##n,      \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_nicola_driver_api);

DT_INST_FOREACH_STATUS_OKAY(NC_INST)

/* ---- Conductor拡張: 親指シフトキー専用 behavior (&ncl / &ncr) ----
 * param1 = 単独タップで送出するキーコード、param2 = 単独タップで離脱するレイヤー
 * (NC_NO_LAYER=255 なら離脱しない)。左右は DT の side (0=左, 1=右) で決まる。
 * 判定ロジックは &nc の親指キーと共通 (nc_thumb_pressed / nc_thumb_released)。 */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_nicola_thumb

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_nicola_thumb_config {
    uint8_t side;
};

static bool nct_is_left(struct zmk_behavior_binding *binding) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_nicola_thumb_config *cfg = dev->config;
    return cfg->side == 0;
}

static int on_nicola_thumb_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    const int64_t ts = event.timestamp;

    /* 修飾キー押下中は単独タップ用のキーをそのまま素通し (&nc と同じ扱い) */
    if (nc_mods_active()) {
        nc_flush(ts);
        if (nc_raw_n < NC_BUF_MAX) {
            nc_raw[nc_raw_n++] = binding->param1;
        }
        raise_zmk_keycode_state_changed_from_encoded(binding->param1, true, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    nc_thumb_pressed(nct_is_left(binding), ts);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_nicola_thumb_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const int64_t ts = event.timestamp;
    const uint32_t key = binding->param1;

    for (int i = 0; i < nc_raw_n; i++) {
        if (nc_raw[i] == key) {
            for (int j = i; j < nc_raw_n - 1; j++) {
                nc_raw[j] = nc_raw[j + 1];
            }
            nc_raw_n--;
            raise_zmk_keycode_state_changed_from_encoded(key, false, ts);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }
    nc_thumb_released(nct_is_left(binding), ts, key, binding->param2);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_nicola_thumb_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static const struct behavior_driver_api behavior_nicola_thumb_driver_api = {
    .binding_pressed = on_nicola_thumb_pressed,
    .binding_released = on_nicola_thumb_released,
};

#define NCT_INST(n)                                                                                \
    static const struct behavior_nicola_thumb_config behavior_nicola_thumb_config_##n = {          \
        .side = DT_INST_PROP(n, side),                                                             \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_nicola_thumb_init, NULL, NULL,                            \
                            &behavior_nicola_thumb_config_##n, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_nicola_thumb_driver_api);

DT_INST_FOREACH_STATUS_OKAY(NCT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_nicola_thumb) */
