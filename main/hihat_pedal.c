/*
 * Hi-hat pedal (open / closed switch).
 *
 *  - Digital mode (default): TTP223 touch module OUT pin, or a footswitch.
 *  - Touch mode: ESP32-S3 built-in capacitive touch on a bare wire / foil.
 *
 * The raw signal is debounced in software, then mapped to CLOSED / OPEN.
 */
#include "hihat_pedal.h"

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_DRUMS_PEDAL_MODE_TOUCH
#include "driver/touch_sens.h"
#else
#include "driver/gpio.h"
#endif

static const char *TAG = "pedal";

static bool s_raw_last;          // last raw "active" reading
static int64_t s_raw_change_us;  // when the raw reading last changed
static bool s_closed;            // debounced output

static inline bool active_to_closed(bool active)
{
#if CONFIG_DRUMS_PEDAL_CLOSED_WHEN_ACTIVE
    return active;
#else
    return !active;
#endif
}

/* ======================================================================= */
#if CONFIG_DRUMS_PEDAL_MODE_TOUCH

static volatile bool s_touch_active;
static touch_sensor_handle_t s_touch;
static touch_channel_handle_t s_touch_chan;

static bool on_touch_active(touch_sensor_handle_t h, const touch_active_event_data_t *e, void *ctx)
{
    s_touch_active = true;
    return false;
}

static bool on_touch_inactive(touch_sensor_handle_t h, const touch_inactive_event_data_t *e, void *ctx)
{
    s_touch_active = false;
    return false;
}

static esp_err_t pedal_hw_init(void)
{
    const int chan_id = CONFIG_DRUMS_PEDAL_GPIO;   // on ESP32-S3: touch channel N == GPIO N
    if (chan_id < TOUCH_MIN_CHAN_ID || chan_id > TOUCH_MAX_CHAN_ID) {
        ESP_LOGE(TAG, "GPIO%d is not a touch pin (use GPIO%d..GPIO%d)",
                 chan_id, TOUCH_MIN_CHAN_ID, TOUCH_MAX_CHAN_ID);
        return ESP_ERR_INVALID_ARG;
    }

    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2)
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&sens_cfg, &s_touch), TAG, "touch controller");

    touch_channel_config_t chan_cfg = {
        .active_thresh = {2000},   // temporary, recalculated from the baseline below
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(touch_sensor_new_channel(s_touch, chan_id, &chan_cfg, &s_touch_chan), TAG, "touch channel");

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_touch, &filter_cfg), TAG, "touch filter");

    /* Initial scans to learn the untouched baseline ("benchmark").
     * Do NOT touch the sensor while the board boots! */
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "touch enable");
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(s_touch, 2000), TAG, "touch scan");
    }
    ESP_RETURN_ON_ERROR(touch_sensor_disable(s_touch), TAG, "touch disable");

    uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {0};
    ESP_RETURN_ON_ERROR(touch_channel_read_data(s_touch_chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, benchmark), TAG, "bm");
    uint32_t thr = (uint32_t)((uint64_t)benchmark[0] * CONFIG_DRUMS_TOUCH_THRESHOLD_PERMILLE / 1000);
    if (thr == 0) {
        thr = 1;
    }
    chan_cfg.active_thresh[0] = thr;
    ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(s_touch_chan, &chan_cfg), TAG, "touch reconfig");
    ESP_LOGI(TAG, "Touch pedal on GPIO%d: baseline=%lu threshold=%lu",
             chan_id, (unsigned long)benchmark[0], (unsigned long)thr);

    touch_event_callbacks_t cbs = {
        .on_active = on_touch_active,
        .on_inactive = on_touch_inactive,
    };
    ESP_RETURN_ON_ERROR(touch_sensor_register_callbacks(s_touch, &cbs, NULL), TAG, "touch cbs");
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "touch enable");
    ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s_touch), TAG, "touch start");
    return ESP_OK;
}

static inline bool pedal_read_active(void)
{
    return s_touch_active;
}

/* ======================================================================= */
#else  // digital GPIO mode

static esp_err_t pedal_hw_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_DRUMS_PEDAL_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_DRUMS_PEDAL_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    ESP_LOGI(TAG, "Digital pedal on GPIO%d (active %s)", CONFIG_DRUMS_PEDAL_GPIO,
#if CONFIG_DRUMS_PEDAL_ACTIVE_LOW
             "LOW, pull-up"
#else
             "HIGH, pull-down"
#endif
            );
    return ESP_OK;
}

static inline bool pedal_read_active(void)
{
    int level = gpio_get_level(CONFIG_DRUMS_PEDAL_GPIO);
#if CONFIG_DRUMS_PEDAL_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

#endif
/* ======================================================================= */

esp_err_t hihat_pedal_init(void)
{
    esp_err_t err = pedal_hw_init();
    if (err != ESP_OK) {
        return err;
    }
    s_raw_last = pedal_read_active();
    s_raw_change_us = 0;
    s_closed = active_to_closed(s_raw_last);
    ESP_LOGI(TAG, "Initial hi-hat state: %s", s_closed ? "CLOSED" : "OPEN");
    return ESP_OK;
}

bool hihat_pedal_poll(int64_t now_us, bool *changed)
{
    *changed = false;
    bool raw = pedal_read_active();
    if (raw != s_raw_last) {
        s_raw_last = raw;
        s_raw_change_us = now_us;
    }
    if ((now_us - s_raw_change_us) >= (int64_t)CONFIG_DRUMS_PEDAL_DEBOUNCE_MS * 1000) {
        bool closed = active_to_closed(raw);
        if (closed != s_closed) {
            s_closed = closed;
            *changed = true;
        }
    }
    return s_closed;
}

bool hihat_pedal_is_closed(void)
{
    return s_closed;
}
