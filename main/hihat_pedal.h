#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the hi-hat pedal input (digital GPIO or built-in capacitive touch). */
esp_err_t hihat_pedal_init(void);

/**
 * Poll the (debounced) pedal state. Call regularly (every ~1 ms).
 * @param now_us   current time from esp_timer_get_time()
 * @param changed  set to true when the debounced state changed in this call
 * @return true = hi-hat CLOSED (foot down), false = OPEN
 */
bool hihat_pedal_poll(int64_t now_us, bool *changed);

/** Last debounced state without polling. */
bool hihat_pedal_is_closed(void);

#ifdef __cplusplus
}
#endif
