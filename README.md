# AI-Powered Smart Scientific Calculator & ESP32 Voice Assistant (PCB + Firmware)

Custom 2-layer PCB manufacturing package and ESP32 I2S voice-to-LLM embedded firmware designed around a compact **Casio fx-991MS-inspired smart calculator** and **ESP32 voice assistant** architecture.

---

## Repository Contents

```text
AI-Smart-Calculator-ESP32-PCB/
├── pcb/
│   ├── gerbers/                  # Fabrication Gerbers (.GTL, .GBL, .GTS, .GBS, .GTO, .GBO, .GKO) & Drill (.DRL)
│   ├── BOM_Calculator.csv        # Complete LCSC/JLCPCB Bill of Materials (ESP32-WROOM-32E, TP4056, 20x tactile keys)
│   ├── PickAndPlace_Calculator.xlsx # SMT centroid coordinates for assembly
│   ├── calculator.dsn            # Specctra design file
│   └── calculator_final.ses      # Routed Specctra session file
└── firmware/
    └── esp32_ai_assistant.ino    # ESP32 I2S Mic -> STT -> LLM -> 128x64 OLED Display firmware
```

---

## Hardware Architecture
- **MCU / Wireless:** `ESP32-WROOM-32E` (PCB layout) / `ESP32-S3` voice-assistant target
- **Audio Input:** `INMP441` digital I2S MEMS microphone (`16 kHz`, 16-bit mono PCM)
- **Display:** `128×64` SSD1306 I2C OLED
- **Power & Charging:** `TP4056` Li-Po battery charger + decoupling network (`100 nF` / `10 µF`) + 20-key SMD tactile matrix

### Confirmed ESP32 Firmware Pinout (`esp32_ai_assistant.ino`)

| Peripheral | Signal | ESP32 GPIO |
|---|---|---|
| **INMP441 Mic** | `WS` | `GPIO 15` |
| **INMP441 Mic** | `SCK` | `GPIO 14` |
| **INMP441 Mic** | `SD` | `GPIO 32` |
| **INMP441 Mic** | `L/R` | `GND` |
| **SSD1306 OLED** | `SDA / SCL` | `GPIO 21 / GPIO 22` |
| **Trigger Button** | `BOOT` | `GPIO 0` (Active LOW) |

---

## Firmware Setup
1. Install libraries in Arduino IDE: `Adafruit SSD1306`, `Adafruit GFX Library`, and `ArduinoJson` (v6+).
2. Open `firmware/esp32_ai_assistant.ino` and replace `YOUR_WIFI_SSID`, `YOUR_WIFI_PASSWORD`, and your API key placeholders.
3. Upload to ESP32, press the `BOOT` button to record a 4-second voice query, and view the AI response directly on the OLED.

---

## Development Note
System concept, hardware architecture (`ESP32` + `INMP441` + `TP4056` + keypad), and custom 2-layer PCB fabrication package were put together by **Sriram C.**; the ESP32 I2S voice-to-LLM firmware was built using **AI-assisted prototyping**.
