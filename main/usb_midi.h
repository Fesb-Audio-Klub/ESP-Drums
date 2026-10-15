#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install TinyUSB and expose the device as a class-compliant USB-MIDI device. */
esp_err_t usb_midi_init(void);

/** true when the host has enumerated/configured the MIDI interface. */
bool usb_midi_ready(void);

/* All send functions are no-ops when USB is not mounted.
 * They must be called from a single task (the drum task). */
void usb_midi_note_on(uint8_t note, uint8_t velocity);
void usb_midi_note_off(uint8_t note);
void usb_midi_cc(uint8_t controller, uint8_t value);

/** Discard any MIDI coming from the host (must be drained so the host never blocks). */
void usb_midi_drain_rx(void);

#ifdef __cplusplus
}
#endif
