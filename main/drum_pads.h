#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAD_KICK = 0,
    PAD_SNARE,
    PAD_HIHAT,
    PAD_CRASH,
    PAD_TOM,
    PAD_COUNT
} drum_pad_id_t;

typedef enum {
    DRUM_EVT_HIT = 0,       // note sent
    DRUM_EVT_XTALK,         // hit rejected as crosstalk
    DRUM_EVT_PEDAL,         // hi-hat pedal changed (velocity field: 1 = closed, 0 = open)
    DRUM_EVT_USB_MOUNTED,   // host connected
    DRUM_EVT_USB_UNMOUNTED, // host disconnected
} drum_evt_type_t;

typedef struct {
    uint8_t  type;      // drum_evt_type_t
    uint8_t  pad;       // drum_pad_id_t
    uint8_t  note;
    uint8_t  velocity;
    uint16_t peak;      // ADC counts above baseline
} drum_event_t;

/** Configure the ADC (continuous / DMA mode) and start the real-time drum task. */
esp_err_t drum_pads_start(void);

/** Queue with drum_event_t items for logging (never blocks the drum task). */
QueueHandle_t drum_pads_event_queue(void);

/** Highest signal per pad since the last call (calibration). Resets the values. */
void drum_pads_take_levels(uint16_t out[PAD_COUNT]);

/** Number of times the ADC DMA pool overflowed (drum task too slow). */
uint32_t drum_pads_overflow_count(void);

const char *drum_pad_name(int pad);
int drum_pad_gpio(int pad);

#ifdef __cplusplus
}
#endif
