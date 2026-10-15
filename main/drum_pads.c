/*
 * Real-time drum trigger engine.
 *
 * All 5 piezo inputs are sampled by ADC1 in continuous (DMA) mode at
 * 8 kHz per pad. For every pad a small state machine runs:
 *
 *   IDLE  --(signal >= threshold)-->  SCAN  (look for the peak for SCAN_TIME)
 *   SCAN  --(scan time over)------->  send Note-On, velocity = f(peak)  -> MASK
 *   MASK  --(mask time over)------->  IDLE (with a decaying dynamic threshold)
 *
 * The peak voltage of the piezo during the scan window = how hard the pad was
 * hit = MIDI velocity (1..127).
 */
#include "drum_pads.h"

#include <math.h>
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "hihat_pedal.h"
#include "usb_midi.h"

static const char *TAG = "drums";

/* ---------------- sampling parameters ---------------- */
#define PER_PAD_HZ              8000                        // samples / second / pad
#define TOTAL_HZ                (PER_PAD_HZ * PAD_COUNT)     // 40 kHz total (S3 max ~83 kHz)
#define MS_TO_SAMPLES(ms)       ((uint32_t)(ms) * PER_PAD_HZ / 1000)
#define FRAME_SAMPLES_PER_PAD   8                           // 1 ms worth of data per DMA frame
#define FRAME_BYTES             (PAD_COUNT * FRAME_SAMPLES_PER_PAD * SOC_ADC_DIGI_RESULT_BYTES)
#define READ_BYTES              (FRAME_BYTES * 4)           // read up to 4 frames at once if behind
#define READ_SAMPLES            (READ_BYTES / SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_MAX_VALUE           4095

#define SCAN_SAMPLES            MS_TO_SAMPLES(CONFIG_DRUMS_SCAN_TIME_MS)
#define MASK_SAMPLES            MS_TO_SAMPLES(CONFIG_DRUMS_MASK_TIME_MS)
#define DECAY_SAMPLES           MS_TO_SAMPLES(CONFIG_DRUMS_DYN_DECAY_MS)
#define XTALK_SAMPLES           MS_TO_SAMPLES(CONFIG_DRUMS_CROSSTALK_WINDOW_MS)
#define NOTE_OFF_US             ((int64_t)CONFIG_DRUMS_NOTE_OFF_MS * 1000)

typedef enum { ST_IDLE = 0, ST_SCAN, ST_MASK } pad_state_t;

typedef struct {
    /* configuration */
    const char   *name;
    int           gpio;
    uint8_t       note;
    uint16_t      threshold;
    uint16_t      max_peak;
    adc_channel_t channel;

    /* runtime */
    pad_state_t   state;
    uint32_t      tick;          // samples processed on this pad (time base)
    uint32_t      counter;       // samples remaining in SCAN / MASK
    int32_t       baseline_q8;   // idle level, fixed point 24.8
    bool          baseline_init;
    uint16_t      peak;
    uint16_t      dyn_start;     // dynamic threshold start value
    uint32_t      decay_pos;     // samples since end of mask
    /* crosstalk bookkeeping (last accepted hit) */
    bool          has_last_hit;
    uint32_t      last_hit_tick;
    float         last_hit_norm;
    /* pending note-off */
    int16_t       off_note;      // -1 = none
    int64_t       off_time_us;
} pad_t;

static pad_t s_pads[PAD_COUNT] = {
    [PAD_KICK]  = { "Kick",   CONFIG_DRUMS_KICK_GPIO,   CONFIG_DRUMS_KICK_NOTE,
                    CONFIG_DRUMS_KICK_THRESHOLD,   CONFIG_DRUMS_KICK_MAX_PEAK },
    [PAD_SNARE] = { "Snare",  CONFIG_DRUMS_SNARE_GPIO,  CONFIG_DRUMS_SNARE_NOTE,
                    CONFIG_DRUMS_SNARE_THRESHOLD,  CONFIG_DRUMS_SNARE_MAX_PEAK },
    [PAD_HIHAT] = { "Hi-Hat", CONFIG_DRUMS_HIHAT_GPIO,  CONFIG_DRUMS_HIHAT_NOTE_CLOSED,
                    CONFIG_DRUMS_HIHAT_THRESHOLD,  CONFIG_DRUMS_HIHAT_MAX_PEAK },
    [PAD_CRASH] = { "Crash",  CONFIG_DRUMS_CRASH_GPIO,  CONFIG_DRUMS_CRASH_NOTE,
                    CONFIG_DRUMS_CRASH_THRESHOLD,  CONFIG_DRUMS_CRASH_MAX_PEAK },
    [PAD_TOM]   = { "Tom",    CONFIG_DRUMS_TOM_GPIO,    CONFIG_DRUMS_TOM_NOTE,
                    CONFIG_DRUMS_TOM_THRESHOLD,    CONFIG_DRUMS_TOM_MAX_PEAK },
};

static adc_continuous_handle_t s_adc;
static QueueHandle_t s_evt_queue;
static int8_t s_chan_to_pad[SOC_ADC_CHANNEL_NUM(0)];
static volatile uint16_t s_levels[PAD_COUNT];
static volatile uint32_t s_overflows;

/* ======================================================================= */

const char *drum_pad_name(int pad)
{
    return (pad >= 0 && pad < PAD_COUNT) ? s_pads[pad].name : "?";
}

int drum_pad_gpio(int pad)
{
    return (pad >= 0 && pad < PAD_COUNT) ? s_pads[pad].gpio : -1;
}

QueueHandle_t drum_pads_event_queue(void)
{
    return s_evt_queue;
}

void drum_pads_take_levels(uint16_t out[PAD_COUNT])
{
    for (int i = 0; i < PAD_COUNT; i++) {
        out[i] = s_levels[i];
        s_levels[i] = 0;
    }
}

uint32_t drum_pads_overflow_count(void)
{
    return s_overflows;
}

static inline void post_event(drum_evt_type_t type, int pad, uint8_t note, uint8_t vel, uint16_t peak)
{
    drum_event_t e = { .type = type, .pad = (uint8_t)pad, .note = note, .velocity = vel, .peak = peak };
    (void)xQueueSend(s_evt_queue, &e, 0);   // drop if the logger can't keep up
}

/* ======================================================================= */

static uint8_t peak_to_velocity(const pad_t *p, uint16_t peak)
{
    float n;
    if (p->max_peak <= p->threshold) {
        n = 1.0f;
    } else {
        n = (float)((int)peak - (int)p->threshold) / (float)(p->max_peak - p->threshold);
    }
    if (n < 0.0f) {
        n = 0.0f;
    } else if (n > 1.0f) {
        n = 1.0f;
    }
#if CONFIG_DRUMS_VELOCITY_CURVE != 100
    n = powf(n, (float)CONFIG_DRUMS_VELOCITY_CURVE / 100.0f);
#endif
    int v = 1 + (int)lroundf(n * 126.0f);
    return (uint8_t)(v > 127 ? 127 : v);
}

/* Returns true if this hit is most likely vibration from a louder hit on another pad. */
static bool is_crosstalk(int pad_idx, float norm)
{
#if CONFIG_DRUMS_CROSSTALK_PERCENT > 0
    const pad_t *p = &s_pads[pad_idx];
    const float ratio = (float)CONFIG_DRUMS_CROSSTALK_PERCENT / 100.0f;
    for (int i = 0; i < PAD_COUNT; i++) {
        if (i == pad_idx || !s_pads[i].has_last_hit) {
            continue;
        }
        uint32_t age = p->tick - s_pads[i].last_hit_tick;   // wrap-safe
        if (age <= XTALK_SAMPLES && norm < s_pads[i].last_hit_norm * ratio) {
            return true;
        }
    }
#endif
    return false;
}

static void emit_hit(int pad_idx)
{
    pad_t *p = &s_pads[pad_idx];
    const uint16_t peak = p->peak;
    const float norm = (float)peak / (float)p->max_peak;

    if (is_crosstalk(pad_idx, norm)) {
        post_event(DRUM_EVT_XTALK, pad_idx, 0, 0, peak);
        return;
    }
    p->has_last_hit = true;
    p->last_hit_tick = p->tick;
    p->last_hit_norm = norm;

    uint8_t note = p->note;
    if (pad_idx == PAD_HIHAT) {
        note = hihat_pedal_is_closed() ? CONFIG_DRUMS_HIHAT_NOTE_CLOSED : CONFIG_DRUMS_HIHAT_NOTE_OPEN;
    }
    const uint8_t vel = peak_to_velocity(p, peak);

    if (p->off_note >= 0) {             // previous note still "on" -> close it first
        usb_midi_note_off((uint8_t)p->off_note);
    }
    usb_midi_note_on(note, vel);
    p->off_note = note;
    p->off_time_us = esp_timer_get_time() + NOTE_OFF_US;

    post_event(DRUM_EVT_HIT, pad_idx, note, vel, peak);
}

/* Run the trigger state machine for one ADC sample of one pad. */
static inline void process_sample(int pad_idx, uint32_t raw)
{
    pad_t *p = &s_pads[pad_idx];
    p->tick++;

    if (!p->baseline_init) {
        p->baseline_q8 = (int32_t)raw << 8;
        p->baseline_init = true;
    }
    int sig = (int)raw - (p->baseline_q8 >> 8);
    if (sig < 0) {
        sig = 0;
    }
    if (sig > s_levels[pad_idx]) {
        s_levels[pad_idx] = (uint16_t)sig;
    }

    switch (p->state) {
    case ST_IDLE: {
        int thr = p->threshold;
#if CONFIG_DRUMS_DYN_THRESHOLD_PERCENT > 0
        if (p->decay_pos < DECAY_SAMPLES) {
            int dyn = (int)((uint32_t)p->dyn_start * (DECAY_SAMPLES - p->decay_pos) / DECAY_SAMPLES);
            if (dyn > thr) {
                thr = dyn;
            }
            p->decay_pos++;
        }
#endif
        if (sig >= thr) {
            p->state = ST_SCAN;
            p->peak = (uint16_t)sig;
            p->counter = SCAN_SAMPLES;
        } else if (sig < p->threshold / 2) {
            /* slowly follow the idle level (time constant ~128 ms) */
            p->baseline_q8 += (((int32_t)raw << 8) - p->baseline_q8) >> 10;
        }
        break;
    }
    case ST_SCAN:
        if (sig > p->peak) {
            p->peak = (uint16_t)sig;
        }
        if (--p->counter == 0) {
            emit_hit(pad_idx);
            p->state = ST_MASK;
            p->counter = MASK_SAMPLES;
            p->dyn_start = (uint16_t)((uint32_t)p->peak * CONFIG_DRUMS_DYN_THRESHOLD_PERCENT / 100);
        }
        break;

    case ST_MASK:
        if (--p->counter == 0) {
            p->state = ST_IDLE;
            p->decay_pos = 0;
        }
        break;
    }
}

static void handle_pedal(int64_t now_us)
{
    bool changed;
    bool closed = hihat_pedal_poll(now_us, &changed);
    if (!changed) {
        return;
    }
#if CONFIG_DRUMS_PEDAL_SEND_CC4
    usb_midi_cc(4, closed ? 127 : 0);
#endif
#if CONFIG_DRUMS_PEDAL_CHICK_ENABLE
    if (closed) {
        usb_midi_note_on(CONFIG_DRUMS_PEDAL_CHICK_NOTE, CONFIG_DRUMS_PEDAL_CHICK_VELOCITY);
        usb_midi_note_off(CONFIG_DRUMS_PEDAL_CHICK_NOTE);
    }
#endif
    post_event(DRUM_EVT_PEDAL, PAD_HIHAT, 0, closed ? 1 : 0, 0);
}

static void handle_note_offs(int64_t now_us)
{
    for (int i = 0; i < PAD_COUNT; i++) {
        pad_t *p = &s_pads[i];
        if (p->off_note >= 0 && now_us >= p->off_time_us) {
            usb_midi_note_off((uint8_t)p->off_note);
            p->off_note = -1;
        }
    }
}

static void drum_task(void *arg)
{
    static uint8_t buf[READ_BYTES];
    static adc_continuous_data_t parsed[READ_SAMPLES];
    bool was_mounted = false;

    ESP_ERROR_CHECK(adc_continuous_start(s_adc));

    while (true) {
        uint32_t len = 0;
        esp_err_t err = adc_continuous_read(s_adc, buf, sizeof(buf), &len, 20);
        const int64_t now = esp_timer_get_time();

        /* USB connection housekeeping */
        const bool mounted = usb_midi_ready();
        if (mounted != was_mounted) {
            was_mounted = mounted;
            post_event(mounted ? DRUM_EVT_USB_MOUNTED : DRUM_EVT_USB_UNMOUNTED, 0, 0, 0, 0);
#if CONFIG_DRUMS_PEDAL_SEND_CC4
            if (mounted) {   // tell the host the current hi-hat position
                usb_midi_cc(4, hihat_pedal_is_closed() ? 127 : 0);
            }
#endif
        }
        if (mounted) {
            usb_midi_drain_rx();
        }

        handle_pedal(now);

        if (err == ESP_OK && len > 0) {
            uint32_t n = 0;
            if (adc_continuous_parse_data(s_adc, buf, len, parsed, &n) == ESP_OK) {
                for (uint32_t i = 0; i < n; i++) {
                    if (!parsed[i].valid || parsed[i].unit != ADC_UNIT_1 ||
                            parsed[i].channel >= SOC_ADC_CHANNEL_NUM(0)) {
                        continue;
                    }
                    const int pad = s_chan_to_pad[parsed[i].channel];
                    if (pad >= 0) {
                        process_sample(pad, parsed[i].raw_data);
                    }
                }
            }
        }

        handle_note_offs(now);
    }
}

static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t h, const adc_continuous_evt_data_t *e, void *ctx)
{
    s_overflows++;
    return false;
}

/* ======================================================================= */

esp_err_t drum_pads_start(void)
{
    s_evt_queue = xQueueCreate(32, sizeof(drum_event_t));
    ESP_RETURN_ON_FALSE(s_evt_queue, ESP_ERR_NO_MEM, TAG, "queue");

    memset(s_chan_to_pad, -1, sizeof(s_chan_to_pad));

    adc_digi_pattern_config_t pattern[PAD_COUNT] = {0};
    for (int i = 0; i < PAD_COUNT; i++) {
        pad_t *p = &s_pads[i];
        adc_unit_t unit;
        adc_channel_t ch;
        if (adc_continuous_io_to_channel(p->gpio, &unit, &ch) != ESP_OK || unit != ADC_UNIT_1) {
            ESP_LOGE(TAG, "%s: GPIO%d is not an ADC1 pin (use GPIO1..GPIO10)", p->name, p->gpio);
            return ESP_ERR_INVALID_ARG;
        }
        if (s_chan_to_pad[ch] >= 0) {
            ESP_LOGE(TAG, "%s: GPIO%d already used by %s", p->name, p->gpio, s_pads[s_chan_to_pad[ch]].name);
            return ESP_ERR_INVALID_ARG;
        }
        if (p->gpio == CONFIG_DRUMS_PEDAL_GPIO) {
            ESP_LOGE(TAG, "%s: GPIO%d is also the hi-hat pedal pin", p->name, p->gpio);
            return ESP_ERR_INVALID_ARG;
        }
        p->channel = ch;
        p->off_note = -1;
        s_chan_to_pad[ch] = (int8_t)i;

        pattern[i].unit = ADC_UNIT_1;
        pattern[i].channel = ch;
        pattern[i].atten = ADC_ATTEN_DB_12;           // full 0 .. ~3.1 V range
        pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    }

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = FRAME_BYTES * 32,       // ~32 ms of buffering
        .conv_frame_size = FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&hcfg, &s_adc), TAG, "adc handle");

    adc_continuous_config_t ccfg = {
        .pattern_num = PAD_COUNT,
        .adc_pattern = pattern,
        .sample_freq_hz = TOTAL_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc, &ccfg), TAG, "adc config");

    adc_continuous_evt_cbs_t cbs = { .on_pool_ovf = on_pool_ovf };
    ESP_RETURN_ON_ERROR(adc_continuous_register_event_callbacks(s_adc, &cbs, NULL), TAG, "adc cbs");

    ESP_LOGI(TAG, "ADC: %d pads @ %d Hz each, scan %d ms, mask %d ms",
             PAD_COUNT, PER_PAD_HZ, CONFIG_DRUMS_SCAN_TIME_MS, CONFIG_DRUMS_MASK_TIME_MS);

    /* Highest priority, alone on core 1 -> deterministic, low latency */
    BaseType_t ok = xTaskCreatePinnedToCore(drum_task, "drums", 4096, NULL,
                                            configMAX_PRIORITIES - 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
