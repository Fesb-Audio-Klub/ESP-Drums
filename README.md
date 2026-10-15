# ESP Drums – ESP32-S3 USB-MIDI electronic drum module

Firmware in **native ESP-IDF (v6.0.3)**. It turns an ESP32-S3 into a class-compliant
**USB-MIDI drum module**. Plug it into a computer and it shows up as **"ESP Drums"**
(no drivers needed on macOS, Windows or Linux). It works with GarageBand, Logic, Ableton,
Reaper, EZdrummer, Superior Drummer, Addictive Drums and similar.

- 5 piezo pads: **Kick, Snare, Hi-Hat, Crash, Tom**
- **Velocity from how hard you hit.** The peak piezo voltage maps to MIDI velocity 1–127.
- A **hi-hat open/close switch** (touch sensor now, a footswitch later)
  - pedal closed → hi-hat pad plays **Closed Hi-Hat (42)**
  - pedal open → hi-hat pad plays **Open Hi-Hat (46)**
  - closing the pedal plays **Pedal Hi-Hat "chick" (44)** and sends **CC#4**
- 8 kHz sampling per pad (ADC DMA), about 3 ms latency, retrigger masking, a dynamic threshold
  and crosstalk rejection
- Every setting can be changed in `idf.py menuconfig`

---

## 1. Pinout

| Function            | ESP32-S3 pin | ADC channel | MIDI note (GM)            |
|---------------------|-------------|-------------|---------------------------|
| **Kick** piezo      | **GPIO4**   | ADC1_CH3    | 36 – Bass Drum 1          |
| **Snare** piezo     | **GPIO5**   | ADC1_CH4    | 38 – Acoustic Snare       |
| **Hi-Hat** piezo    | **GPIO6**   | ADC1_CH5    | 42 Closed / 46 Open       |
| **Crash** piezo     | **GPIO7**   | ADC1_CH6    | 49 – Crash Cymbal 1       |
| **Tom** piezo       | **GPIO8**   | ADC1_CH7    | 48 – Hi-Mid Tom           |
| **Hi-Hat pedal** (touch sensor) | **GPIO9** | –   | 44 Pedal HH + CC#4        |
| USB D- (MIDI)       | GPIO19      | –           | native USB port           |
| USB D+ (MIDI)       | GPIO20      | –           | native USB port           |
| Console log (UART0) | GPIO43 TX / GPIO44 RX | – | "UART"/"COM" USB port     |

MIDI channel: **10** (General MIDI drums).

> Only **ADC1 pins (GPIO1–GPIO10)** can be used for pads. GPIO4–GPIO8 sit next to each
> other on the ESP32-S3-DevKitC-1 left header. GPIO19/20 are reserved for USB.

---

## 2. Wiring

### 2.1 Piezo pads (the same circuit for all 5)

A piezo can produce high voltages on a hard hit. Never connect it straight to the pin without a load and current-limiting resistor.

```
 Piezo RED (+) ──┬────────[ R2: 1 kΩ ]───────►  GPIOx (4/5/6/7/8)
                 │
            [ R1: 1 MΩ ]
                 │
 Piezo BLACK (–) ┴───────────────────────────►  GND
```

| Part | Value | Purpose |
|------|-------|---------|
| R1   | 1 MΩ  | Parallel load and discharge resistor across the piezo. Bleeds off voltage and dampens ringing. |
| R2   | 1 kΩ  | Series resistor before the ESP32 input pin. Limits current into the ESP32 GPIO. |

Per pad: **1× 1 MΩ resistor (in parallel), 1× 1 kΩ resistor (in series)**.
All piezo grounds go to an ESP32 **GND** pin.

**Mounting tips:** glue the piezo to the middle of the pad (under foam or rubber), use a cable
with a shield or twisted pair, and isolate the pads from each other mechanically (foam).
That isolation reduces crosstalk.

### 2.2 Hi-Hat pedal = touch sensor (default: TTP223 module)

```
 TTP223 module          ESP32-S3
   VCC  ─────────────── 3V3
   GND  ─────────────── GND
   I/O (OUT/SIG) ────── GPIO9
```

- Leave the TTP223 jumpers **open** (default: momentary, output HIGH while touched).
- **Touched = foot down = hi-hat CLOSED.** Not touched = OPEN.
  You can flip this in menuconfig (`Sensor ACTIVE = hi-hat CLOSED`).
- GPIO9 has an internal pull-down, so the pin reads "open" while nothing is connected.

#### Option B – no module, ESP32-S3 built-in capacitive touch
Connect **just a wire** from **GPIO9** to a piece of aluminium foil or a metal plate. Then set
`menuconfig → ESP Drums configuration → Hi-Hat pedal → Pedal input type →
ESP32-S3 built-in capacitive touch`.
Don't touch the plate while the board boots, because the baseline is measured at power-up.
If it's too sensitive or not sensitive enough, change `Touch threshold` (default 20 = 2 %).

#### Later – a real footswitch / hi-hat pedal
- Wire the switch between **GPIO9 and GND**.
- In menuconfig choose *Digital input* and turn on **"Digital input is active LOW"**.
  This enables the internal pull-up.
- If the switch is *normally closed*, also turn off "Sensor ACTIVE = hi-hat CLOSED".

### 2.3 USB

The **ESP32-S3-DevKitC-1** and most clones have **two USB-C ports**:

| Port label | Connected to | Used for |
|------------|--------------|----------|
| **USB** (native, "OTG") | GPIO19/20 | **MIDI to the computer.** This is the one you play through. |
| **UART** / "COM" | CP2102/CH343 USB-serial chip | **Flashing and log output** |

While you are developing, plug in both. To just play, the **USB** port alone is enough
(it also powers the board).

> **Boards with only ONE USB-C port** (S3 Zero, SuperMini, etc.): the port is the native USB.
> Flash through it (see 3.4). Once the firmware runs, the port becomes the MIDI device and
> the serial port disappears. Log output then needs a separate USB-UART adapter on GPIO43/44.

---

## 3. Build & flash (macOS)

All tools are already installed:

| What | Where |
|------|-------|
| ESP-IDF v6.0.3 | `~/esp/esp-idf-v6.0.3` |
| Toolchain (xtensa-esp-elf, etc.) | `~/.espressif` |
| cmake, ninja, dfu-util, ccache | Homebrew |
| esp_tinyusb (USB-MIDI) | downloaded into `managed_components/` on the first build |

### 3.1 Every new terminal: activate ESP-IDF
```bash
. ~/esp/esp-idf-v6.0.3/export.sh
cd "/Users/ivanbegonja/Documents/esp drums"
```
(Optional shortcut: add `alias get_idf='. ~/esp/esp-idf-v6.0.3/export.sh'` to `~/.zshrc`,
then just run `get_idf`.)

### 3.2 Build
```bash
idf.py build
```

### 3.3 Flash through the "UART" port (DevKitC with two ports – recommended)
```bash
ls /dev/cu.*          # find the port, e.g. /dev/cu.usbserial-0001, /dev/cu.SLAB_USBtoUART,
                      #                     /dev/cu.wchusbserial5A7B0012345
idf.py -p /dev/cu.usbserial-0001 flash monitor
```
`monitor` shows the log (pin map, every hit with its velocity). Exit with **Ctrl + ]**.

### 3.4 Flash through the native "USB" port (one-port boards)
The first time, `idf.py -p /dev/cu.usbmodemXXXX flash` works directly. After that, the
port runs the MIDI firmware, so put the chip into download mode by hand:

1. Hold **BOOT**
2. Press and release **RESET** (RST/EN)
3. Release **BOOT**
4. `ls /dev/cu.usbmodem*` → `idf.py -p /dev/cu.usbmodemXXXX flash`
5. Press **RESET** again to start the drums

### 3.5 Other useful commands
```bash
idf.py menuconfig                       # change pins, notes, thresholds, pedal type ...
idf.py -p <PORT> monitor                # log only
idf.py -p <PORT> erase-flash            # wipe the chip
idf.py fullclean && idf.py build        # clean rebuild
rm sdkconfig && idf.py build            # reset all settings to sdkconfig.defaults
```
In `menuconfig`, everything is under **"ESP Drums configuration"**.

---

## 4. Testing on the computer

1. Plug the **USB** port into the Mac.
2. **Audio MIDI Setup** → *Window → Show MIDI Studio* → **"ESP Drums"** should appear.
3. **GarageBand**: New project → *Software Instrument* → choose a **Drum Kit**. Hit the pads.
   GM note numbers map to the right drums automatically.
4. To see the raw MIDI, use [MIDI Monitor](https://www.snoize.com/midimonitor/) (free).

Messages sent on MIDI channel 10:

| Event | MIDI |
|-------|------|
| Pad hit | Note-On (velocity 1–127), then Note-Off 50 ms later |
| Hi-hat hit, pedal closed | Note 42 |
| Hi-hat hit, pedal open | Note 46 |
| Pedal pressed (open → closed) | Note 44 + CC#4 = 127 |
| Pedal released | CC#4 = 0 |
| USB connected | CC#4 = current pedal state |

---

## 5. Calibration / tuning

Each pad and piezo is different, so expect to tune.

1. `idf.py menuconfig` → *ESP Drums configuration → Debug / calibration* → turn on
   **"Print live signal levels"**. Then run `idf.py build flash monitor`.
2. The monitor prints the highest signal per pad every 250 ms:
   ```
   levels  Kick:  12  Snare:  18  HiHat:   9  Crash:  14  Tom:  11   pedal:open
   ```
3. **Threshold:** don't touch anything and note the noise level. Then hit other pads and
   note how much they bleed. Set each pad's threshold a bit higher (often 80–250).
4. **Max peak:** hit the pad as hard as you will ever play it. Set *Max peak* a little below
   that value (that hit then gives velocity 127).
5. Watch the `hit` lines (`Snare note= 38 vel= 97 (peak=1912)`) and fine-tune.

| Problem | Fix (menuconfig) |
|---------|------------------|
| One hit plays twice | Raise **Mask time** (default is 70 ms in `sdkconfig.defaults`, 40 ms in Kconfig) or **Dynamic threshold %** (default 50%) |
| Fast rolls lose notes | Lower **Mask time** (e.g. 25–40 ms) |
| Soft hits ignored | Lower that pad's **Threshold** (default 150 for Kick, 120 for others) |
| Everything too loud or too quiet | Change the pad's **Max peak** (default 2500 counts for velocity 127) |
| Soft playing feels dead | Lower **Velocity curve** < 100 (e.g. 60–80) for log-like response |
| Hard to control loud hits | Raise **Velocity curve** > 100 (e.g. 130–160) for exponential response |
| Hitting one pad triggers another | Raise **Crosstalk %** (default 20% → 35%) or check mechanical pad isolation |
| Soft ghost notes vanish during loud hits | Lower **Crosstalk %**, or set to 0 to disable |
| Velocity feels random or inconsistent | Raise **Scan time** (default 3 ms → 4–5 ms) |
| `ADC buffer overflow` in the log | Turn off live level printing (`CONFIG_DRUMS_LOG_LEVELS=n`) |

---

## 6. Architecture & Data Flow

The firmware is designed for ultra-low latency and zero jitter by isolating real-time audio/MIDI processing on **Core 1** while offloading all formatting, serial UART logging, and diagnostic calculations to **Core 0**.

```
  Core 1 (Real-Time DSP & MIDI - Priority 23):
  ===========================================
   ADC1 Continuous DMA (40 kHz total: 5 pads @ 8 kHz)
            │
            ▼
   drum_task (pinned to Core 1, alone)
     ├─ process_sample() per ADC reading
     │    ├─ Adaptive baseline tracking (24.8 fixed-point EMA)
     │    ├─ State machine: IDLE → SCAN → MASK → Dynamic Decay
     │    ├─ Peak detection over scan window (default 3 ms)
     │    ├─ Crosstalk rejection relative to other recent pad peaks
     │    ├─ Velocity calculation with power curve exponent
     │    └─ Dynamic Hi-Hat note mapping (closed 42 / open 46)
     ├─ Hi-Hat pedal polling & debounce (GPIO / touch)
     ├─ Scheduled hardware Note-Off generation (default 50 ms)
     ├─ TinyUSB MIDI packet transmission (Note-On, Note-Off, CC#4) ──► USB Host
     └─ Drain TinyUSB incoming RX queue
            │
            │ Non-blocking FreeRTOS queue (drum_pads_event_queue, cap 32)
            ▼
  Core 0 (System & Logging - Priority 2):
  ======================================
   log_task (pinned to Core 0)
     ├─ UART hit telemetry (pad, MIDI note, velocity, peak ADC counts)
     ├─ Crosstalk rejection warnings
     ├─ Pedal state transition logs (OPEN / CLOSED)
     ├─ USB connection / disconnection events
     ├─ ADC DMA buffer overflow monitoring
     └─ Live calibration level monitor (optional 250 ms periodic print)
```

---

## 7. What's What in the Codebase

| File | Role & Key Responsibilities |
|------|-----------------------------|
| [`main/main.c`](main/main.c) | **Firmware Entry & Logging Task**<br>• `app_main()`: Initializes USB-MIDI, hi-hat pedal, and drum pad engine in order, then spawns the logging task.<br>• `log_task`: Core 0 consumer task. Receives `drum_event_t` records from the FreeRTOS event queue. Formats and prints hit telemetry, crosstalk drops, pedal state changes, and USB status without ever blocking real-time hit detection.<br>• Live level monitoring: When `CONFIG_DRUMS_LOG_LEVELS` is enabled, samples the peak values seen across all pads every 250 ms. |
| [`main/drum_pads.c`](main/drum_pads.c)<br>[`main/drum_pads.h`](main/drum_pads.h) | **ADC DMA Engine, Trigger State Machine & DSP**<br>• `drum_pads_start()`: Configures ADC1 continuous DMA mode (12-bit, 12 dB attenuation, 40 kHz total aggregate rate across 5 channels) and launches `drum_task` at `configMAX_PRIORITIES - 2` on Core 1.<br>• `process_sample()`: Runs the per-pad state machine (`ST_IDLE`, `ST_SCAN`, `ST_MASK`) and continuous baseline tracking.<br>• `is_crosstalk()`: Compares new hits against recent hits on other pads within `CONFIG_DRUMS_CROSSTALK_WINDOW_MS`. Rejects hits below `CONFIG_DRUMS_CROSSTALK_PERCENT` of the stronger hit.<br>• `peak_to_velocity()`: Normalizes the peak ADC amplitude between `threshold` and `max_peak`, applying `CONFIG_DRUMS_VELOCITY_CURVE` exponent.<br>• `handle_note_offs()`: Manages automated microsecond Note-Off timers (`CONFIG_DRUMS_NOTE_OFF_MS`).<br>• `drum_pads_take_levels()`: Atomic peak-level extraction for calibration mode. |
| [`main/hihat_pedal.c`](main/hihat_pedal.c)<br>[`main/hihat_pedal.h`](main/hihat_pedal.h) | **Hi-Hat Pedal Driver & Controller Generator**<br>• Supports two hardware modes selectable in menuconfig:<br>&nbsp;&nbsp;1. **Digital GPIO:** Reads active HIGH (TTP223 capacitive touch module) or active LOW (footswitch to GND with internal pull-up).<br>&nbsp;&nbsp;2. **Capacitive Touch:** Uses the ESP32-S3 internal touch peripheral (`driver/touch_sens.h` on GPIO1–14). Calibrates an untouched baseline benchmark during boot and applies a per-mille threshold.<br>• `hihat_pedal_poll()`: Software debouncing filter (`CONFIG_DRUMS_PEDAL_DEBOUNCE_MS`).<br>• Transmits MIDI CC#4 (foot controller: 127 = closed, 0 = open) and optional "pedal chick" note (default note 44, velocity 80) when closed. |
| [`main/usb_midi.c`](main/usb_midi.c)<br>[`main/usb_midi.h`](main/usb_midi.h) | **USB-MIDI Class-Compliant Stack (TinyUSB)**<br>• Uses the `espressif/esp_tinyusb` component.<br>• Sets up USB descriptors: Vendor ID / Product ID, Manufacturer (`"DIY"`), Product Name (`"ESP Drums"`), and MIDI Interface (`"ESP Drums MIDI"`).<br>• Exposes fast MIDI output helpers: `usb_midi_note_on()`, `usb_midi_note_off()`, and `usb_midi_cc()`.<br>• `usb_midi_drain_rx()`: Drains incoming host MIDI packets to prevent USB endpoint stalling. |
| [`main/Kconfig.projbuild`](main/Kconfig.projbuild) | **Menuconfig Definitions**<br>• Exposes all settings under `ESP Drums configuration`: MIDI channel & Note-Off duration, scan/mask times, dynamic threshold & decay, velocity curve exponent, crosstalk rejection, per-pad GPIOs, MIDI notes, thresholds, max peaks, pedal mode, and debug logging toggles. |
| [`sdkconfig.defaults`](sdkconfig.defaults) | **Default Project Presets**<br>• Target: ESP32-S3 (`CONFIG_IDF_TARGET="esp32s3"`).<br>• Enables TinyUSB MIDI count = 1.<br>• Directs system console to UART0 (freeing the native USB port for dedicated MIDI).<br>• Sets 1 kHz FreeRTOS tick rate (`CONFIG_FREERTOS_HZ=1000`) and `-O2` compiler optimization (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`).<br>• Sets default retrigger mask to 70 ms (`CONFIG_DRUMS_MASK_TIME_MS=70`) to prevent kick piezo double-triggering out of the box. |

---

## 8. Trigger State Machine & Signal Processing Details

Every pad is sampled independently at **8 kHz** (40 kHz aggregate across all 5 pads via ADC1 DMA). For every single ADC sample, `process_sample()` runs the following algorithm:

1. **Adaptive Baseline Tracking:**
   $$\text{sig} = \max\left(0, \; \text{raw} - (\text{baseline\_q8} \gg 8)\right)$$
   When the pad is idle ($\text{sig} < \frac{\text{threshold}}{2}$), `baseline_q8` (24.8 fixed-point) slowly tracks thermal and electrical DC drift with an exponential moving average (time constant ~128 ms).
2. **`ST_IDLE` (Waiting for hit):**
   A hit begins when $\text{sig} \ge \text{active\_threshold}$.
   - If in the dynamic decay window, $\text{active\_threshold}$ decays linearly:
     $$\text{active\_threshold} = \max\left(\text{threshold}, \; \text{dyn\_start} \cdot \frac{\text{DECAY\_SAMPLES} - \text{decay\_pos}}{\text{DECAY\_SAMPLES}}\right)$$
   - On trigger, pad switches to `ST_SCAN` and sets the scan counter to `CONFIG_DRUMS_SCAN_TIME_MS` (24 samples at 8 kHz = 3 ms).
3. **`ST_SCAN` (Peak search window):**
   - Tracks the highest ADC value reached by the initial wave.
   - When the counter reaches 0:
     - **Crosstalk Check:** Compares normalized amplitude against recent hits on other pads within `CONFIG_DRUMS_CROSSTALK_WINDOW_MS`. If dropped, posts `DRUM_EVT_XTALK`.
     - **Hi-Hat Note Selection:** If pad is Hi-Hat, chooses closed (42) or open (46) based on `hihat_pedal_is_closed()`.
     - **Velocity Calculation:**
       $$\text{norm} = \text{clamp}\left(\frac{\text{peak} - \text{threshold}}{\text{max\_peak} - \text{threshold}}, \; 0.0, \; 1.0\right)$$
       $$\text{velocity} = 1 + \left\lfloor 126 \cdot \left(\text{norm}\right)^{\frac{\text{curve}}{100}} + 0.5 \right\rfloor$$
     - **MIDI Note-On:** Emitted immediately over USB-MIDI.
     - **Note-Off Scheduled:** Scheduled for $t_{\text{now}} + \text{CONFIG\_DRUMS\_NOTE\_OFF\_MS}$.
     - **Transition to `ST_MASK`:** Sets dynamic threshold start to $\text{peak} \cdot \frac{\text{CONFIG\_DRUMS\_DYN\_THRESHOLD\_PERCENT}}{100}$.
4. **`ST_MASK` (Retrigger suppression):**
   - The pad ignores all signals for `CONFIG_DRUMS_MASK_TIME_MS` (default 70 ms) to suppress physical mechanical piezo ringing.
5. **Dynamic Threshold Decay:**
   - Pad returns to `ST_IDLE`, with the threshold starting at $\text{dyn\_start}$ and linearly decaying back to base $\text{threshold}$ over `CONFIG_DRUMS_DYN_DECAY_MS` (default 100 ms). This rejects ringing tails after hard hits while allowing quick, deliberate follow-up hits.
