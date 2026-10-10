/*
 * zmk-nicola: IME settle behavior (&ime_settle <ms>)
 *
 * NICOLA切替でIMEキー(半角/全角等)を送った直後は、ホスト側でIME切替が完了する
 * 前に後続の打鍵が届くと、切替前のIME状態で文字が処理されてしまう。
 * この behavior をマクロ内でIMEキーの直後に置くと、以降 <ms> の間に発生した
 * keycodeイベント(打鍵の出力)をファーム内で保留し、期限後に元の順序のまま
 * 再送出する。これにより「IMEキー → (ms空けて) → 後続文字」がホスト到達順で
 * 保証される。
 *
 * positionイベントではなくkeycodeイベントを捕まえる理由: 切替キーの
 * タップダンスに割り込んだキーのpositionイベントは、保留ウィンドウが開く前に
 * このモジュールのリスナーを通過してしまう。keycodeイベントはkeymap処理の
 * 中で(=ウィンドウが開いた後に)発生するので確実に捕まえられる。
 * 同じ理由で、切替マクロの wait-ms/tap-ms は 0 でなければならない
 * (待ち>0だとウィンドウが開く前に割り込みキーの出力が先行する)。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DT_DRV_COMPAT zmk_behavior_ime_settle

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* 150msの窓で捕まえる可能性があるのは高速打鍵でもたかだか数キー分
 * (押下+解放で2イベント/キー)。余裕を持たせた上限。 */
#define IME_SETTLE_MAX_CAPTURED 24

static bool settle_active;
static struct zmk_keycode_state_changed_event captured[IME_SETTLE_MAX_CAPTURED];
static int captured_count;

const struct zmk_listener zmk_listener_behavior_ime_settle;

/* 保留中のイベントを元の順序で再送出する。settle_active を先に落とすので、
 * 再送出されたイベントが自分自身に再捕獲されることはない。 */
static void flush_captured(void) {
    settle_active = false;
    const int n = captured_count;
    captured_count = 0;
    for (int i = 0; i < n; i++) {
        ZMK_EVENT_RAISE_AT(captured[i], behavior_ime_settle);
    }
}

static void settle_expired(struct k_work *work) { flush_captured(); }
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_expired);

static int on_ime_settle_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    const uint32_t ms = binding->param1;
    if (ms == 0) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    settle_active = true;
    k_work_reschedule(&settle_work, K_MSEC(ms));
    LOG_DBG("ime_settle: hold window %ums", ms);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_ime_settle_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int ime_settle_listener(const zmk_event_t *eh) {
    if (!settle_active) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (captured_count >= IME_SETTLE_MAX_CAPTURED) {
        /* 枠が尽きたら順序維持を優先: 保留分を全部流してから素通しに戻る */
        LOG_WRN("ime_settle: capture buffer full, flushing early");
        k_work_cancel_delayable(&settle_work);
        flush_captured();
        return ZMK_EV_EVENT_BUBBLE;
    }
    captured[captured_count++] = copy_raised_zmk_keycode_state_changed(ev);
    LOG_DBG("ime_settle: captured 0x%02X %s (%d held)", ev->keycode,
            ev->state ? "down" : "up", captured_count);
    return ZMK_EV_EVENT_CAPTURED;
}

ZMK_LISTENER(behavior_ime_settle, ime_settle_listener);
ZMK_SUBSCRIPTION(behavior_ime_settle, zmk_keycode_state_changed);

static int behavior_ime_settle_init(const struct device *dev) { return 0; }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {{
    .display_name = "hold ms",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = 1000},
}};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

static const struct behavior_driver_api behavior_ime_settle_driver_api = {
    .binding_pressed = on_ime_settle_pressed,
    .binding_released = on_ime_settle_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define IME_SETTLE_INST(n)                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_ime_settle_init, NULL, NULL, NULL, POST_KERNEL,           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_ime_settle_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IME_SETTLE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
