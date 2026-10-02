# Scroll Ring MIDI Thing

ESP32-S3 firmware that pairs with a cheap Bluetooth "TikTok scroll ring" (or
any Bluetooth LE HID remote), learns what each button sends, and turns the
presses into **USB MIDI** notes. You configure it from a single web page served
by the ESP32's own Wi-Fi access point.

```
 ring ──BLE HID──▶ ESP32-S3 ──USB MIDI──▶ computer / synth / DAW
                      ▲
                      └── Wi-Fi AP + web UI (phone or laptop)
```

## Hardware

* An **ESP32-S3** board. It is the only common ESP32 that has both Bluetooth LE
  and native USB, which USB MIDI needs. Any S3 module with 4 MB or more of
  flash works.
  * Boards with two USB ports (e.g. ESP32-S3-DevKitC-1): flash and read logs
    over the **UART/COM** port, and plug the **USB/OTG** port into the
    computer for MIDI. Use the `esp32s3` environment.
  * Boards with only one USB port (e.g. Seeed XIAO ESP32S3): use the
    `esp32s3-single-usb` environment. That port then provides both MIDI and a
    serial console.
* The ring must be **Bluetooth Low Energy**. Nearly all of these rings are. A
  classic-Bluetooth-only device will never appear in the scan, because the S3
  has no classic Bluetooth.

## Build & flash

PlatformIO, using the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform (Arduino-ESP32 3.x). It is pulled in automatically.

```sh
pio run -e esp32s3 -t upload        # or -e esp32s3-single-usb
pio device monitor                  # optional: logs at 115200
pio test -e native                  # host unit tests for the matching engine
```

If an upload on a single-USB board won't start, hold **BOOT** while tapping
**RESET** to enter the bootloader.

## Using it

1. Power the board. Join the Wi-Fi network **`ScrollRingMIDI-XXXX`**
   (password **`ringmidi`**). The page usually pops up as a captive portal.
   Otherwise open <http://192.168.4.1/>.
2. **Unpair the ring from your phone** (or turn the phone's Bluetooth off).
   Most rings reconnect only to the last host they were bonded with.
3. Put the ring in pairing mode and press **Scan**. HID devices are listed
   first. Press **Pair**. The ESP32 bonds with the ring, remembers it, and
   reconnects automatically from then on. A sleeping ring wakes and reconnects
   when you press any button.
4. Press **+ Add & learn**, then press the ring button you want to map
   **3 times**, pausing briefly between presses. Pick the note, channel and
   velocity. Repeat for each button.
5. Plug the ESP32's USB port into your computer. It shows up as a
   class-compliant MIDI device called **Scroll Ring MIDI**.

The **Monitor** panel shows every raw HID report and every note sent, which is
the quickest way to see what your ring is doing. **Test** sends a mapping's
note without the ring. **MIDI panic** sends all-notes-off. **Settings &
backup** lets you export and import mappings as JSON.

## How learning works

The firmware does not parse HID descriptors. It treats each input report as
opaque bytes tagged with the GATT handle it arrived on. That covers rings that
pretend to be a keyboard, a media remote, a mouse or a touchscreen.

* **Gestures.** Reports are grouped into gestures. A gesture starts at the
  first non-blank report and ends at an all-zero report or after 150 ms of
  silence. A key press looks like `[key down, all zeros]`. An emulated swipe
  looks like `[touch down, move, move, …, lift]`.
* **Several presses.** Each press is captured, and the presses are combined
  report by report. Bytes that differ between presses (counters, jitter)
  become wildcards (`??`).
* **Trigger (note on).** The trigger is the earliest report in the gesture
  that cannot appear in any other mapping's gesture. Rings often emulate
  swipe-up and swipe-down from the *same* touch-down point, so the shared
  first report is ambiguous. In that case both mappings trigger on the first
  report where the two gestures diverge. If a new mapping makes an older one
  ambiguous, the older one's trigger is moved automatically.
* **Release (note off).** If the gesture ends in a distinctive report (key up
  or finger lift), the note is held until that report arrives, so holding a
  button holds the note. Otherwise the note ends when the gesture ends. You
  can also choose a fixed note length.
* A button can drive several mappings, and so several notes. If two mappings
  can't be told apart, the learn summary warns you and both fire.

Every mapping's **Details** section shows the learned gesture. You can tap a
different report to trigger on it, or hand-edit the trigger and release
patterns. The pattern format is `<handle hex>:<bytes>`, with `??` for any byte,
for example `0030:01 ?? 40 01 70 02`.

## Code layout

| File | What |
|---|---|
| `src/pattern.*` | Raw report and masked-pattern matching (pure C++) |
| `src/engine.*` | Gesture segmentation, learning, disambiguation, note on/off (pure C++, unit tested) |
| `src/ble_hid_host.*` | NimBLE central: scan, bond, subscribe to HID input reports, auto-reconnect |
| `src/web_ui.*` | Wi-Fi AP, captive-portal DNS, JSON API |
| `src/config_store.*` | Mappings ⇄ JSON, saved to LittleFS |
| `src/main.cpp` | Wiring, USB MIDI output, engine task |
| `web/index.html` | The whole UI (embedded into the firmware at build time) |
| `test/test_engine` | Unity tests for the engine (`pio test -e native`) |

Reports flow BLE task → queue → engine task (which owns MIDI output), so
serving web pages never delays a note.

Compile-time settings such as the AP name, password and USB name live in
`src/app_config.h`. Override them from `build_flags`.

## Roadmap / extending

* **CC mappings.** `Mapping::action` is the hook. Add a `MidiAction` value,
  handle it in `Engine::noteOn` / `noteOff`, and add it to the UI. Planned
  first: momentary CC (value on press, 0 on release). Then relative
  "scroll → CC increment" mappings driven by repeated gestures.
* The HID report descriptor is already read and shown under *HID details*.
  Parsing it would allow mappings for continuous axes (touch X/Y → CC).
