/*
 * ESP Drums - ESP32-S3 USB-MIDI electronic drum module
 *
 *   5 piezo pads (Kick, Snare, Hi-Hat, Crash, Tom) on ADC1
 *   1 hi-hat open/close switch (touch sensor / footswitch)
 *   Native USB -> class-compliant USB-MIDI device "ESP Drums"
 *
 * See README.md for wiring, pinout and flashing instructions.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drum_pads.h"
#include "hihat_pedal.h"
#include "usb_midi.h"

static const char *TAG = "main";

/* Low-priority task: prints hits / pedal changes / levels on the UART console,
 * so that slow serial output never delays the real-time drum task. */
static void log_task(void *arg)
{
    QueueHandle_t q = drum_pads_event_queue();
    uint32_t last_ovf = 0;
    TickType_t last_levels = xTaskGetTickCount();

    while (true) {
        drum_event_t e;
        if (xQueueReceive(q, &e, pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (e.type) {
            case DRUM_EVT_HIT:
#if CONFIG_DRUMS_LOG_HITS
                ESP_LOGI("hit", "%-6s note=%3u vel=%3u  (peak=%4u)",
                         drum_pad_name(e.pad), e.note, e.velocity, e.peak);
#endif
                break;
            case DRUM_EVT_XTALK:
#if CONFIG_DRUMS_LOG_HITS
                ESP_LOGW("hit", "%-6s ignored as crosstalk (peak=%4u)", drum_pad_name(e.pad), e.peak);
#endif
                break;
            case DRUM_EVT_PEDAL:
                ESP_LOGI("pedal", "Hi-Hat %s", e.velocity ? "CLOSED" : "OPEN");
                break;
            case DRUM_EVT_USB_MOUNTED:
                ESP_LOGI("usb", "Connected to computer - MIDI ready");
                break;
            case DRUM_EVT_USB_UNMOUNTED:
                ESP_LOGW("usb", "Disconnected from computer");
                break;
            default:
                break;
            }
        }

        uint32_t ovf = drum_pads_overflow_count();
        if (ovf != last_ovf) {
            ESP_LOGW(TAG, "ADC buffer overflow x%lu", (unsigned long)(ovf - last_ovf));
            last_ovf = ovf;
        }

#if CONFIG_DRUMS_LOG_LEVELS
        if (xTaskGetTickCount() - last_levels >= pdMS_TO_TICKS(250)) {
            last_levels = xTaskGetTickCount();
            uint16_t lv[PAD_COUNT];
            drum_pads_take_levels(lv);
            printf("levels  Kick:%4u  Snare:%4u  HiHat:%4u  Crash:%4u  Tom:%4u   pedal:%s\n",
                   lv[PAD_KICK], lv[PAD_SNARE], lv[PAD_HIHAT], lv[PAD_CRASH], lv[PAD_TOM],
                   hihat_pedal_is_closed() ? "CLOSED" : "open");
        }
#else
        (void)last_levels;
#endif
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP Drums (ESP32-S3 USB-MIDI) ===");
    for (int i = 0; i < PAD_COUNT; i++) {
        ESP_LOGI(TAG, "  %-6s -> GPIO%d", drum_pad_name(i), drum_pad_gpio(i));
    }
    ESP_LOGI(TAG, "  Pedal  -> GPIO%d (%s)", CONFIG_DRUMS_PEDAL_GPIO,
#if CONFIG_DRUMS_PEDAL_MODE_TOUCH
             "built-in capacitive touch"
#else
             "digital input"
#endif
            );
    ESP_LOGI(TAG, "  MIDI channel %d", CONFIG_DRUMS_MIDI_CHANNEL);

    ESP_ERROR_CHECK(usb_midi_init());
    ESP_ERROR_CHECK(hihat_pedal_init());
    ESP_ERROR_CHECK(drum_pads_start());

    xTaskCreatePinnedToCore(log_task, "log", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "Ready - hit the pads!");
}
