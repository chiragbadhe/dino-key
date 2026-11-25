# Dino Key

A tiny touch keychain with a face. It's built on an ESP32-C3 SuperMini and a 1.3" OLED, and one touch sensor controls everything: a face that blinks and loves being petted, a clock, a stopwatch and a Dino Run game.

Open `DinoKey.html` in your browser to see every screen, the wiring diagram and a live demo.

## Files

| File | What it is |
|---|---|
| `DinoKey.ino` | Arduino code. Upload this to the ESP32-C3. |
| `DinoKey.html` | Project page: screens, parts, wiring and a live demo. |
| `README.md` | This file. |

## Parts

- ESP32-C3 SuperMini
- 1.3" OLED display, SH1106, 128×64, I2C
- TTP223 touch sensor
- 3.7 V 350 mAh LiPo battery
- TP4056 charger module
- Slide switch

No resistors needed.

## Wiring

| From | To |
|---|---|
| OLED VCC | ESP32 3V3 |
| OLED GND | ESP32 GND |
| OLED SDA | ESP32 GPIO 6 |
| OLED SCL | ESP32 GPIO 7 |
| Touch VCC | ESP32 3V3 |
| Touch GND | ESP32 GND |
| Touch I/O (OUT) | ESP32 GPIO 3 |
| Battery red (+) | Charger B+ |
| Battery black (−) | Charger B− |
| Charger OUT+ | Slide switch (middle pin) |
| Slide switch (side pin) | ESP32 5V |
| Charger OUT− | ESP32 GND |

Power path: **Battery → Charger → Switch → ESP32 5V**

> ⚠️ Check the battery's red and black wires twice before connecting. Reversed, they can damage the charger and the ESP32.
> ⚠️ Never connect the battery straight to a GPIO pin. It's 4.2 V when full, and the pins take 3.3 V.

## Modes

**Startup → Home → Clock → Stopwatch → Dino Run → Faces → Battery**

| Mode | What it shows |
|---|---|
| Startup | "DINO KEY" title with the dino running in, shown at power-on |
| Home | Time and date in the top bar, the face below |
| Clock | Big 12-hour time with day and date |
| Stopwatch | Minutes, seconds and hundredths |
| Dino Run | The jump-the-cactus game, with a high score |
| Faces | 12 full-screen faces |
| Battery | Needs 2 resistors (see the extras section) |

## Touch controls

| Touch | What it does |
|---|---|
| Tap | Next mode |
| Double tap | Next face · start/stop stopwatch · start Dino Run |
| Hold (0.6 s) | Pet it (Home/Faces) · reset stopwatch |
| Hold 3 s | Sleep (touch again to wake) |
| Any touch in Dino Run | Jump |

- When you pet it, its eyes turn to hearts, it blushes and hearts float up. When you let go, it laughs.
- Ignore it for 1 minute on Home and it gets sad.
- It auto-sleeps after 3 minutes without a touch, but not while the stopwatch runs or a game is on.

## Setup

1. Install **Arduino IDE**.
2. **Tools → Board → Boards Manager**: install **esp32 by Espressif Systems**.
3. **Tools → Manage Libraries**: install **U8g2** by olikraus.
4. Open `DinoKey.ino` and put your Wi-Fi name and password at the top:
   ```cpp
   const char* WIFI_SSID = "YourWiFiName";
   const char* WIFI_PASS = "YourWiFiPassword";
   ```
   Wi-Fi is used once at power-on to get the time (India time by default; change `TZ_INFO` for another time zone). It must be a 2.4 GHz network.
5. Set **Tools → Board** to **ESP32C3 Dev Module** and **USB CDC On Boot** to **Enabled**.
6. Turn the slide switch **OFF**, connect USB and click **Upload**.

## Uploading tips

If you see `Failed to connect to ESP32-C3: No serial data received`:

1. Unplug USB.
2. Hold the **BOOT** button.
3. Plug USB in while holding it, then release.
4. Choose the port again (`/dev/cu.usbmodem…` on Mac) and upload.
5. When it finishes, press **RST**.

Also try:
- Use a USB cable that carries data (some only charge).
- Set Upload Speed to 115200.
- Remember that when Dino Key is asleep it doesn't show up on USB. Touch it to wake it, or use the BOOT steps above.

## Troubleshooting

| Problem | Fix |
|---|---|
| `U8g2lib.h: No such file` | Install the **U8g2** library |
| Screen garbled or shifted | Your OLED may be SSD1306: change `SH1106` to `SSD1306` in the `U8G2_...` line |
| Touch works backwards | The sensor is active-low: bridge its **A** pad |
| Clock shows `--:--` | Wrong Wi-Fi name or password, or a 5 GHz network |
| Battery screen says "No battery sensor" | Normal without resistors (see the extras section) |

## Charging

Charge through the **TP4056 module's USB port**. Turn the slide switch **OFF** before plugging USB into the ESP32 to upload code.

## Optional extras

**Battery percentage:** add two equal resistors (any value from 10k to 470k):

```
Battery + → resistor → GPIO 1 → resistor → GND
```
Then set `#define HAS_BAT_SENSE 1` in the code.

**Charging face:** wire two more equal resistors from the charger's IN+ (5V) to GPIO 4 in the same way, then set `#define PIN_USB 4`.

## Settings in the code

| Setting | Default | Meaning |
|---|---|---|
| `TZ_INFO` | `IST-5:30` | Time zone |
| `AUTO_SLEEP_MS` | 3 minutes | Auto-sleep time |
| `LONELY_MS` | 1 minute | How long until it gets sad |
| `HAS_BAT_SENSE` | `0` | Battery resistors fitted? |
| `PIN_USB` | `-1` | Charging-detect pin (`-1` = off) |
