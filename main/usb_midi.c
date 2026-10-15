/*
 * USB-MIDI device using TinyUSB (espressif/esp_tinyusb).
 * The ESP32-S3 native USB port (GPIO19 = D-, GPIO20 = D+) shows up on the
 * computer as a class-compliant MIDI device called "ESP Drums" - no driver needed.
 */
#include "usb_midi.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_midi";

#define MIDI_CABLE      0
#define MIDI_CH         ((uint8_t)(CONFIG_DRUMS_MIDI_CHANNEL - 1))

enum {
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_COUNT
};

enum {
    EP_EMPTY = 0,
    EPNUM_MIDI,
};

#define TUSB_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_MIDI * TUD_MIDI_DESC_LEN)

static const char *s_str_desc[] = {
    (char[]){0x09, 0x04},   // 0: language = English (0x0409)
    "DIY",                  // 1: Manufacturer
    "ESP Drums",            // 2: Product
    "000001",               // 3: Serial
    "ESP Drums MIDI",       // 4: MIDI interface name
};

static const uint8_t s_midi_cfg_desc[] = {
    // config number, interface count, string index, total length, attribute, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),
    // interface number, string index, EP OUT & EP IN address, EP size
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 64),
};

esp_err_t usb_midi_init(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.string = s_str_desc;
    cfg.descriptor.string_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    cfg.descriptor.full_speed_config = s_midi_cfg_desc;

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB-MIDI device started (channel %d)", CONFIG_DRUMS_MIDI_CHANNEL);
    }
    return err;
}

bool usb_midi_ready(void)
{
    return tud_midi_mounted();
}

static inline void send3(uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!tud_midi_mounted()) {
        return;
    }
    const uint8_t msg[3] = { status, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F) };
    tud_midi_stream_write(MIDI_CABLE, msg, sizeof(msg));
}

void usb_midi_note_on(uint8_t note, uint8_t velocity)
{
    if (velocity == 0) {
        velocity = 1;   // velocity 0 would mean Note-Off
    }
    send3(0x90 | MIDI_CH, note, velocity);
}

void usb_midi_note_off(uint8_t note)
{
    send3(0x80 | MIDI_CH, note, 0);
}

void usb_midi_cc(uint8_t controller, uint8_t value)
{
    send3(0xB0 | MIDI_CH, controller, value);
}

void usb_midi_drain_rx(void)
{
    uint8_t packet[4];
    while (tud_midi_available()) {
        if (!tud_midi_packet_read(packet)) {
            break;
        }
    }
}
