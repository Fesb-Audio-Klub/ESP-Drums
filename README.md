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

A piezo can produce **tens of volts** on a hard hit. Never connect it straight to the pin.

```
                                   D1  (BAT85 / BAT43 / 1N5819 Schottky)
                                  3V3 ──|◄──┐   (cathode/stripe toward 3V3)
                                            │
 Piezo RED (+) ──┬────────[ R2 1 kΩ ]───────┼──────────►  GPIOx (4/5/6/7/8)
                 │                          │
              [ R1 1 MΩ ]                   └──|◄── GND
                 │                         D2  (stripe toward GPIO, anode to GND)
 Piezo BLACK (–) ┴─────────────────────────────────────►  GND
```

| Part | Value | Purpose |
|------|-------|---------|
| R1   | 1 MΩ (470 kΩ–1 MΩ) | Load and discharge resistor across the piezo. A lower value gives a weaker signal and less ringing. |
| R2   | 1 kΩ (up to 10 kΩ) | Limits the current into the clamp diodes. |
| D1   | Schottky to 3V3 | Clamps positive spikes to about 3.5 V. |
| D2   | Schottky to GND | Clamps the negative half-wave. |

Per pad: **1× 1 MΩ, 1× 1 kΩ, 2× Schottky diodes** (BAT85, BAT43, BAT46, 1N5819, etc.).
All piezo grounds go to an ESP32 **GND** pin.

*Quick test without diodes:* use **R1 = 1 MΩ** and **R2 = 10 kΩ**. The chip's internal
protection diodes then do the clamping. That's fine on a desk, but add the Schottkys for real playing.

*Alternative to D1/D2:* a **3.3 V zener** from the GPIO to GND (stripe toward the GPIO).
It is simpler, but it starts conducting around 2.5 V, which squashes the loudest hits a little.

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
| One hit plays twice | Raise **Mask time** (40 → 60 ms) or **Dynamic threshold %** |
| Fast rolls lose notes | Lower **Mask time** (e.g. 25 ms) |
| Soft hits ignored | Lower that pad's **Threshold** |
| Everything too loud or too quiet | Change the pad's **Max peak** |
| Soft playing feels dead | **Velocity curve** < 100 (e.g. 60) |
| Hard to control loud hits | **Velocity curve** > 100 (e.g. 150) |
| Hitting one pad triggers another | Raise **Crosstalk %** (20 → 35), improve mechanical isolation |
| Soft ghost notes vanish during loud hits | Lower **Crosstalk %**, or 0 = off |
| Velocity feels random | Raise **Scan time** (3 → 4–5 ms) |
| `ADC buffer overflow` in the log | Shouldn't happen. Turn off level printing. |

---

## 6. How it works

```
 piezo ─► clamp circuit ─► ADC1 (DMA, 40 kHz total = 8 kHz/pad)
                                 │
                       drum task (core 1, top priority)
                                 │  per pad: IDLE → SCAN (find peak) → MASK
                                 │  velocity = curve((peak - threshold)/(max_peak - threshold))
                                 │  crosstalk filter, hi-hat note chosen from pedal state
                                 ▼
                           TinyUSB MIDI  ─► computer
```

| File | Content |
|------|---------|
| [`main/main.c`](main/main.c) | Startup, low-priority log task |
| [`main/drum_pads.c`](main/drum_pads.c) | ADC DMA sampling, trigger state machine, velocity, crosstalk, note-off |
| [`main/hihat_pedal.c`](main/hihat_pedal.c) | Hi-hat switch: TTP223/footswitch (GPIO) or built-in capacitive touch, debounce |
| [`main/usb_midi.c`](main/usb_midi.c) | USB descriptors ("ESP Drums") and MIDI send helpers |
| [`main/Kconfig.projbuild`](main/Kconfig.projbuild) | All menuconfig options |
| [`sdkconfig.defaults`](sdkconfig.defaults) | Target esp32s3, USB-MIDI, console on UART, 1 kHz tick |

The trigger state machine, per pad and per sample:
- **IDLE:** the signal (minus a slowly tracked baseline) goes above the threshold, so start a hit.
- **SCAN (3 ms):** keep the highest value. At the end, send a Note-On with a velocity based on that peak.
- **MASK (40 ms):** ignore the pad so the piezo's ringing can't retrigger it.
- After the mask, the threshold starts at 50 % of the last peak and decays linearly back to normal
  over 100 ms. Soft, fast notes still trigger, but ringing after hard hits doesn't.
