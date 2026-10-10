/*
 * zmk-nicola: USBシリアル設定コンソール
 *
 * キーボード側dtsで chosen "zmk,nicola-cfg-uart" にCDC-ACMノードを指定すると
 * 有効になる。行単位のテキストプロトコル:
 *   get                 -> ok timeout=50 range=-1 mode=0 cont=0 log=0 lthumb=SPACE rthumb=INT4 boot=0x00
 *   set timeout 60      -> ok ...   (即時反映+フラッシュ保存)
 *   set range 65        -> ok ...
 *   set cont 1          -> ok ...
 *   set log 1           -> ok ...   (動作ログのストリーム。永続: 次回起動後も有効)
 *   set lthumb SPACE    -> ok ...   (単独タップ時の送出キー。判定対象キーの物理位置は変わらない)
 *   set rthumb INT4     -> ok ...   (使えるキー名は nc_keynames を参照)
 *   reset               -> ok reset        (保存値を消去、再起動で既定値)
 *
 * boot= は前回起動のリセット理由 (hwinfo、CONFIG_HWINFO有効時)。pin/watchdog/
 * lockup/brownout/software/power-on 等。クラッシュ頻発の切り分け用。
 * ログ行の形式は "NC <起動からのms> <内容>"。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <dt-bindings/zmk/keys.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_CHOSEN(zmk_nicola_cfg_uart) && IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

#include <zmk_nicola/config.h>

static const struct device *cfg_uart = DEVICE_DT_GET(DT_CHOSEN(zmk_nicola_cfg_uart));

/* Web設定ツール(docs/index.html)の親指キー選択肢と対応させる名前<->キーコード表 */
struct nc_keyname {
    const char *name;
    int32_t code;
};

static const struct nc_keyname nc_keynames[] = {
    {"SPACE", SPACE}, {"INT4", INT4}, {"INT5", INT5}, {"INT2", INT2},
    {"LALT", LALT},   {"RALT", RALT}, {"LGUI", LGUI},
};

static int32_t nc_keycode_from_name(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(nc_keynames); i++) {
        if (strcmp(name, nc_keynames[i].name) == 0) {
            return nc_keynames[i].code;
        }
    }
    return -1;
}

static const char *nc_keyname_from_code(uint32_t code) {
    for (size_t i = 0; i < ARRAY_SIZE(nc_keynames); i++) {
        if ((uint32_t)nc_keynames[i].code == code) {
            return nc_keynames[i].name;
        }
    }
    return "?";
}

#define LINE_MAX 64
static char rx_line[LINE_MAX];
static size_t rx_len;
static char pending_line[LINE_MAX];

/* 前回起動のリセット理由 (hwinfo)。get の boot= フィールドで照会できる */
static uint32_t nc_boot_cause;

static const char *nc_cause_str(uint32_t c) {
#if IS_ENABLED(CONFIG_HWINFO)
    if (c & RESET_WATCHDOG) {
        return "watchdog";
    }
    if (c & RESET_CPU_LOCKUP) {
        return "lockup";
    }
    if (c & RESET_BROWNOUT) {
        return "brownout";
    }
    if (c & RESET_SOFTWARE) {
        return "software";
    }
    if (c & RESET_PIN) {
        return "pin";
    }
    if (c & RESET_POR) {
        return "power-on";
    }
    if (c & RESET_LOW_POWER_WAKE) {
        return "wake";
    }
#else
    ARG_UNUSED(c);
#endif
    return "n/a";
}

static void respond(const char *s) {
    for (; *s != '\0'; s++) {
        uart_poll_out(cfg_uart, *s);
    }
}

static void nc_format_status(char *buf, size_t buf_size, const struct nc_settings *s) {
    snprintf(buf, buf_size,
             "ok timeout=%d range=%d mode=%d cont=%d log=%d lthumb=%s rthumb=%s boot=0x%02x\n",
             s->timeout_ms, s->range_pct, s->mode, (int)s->cont, (int)s->log,
             nc_keyname_from_code(s->lthumb_tap), nc_keyname_from_code(s->rthumb_tap),
             nc_boot_cause);
}

static void handle_line(struct k_work *work) {
    ARG_UNUSED(work);
    char buf[128];
    char *saveptr = NULL;
    char *cmd = strtok_r(pending_line, " \t", &saveptr);
    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "get") == 0) {
        struct nc_settings s;
        nc_cfg_get(&s);
        nc_format_status(buf, sizeof(buf), &s);
        respond(buf);
    } else if (strcmp(cmd, "set") == 0) {
        char *key = strtok_r(NULL, " \t", &saveptr);
        char *val = strtok_r(NULL, " \t", &saveptr);
        if (key == NULL || val == NULL) {
            respond("err usage: set <timeout|range|mode|cont|log|lthumb|rthumb> <value>\n");
            return;
        }
        int32_t ival;
        if (strcmp(key, "lthumb") == 0 || strcmp(key, "rthumb") == 0) {
            ival = nc_keycode_from_name(val);
            if (ival < 0) {
                respond("err unknown key name (SPACE|INT4|INT5|INT2|LALT|RALT|LGUI)\n");
                return;
            }
        } else {
            ival = atoi(val);
        }
        if (nc_cfg_set(key, ival) == 0) {
            struct nc_settings s;
            nc_cfg_get(&s);
            nc_format_status(buf, sizeof(buf), &s);
            respond(buf);
        } else {
            respond("err unsupported key on this firmware\n");
        }
    } else if (strcmp(cmd, "reset") == 0) {
        nc_cfg_reset();
        respond("ok reset (reboot to load keymap defaults)\n");
    } else {
        respond("err commands: get / set <key> <val> / reset\n");
    }
}

static K_WORK_DEFINE(line_work, handle_line);

static void uart_cb(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t c;
        while (uart_fifo_read(dev, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (rx_len > 0) {
                    rx_line[rx_len] = '\0';
                    memcpy(pending_line, rx_line, rx_len + 1);
                    rx_len = 0;
                    k_work_submit(&line_work);
                }
            } else if (rx_len < LINE_MAX - 1) {
                rx_line[rx_len++] = (char)c;
            }
        }
    }
}

/* 動作ログ (set log 1) の出力先としてこのCDCを登録する */
static void console_log_sink(const char *line) { respond(line); }

static int nicola_console_init(void) {
#if IS_ENABLED(CONFIG_HWINFO)
    /* リセット理由を取得してクリア (nRF52のRESETREASはソフトリセットを跨いで残る) */
    if (hwinfo_get_reset_cause(&nc_boot_cause) != 0) {
        nc_boot_cause = 0;
    }
    hwinfo_clear_reset_cause();
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
    /* 保存済み設定を反映 (keymap既定値の後に上書き) */
    settings_load_subtree("nicola");
#endif
    if (!device_is_ready(cfg_uart)) {
        LOG_WRN("NICOLA cfg uart not ready");
        return 0;
    }
    uart_irq_callback_set(cfg_uart, uart_cb);
    uart_irq_rx_enable(cfg_uart);
    nc_cfg_set_log_sink(console_log_sink);
    LOG_INF("NICOLA config console ready (boot reset_cause=0x%02x)", nc_boot_cause);
    /* ホスト接続中の再起動ならこの行がストリームに残る (非接続時は捨てられる) */
    char bootline[80];
    snprintf(bootline, sizeof(bootline), "NC 0 boot reset_cause=0x%02x (%s)\n", nc_boot_cause,
             nc_cause_str(nc_boot_cause));
    respond(bootline);
    return 0;
}

SYS_INIT(nicola_console_init, APPLICATION, 99);

#endif /* DT_HAS_CHOSEN(zmk_nicola_cfg_uart) && CONFIG_UART_INTERRUPT_DRIVEN */
