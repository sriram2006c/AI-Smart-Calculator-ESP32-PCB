// ============================================================
//  ESP32 AI ASSISTANT — FINAL WORKING VERSION
//  Mic → Whisper STT → Claude AI → OLED Display
//
//  CONFIRMED WIRING:
//    INMP441: WS->GPIO15 | SCK->GPIO14 | SD->GPIO32 | L/R->GND
//    OLED:    SDA->GPIO21 | SCL->GPIO22
//    BUTTON:  BOOT button (GPIO 0) — built-in, active LOW
//
//  CONFIRMED I2S CONFIG:
//    channel_format       = I2S_CHANNEL_FMT_ALL_RIGHT
//    communication_format = I2S_COMM_FORMAT_STAND_I2S
//
//  LIBRARIES (install via Arduino Library Manager):
//    - Adafruit SSD1306
//    - Adafruit GFX Library
//    - ArduinoJson by Benoit Blanchon (v6+)
//
//  FILL IN WiFi + API keys below before uploading!
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
//  !! FILL THESE IN !!
// ============================================================
const char* WIFI_SSID         = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD     = "YOUR_WIFI_PASSWORD";
const char* OPENAI_API_KEY    = "YOUR_OPENAI_API_KEY";       // platform.openai.com
const char* ANTHROPIC_API_KEY = "YOUR_ANTHROPIC_API_KEY";   // console.anthropic.com

// ============================================================
//  Pin definitions
// ============================================================
#define I2S_WS    15
#define I2S_SCK   14
#define I2S_SD    32
#define BTN_PIN   0     // BOOT button, active LOW

// ============================================================
//  Audio config
// ============================================================
#define SAMPLE_RATE      16000
#define MAX_SECONDS      4
// ALL_RIGHT gives interleaved stereo — we only keep RIGHT samples
// so effective samples = SAMPLE_RATE * MAX_SECONDS
#define MAX_SAMPLES      (SAMPLE_RATE * MAX_SECONDS)
#define WAV_HEADER_SIZE  44

// ============================================================
//  OLED
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ============================================================
//  Buffers
// ============================================================
int16_t* audioBuffer = nullptr;   // final 16-bit mono PCM
uint8_t* wavBuffer   = nullptr;   // WAV header + PCM

// ============================================================
//  OLED helpers
// ============================================================
void oledMsg(const char* l1, const char* l2 = "", const char* l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println(l1);
  display.setCursor(0, 22); display.println(l2);
  display.setCursor(0, 44); display.println(l3);
  display.display();
}

void oledAnswer(String question, String answer) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Question truncated to top line
  display.setCursor(0, 0);
  String q = "Q: " + question;
  if (q.length() > 21) q = q.substring(0, 20) + "~";
  display.println(q);

  // Divider line
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Answer with word wrap
  display.setCursor(0, 13);
  display.setTextWrap(true);
  display.println(answer);
  display.display();
}

// ============================================================
//  WAV header (PCM 16-bit mono 16kHz)
// ============================================================
void buildWavHeader(uint8_t* h, uint32_t dataBytes) {
  uint32_t totalSize   = dataBytes + 36;
  uint32_t sampleRate  = SAMPLE_RATE;   // local var so we can take &
  uint32_t byteRate    = SAMPLE_RATE * 2;
  uint16_t blockAlign  = 2;
  uint16_t bitsPerSamp = 16;
  uint16_t channels    = 1;
  uint16_t audioFmt    = 1;   // PCM
  uint32_t subchunk1   = 16;

  memcpy(h,      "RIFF",       4);
  memcpy(h +  4, &totalSize,   4);
  memcpy(h +  8, "WAVE",       4);
  memcpy(h + 12, "fmt ",       4);
  memcpy(h + 16, &subchunk1,   4);
  memcpy(h + 20, &audioFmt,    2);
  memcpy(h + 22, &channels,    2);
  memcpy(h + 24, &sampleRate,  4);   // fixed: use local variable
  memcpy(h + 28, &byteRate,    4);
  memcpy(h + 32, &blockAlign,  2);
  memcpy(h + 34, &bitsPerSamp, 2);
  memcpy(h + 36, "data",       4);
  memcpy(h + 40, &dataBytes,   4);
}

// ============================================================
//  I2S init — confirmed working config
// ============================================================
void initI2S() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ALL_RIGHT,   // confirmed
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // confirmed
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 4,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// ============================================================
//  Record audio while BOOT button held
//  ALL_RIGHT gives interleaved: [RIGHT, LEFT, RIGHT, LEFT...]
//  We take every other sample (RIGHT = real data, LEFT = zero)
// ============================================================
uint32_t recordAudio() {
  uint32_t count = 0;
  int32_t  raw[256];
  size_t   bytesRead;

  // Flush stale DMA data before recording
  i2s_zero_dma_buffer(I2S_NUM_0);
  delay(50);

  oledMsg(" RECORDING...", " Speak now!", " Release to send");
  Serial.println(">>> Recording started — hold button and speak!");

  while (digitalRead(BTN_PIN) == LOW && count < MAX_SAMPLES) {
    i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
    int total = bytesRead / sizeof(int32_t);

    for (int i = 0; i < total; i += 2) {
      if (count >= MAX_SAMPLES) break;
      audioBuffer[count++] = (int16_t)(raw[i] >> 8);
    }

    // Print sample count every ~0.5 sec so you can see it growing
    if (count % 8000 == 0 && count > 0) {
      Serial.printf("  ... %.1f sec recorded\n", (float)count / SAMPLE_RATE);
    }
  }

  Serial.printf(">>> Recording stopped: %u samples = %.2f sec\n", count, (float)count / SAMPLE_RATE);
  return count;
}

// ============================================================
//  Whisper STT — returns transcribed text
// ============================================================
String transcribeAudio(uint8_t* wav, uint32_t wavLen) {
  oledMsg(" Transcribing", " your speech...", "");
  Serial.printf("Sending %u bytes to Whisper...\n", wavLen);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.begin(client, "https://api.openai.com/v1/audio/transcriptions");
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.setTimeout(20000);

  String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // Build multipart body
  String partHead =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    "whisper-1\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
    "en\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String partTail = "\r\n--" + boundary + "--\r\n";

  uint32_t bodyLen = partHead.length() + wavLen + partTail.length();
  uint8_t* body = (uint8_t*)malloc(bodyLen);
  if (!body) { Serial.println("OOM!"); return ""; }

  memcpy(body,                              partHead.c_str(), partHead.length());
  memcpy(body + partHead.length(),          wav,              wavLen);
  memcpy(body + partHead.length() + wavLen, partTail.c_str(), partTail.length());

  int code = http.POST(body, bodyLen);
  free(body);

  String result = "";
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      result = doc["text"].as<String>();
      result.trim();
      Serial.println("Whisper: " + result);
    }
  } else {
    Serial.printf("Whisper error %d: %s\n", code, http.getString().c_str());
    oledMsg(" Whisper Error", ("Code: " + String(code)).c_str(), "Try again");
    delay(3000);
  }
  http.end();
  return result;
}

// ============================================================
//  Claude AI — returns short answer
// ============================================================
String askClaude(String question) {
  oledMsg(" Thinking...", "", "");
  Serial.println("Asking Claude: " + question);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.begin(client, "https://api.anthropic.com/v1/messages");
  http.addHeader("x-api-key",          ANTHROPIC_API_KEY);
  http.addHeader("anthropic-version",  "2023-06-01");
  http.addHeader("Content-Type",       "application/json");
  http.setTimeout(20000);

  DynamicJsonDocument req(1024);
  req["model"]      = "claude-haiku-4-5";
  req["max_tokens"] = 80;
  req["system"]     =
    "You are a concise assistant on a tiny ESP32 device with a 128x64 OLED screen. "
    "Reply in ONE sentence, strictly under 100 characters. No punctuation flourishes.";

  JsonArray msgs = req.createNestedArray("messages");
  JsonObject m   = msgs.createNestedObject();
  m["role"]    = "user";
  m["content"] = question;

  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  String answer = "Error. Try again.";

  if (code == 200) {
    DynamicJsonDocument res(2048);
    if (deserializeJson(res, http.getString()) == DeserializationError::Ok) {
      answer = res["content"][0]["text"].as<String>();
      answer.trim();
      Serial.println("Claude: " + answer);
    }
  } else {
    Serial.printf("Claude error %d: %s\n", code, http.getString().c_str());
    oledMsg(" Claude Error", ("Code: " + String(code)).c_str(), "Try again");
    delay(3000);
  }
  http.end();
  return answer;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);

  // OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed!"); while (true);
  }
  oledMsg("  Booting...", "", "");

  // Allocate buffers — prefer PSRAM, fall back to heap
  audioBuffer = (int16_t*)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
  if (!audioBuffer) {
    Serial.println("No PSRAM — using heap (2s max)");
    audioBuffer = (int16_t*)malloc(SAMPLE_RATE * 2 * sizeof(int16_t));
  }
  wavBuffer = (uint8_t*)ps_malloc(MAX_SAMPLES * 2 + WAV_HEADER_SIZE);
  if (!wavBuffer) {
    wavBuffer = (uint8_t*)malloc(SAMPLE_RATE * 2 * 2 + WAV_HEADER_SIZE);
  }
  if (!audioBuffer || !wavBuffer) {
    oledMsg("Memory Error!", "Reduce", "MAX_SECONDS"); while (true);
  }

  // WiFi
  oledMsg("  Connecting", "  to WiFi...", "");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    oledMsg(" WiFi Failed!", " Check creds", ""); while (true);
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // I2S
  initI2S();

  oledMsg("    Ready!", " Hold BOOT btn", "   to speak");
  Serial.println("Ready! Hold BOOT button (GPIO 0) to speak.");
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  // Ensure button is fully released before listening for next press
  while (digitalRead(BTN_PIN) == LOW) delay(10);

  // Wait for button press
  while (digitalRead(BTN_PIN) == HIGH) delay(10);

  delay(80); // debounce — confirm it's a real press
  if (digitalRead(BTN_PIN) == HIGH) return;

  // --- STEP 1: Record ---
  uint32_t samplesRecorded = recordAudio();
  Serial.printf("Samples recorded: %u (%.2f sec)\n", samplesRecorded, (float)samplesRecorded / SAMPLE_RATE);

  if (samplesRecorded < 800) {
    oledMsg(" Too short!", " Hold button", " while speaking");
    delay(2000);
    oledMsg("    Ready!", " Hold BOOT btn", "   to speak");
    return;
  }

  // --- STEP 2: Build WAV ---
  uint32_t dataBytes = samplesRecorded * sizeof(int16_t);
  buildWavHeader(wavBuffer, dataBytes);
  memcpy(wavBuffer + WAV_HEADER_SIZE, audioBuffer, dataBytes);
  uint32_t wavSize = WAV_HEADER_SIZE + dataBytes;
  Serial.printf("WAV built: %u bytes\n", wavSize);

  // --- STEP 3: Transcribe ---
  String question = transcribeAudio(wavBuffer, wavSize);
  if (question.isEmpty()) {
    oledMsg(" Didn't catch", " that. Try", " again.");
    delay(2500);
    oledMsg("    Ready!", " Hold BOOT btn", "   to speak");
    return;
  }

  // --- STEP 4: Ask Claude ---
  String answer = askClaude(question);

  // --- STEP 5: Display ---
  oledAnswer(question, answer);
  Serial.println("Done!\n");

  // Hold answer on screen for 8 seconds
  delay(8000);
  oledMsg("    Ready!", " Hold BOOT btn", "   to speak");
}
