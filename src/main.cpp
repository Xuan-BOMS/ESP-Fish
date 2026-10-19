#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <miniz.h>
#include <time.h>

#include "secrets_local.h"
#include "whale_bitmap.h"
#include "pet_frames.h"

#ifndef CONTROL_HOST
#define CONTROL_HOST BALANCE_HOST
#endif
#ifndef CONTROL_PORT
#define CONTROL_PORT 8082
#endif

constexpr int screenSize = 240;
constexpr uint16_t background = 0x08A5;
constexpr uint16_t panel = 0x10E8;
constexpr uint32_t balanceIntervalMs = 60000;
constexpr int petDisplaySize = petSize;
constexpr int petTravel = screenSize - petDisplaySize;
constexpr size_t petPixelBytes = petSize * petSize * sizeof(uint16_t);
constexpr size_t petBufferBytes = petPixelBytes + sizeof(tinfl_decompressor);
static_assert(petSize < screenSize, "Pet must leave room for movement");
Arduino_HWSPI displayBus(15, 14, 7, 6, -1);
Arduino_ST7789 display(&displayBus, 21, 0, true, screenSize, screenSize, 0, 0);
Preferences preferences;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

enum class Mode { Balance, Pet };
struct State {
  Mode desired = Mode::Balance;
  Mode active = Mode::Balance;
  bool swapAxes = false;
  bool invertX = false;
  bool invertY = false;
  bool imu = false;
  bool known = false;
  bool stale = true;
  bool gatewayFailed = true;
  char amount[32] = "--";
  char currency[8] = "CNY";
  uint32_t balanceRevision = 0;
  uint32_t heap = 0;
  uint32_t renderBytes = 0;
  uint32_t frames = 0;
  uint32_t calibration = 0;
  float x = petTravel / 2, y = petTravel / 2, ax = 0, ay = 0;
} shared, shown;

State snapshot() {
  portENTER_CRITICAL(&stateLock);
  State result = shared;
  portEXIT_CRITICAL(&stateLock);
  return result;
}

bool displayReady = false;
String lastShownTime;
int lastShownState = -1;
uint16_t *petBuffer = nullptr;
tinfl_decompressor *petInflater = nullptr;
uint8_t imuAddress = 0;
uint8_t imuErrors = 0;
struct Vector { float x, y, z; } gravity{}, horizontal{1, 0, 0}, vertical{0, 1, 0};
float tiltX = 0, tiltY = 0;
bool facingRight = false;
bool petFrameValid = false, decodedMirror = false;
uint8_t decodedClip = 0;
uint16_t decodedFrame = 0;
float petX = petTravel / 2, petY = petTravel / 2;
float wanderX = 0, wanderY = 0;
int oldPetX = -1, oldPetY = -1;
uint8_t clipIndex = 0;
uint16_t frameIndex = 0;
uint32_t lastFrame = 0, lastMotion = 0, nextWander = 0, lastImuRetry = 0;
uint32_t renderedFrames = 0;

float dot(Vector a, Vector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void calibrateTilt() {
  float norm = sqrtf(dot(gravity, gravity));
  if (norm < 0.4f || norm > 1.8f) return;
  Vector normal{gravity.x / norm, gravity.y / norm, gravity.z / norm};
  Vector axis = fabsf(normal.x) < 0.8f ? Vector{1, 0, 0} : Vector{0, 1, 0};
  float projection = dot(axis, normal);
  horizontal = {axis.x - projection * normal.x, axis.y - projection * normal.y, axis.z - projection * normal.z};
  norm = sqrtf(dot(horizontal, horizontal));
  horizontal = {horizontal.x / norm, horizontal.y / norm, horizontal.z / norm};
  vertical = {normal.y * horizontal.z - normal.z * horizontal.y,
              normal.z * horizontal.x - normal.x * horizontal.z,
              normal.x * horizontal.y - normal.y * horizontal.x};
  Serial.println("[imu] neutral pose calibrated");
}

void drawCentered(const String &text, int y, uint8_t scale, uint16_t color) {
  display.setTextSize(scale);
  display.setTextColor(color);
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text.c_str(), 0, y, &x1, &y1, &width, &height);
  display.setCursor((screenSize - width) / 2 - x1, y);
  display.print(text);
}

void drawBalance() {
  if (!displayReady || shown.active != Mode::Balance) return;
  display.fillRoundRect(14, 180, 212, 51, 10, panel);
  display.drawRoundRect(14, 180, 212, 51, 10, 0x5D9F);
  String balance = String(shown.stale && shown.known ? "~" : "") + shown.currency + " " + shown.amount;
  drawCentered(balance, 196, balance.length() > 11 ? 2 : 3, 0xFFFF);
}

void updateStatus() {
  if (!displayReady || shown.active != Mode::Balance) return;
  struct tm localTime;
  char text[6] = "--:--";
  if (getLocalTime(&localTime, 0)) strftime(text, sizeof(text), "%H:%M", &localTime);
  int state = WiFi.status() != WL_CONNECTED ? 0 : shown.gatewayFailed || shown.stale ? 1 : 2;
  if (String(text) == lastShownTime && state == lastShownState) return;
  lastShownTime = text;
  lastShownState = state;
  constexpr int x = 138, y = 6, width = 96;
  static_assert(x + 20 + 5 * 6 * 2 < x + width, "Clock must fit inside status panel");
  display.fillRoundRect(x, y, width, 29, 7, panel);
  display.drawRoundRect(x, y, width, 29, 7, 0x5D9F);
  display.fillCircle(x + 10, y + 14, 4, state == 2 ? 0x07E0 : state == 1 ? 0xFEA0 : 0xF800);
  display.setTextSize(2);
  display.setTextColor(0xFFFF);
  display.setCursor(x + 20, y + 7);
  display.print(text);
}

void drawScene() {
  if (!displayReady) return;
  display.fillScreen(background);
  oldPetX = oldPetY = -1;
  lastShownTime = "";
  lastShownState = -1;
  if (shown.active == Mode::Balance) {
    display.draw16bitRGBBitmap((screenSize - whaleWidth) / 2, 7, whaleBitmap, whaleWidth, whaleHeight);
    drawBalance();
    updateStatus();
  }
}

bool imuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(imuAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool imuRead(uint8_t reg, uint8_t *data, uint8_t length) {
  Wire.beginTransmission(imuAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(imuAddress, length) != length) return false;
  for (uint8_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

bool initImu() {
  for (uint8_t address : {0x69, 0x68}) {
    imuAddress = address;
    uint8_t id = 0;
    if (!imuRead(0x00, &id, 1) || id != 0xD1) continue;
    if (!imuWrite(0x7E, 0xB6)) continue;
    delay(10);
    if (!imuWrite(0x40, 0x28) || !imuWrite(0x41, 0x03) || !imuWrite(0x7E, 0x11)) continue;
    delay(50);
    imuErrors = 0;
    uint8_t raw[6];
    gravity = {};
    for (int i = 0; i < 12; ++i) {
      if (imuRead(0x12, raw, sizeof(raw))) {
        Vector sample{static_cast<int16_t>(raw[0] | raw[1] << 8) / 16384.0f,
                      static_cast<int16_t>(raw[2] | raw[3] << 8) / 16384.0f,
                      static_cast<int16_t>(raw[4] | raw[5] << 8) / 16384.0f};
        gravity = i == 0 ? sample : Vector{(gravity.x + sample.x) / 2, (gravity.y + sample.y) / 2, (gravity.z + sample.z) / 2};
      }
      delay(10);
    }
    calibrateTilt();
    Serial.printf("[imu] BMI160 ready at 0x%02X\n", address);
    return true;
  }
  imuAddress = 0;
  Serial.println("[imu] unavailable; autonomous movement remains enabled");
  return false;
}

void readTilt() {
  uint8_t raw[6];
  if (!imuAddress || !imuRead(0x12, raw, sizeof(raw))) {
    if (imuAddress && ++imuErrors >= 20) imuAddress = 0;
    tiltX *= 0.8f;
    tiltY *= 0.8f;
    return;
  }
  imuErrors = 0;
  float ax = static_cast<int16_t>(raw[0] | raw[1] << 8) / 16384.0f;
  float ay = static_cast<int16_t>(raw[2] | raw[3] << 8) / 16384.0f;
  float az = static_cast<int16_t>(raw[4] | raw[5] << 8) / 16384.0f;
  gravity.x += 0.22f * (ax - gravity.x);
  gravity.y += 0.22f * (ay - gravity.y);
  gravity.z += 0.22f * (az - gravity.z);
  tiltX = dot(gravity, horizontal);
  tiltY = dot(gravity, vertical);
}

void setClip(uint8_t index) {
  clipIndex = index;
  frameIndex = 0;
  lastFrame = 0;
}

bool decodeFrame() {
  const PetClip &clip = petClips[clipIndex];
  uint32_t offset = pgm_read_dword(clip.offsets + frameIndex);
  uint32_t end = pgm_read_dword(clip.offsets + frameIndex + 1);
  constexpr size_t pixels = petSize * petSize;
  uint8_t *indices = reinterpret_cast<uint8_t *>(petBuffer) + pixels;
  size_t inputSize = end - offset, outputSize = pixels;
  tinfl_init(petInflater);
  tinfl_status status = tinfl_decompress(petInflater, clip.data + offset, &inputSize,
                                       indices, indices, &outputSize,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || outputSize != pixels || inputSize != end - offset) return false;
  // Expanding forward preserves unread indices in the upper half of this buffer.
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t color = indices[i];
    if (color >= 128) return false;
    petBuffer[i] = pgm_read_word(clip.palette + color);
  }
  return true;
}

void renderPet(int x, int y) {
  if (!displayReady) return;
  bool mirror = clipIndex == 1 && facingRight;
  if (!petFrameValid || decodedClip != clipIndex || decodedFrame != frameIndex || decodedMirror != mirror) {
    if (!decodeFrame()) { petFrameValid = false; return; }
    if (mirror) {
      for (int row = 0; row < petSize; ++row) {
        for (int col = 0; col < petSize / 2; ++col) {
          uint16_t &left = petBuffer[row * petSize + col];
          uint16_t &right = petBuffer[row * petSize + petSize - 1 - col];
          uint16_t pixel = left;
          left = right;
          right = pixel;
        }
      }
    }
    decodedClip = clipIndex;
    decodedFrame = frameIndex;
    decodedMirror = mirror;
    petFrameValid = true;
  }
  if (oldPetX >= 0) {
    if (x > oldPetX) display.fillRect(oldPetX, oldPetY, x - oldPetX, petDisplaySize, background);
    if (x < oldPetX) display.fillRect(x + petDisplaySize, oldPetY, oldPetX - x, petDisplaySize, background);
    if (y > oldPetY) display.fillRect(oldPetX, oldPetY, petDisplaySize, y - oldPetY, background);
    if (y < oldPetY) display.fillRect(oldPetX, y + petDisplaySize, petDisplaySize, oldPetY - y, background);
  }
  display.draw16bitRGBBitmap(x, y, petBuffer, petSize, petSize);
  ++renderedFrames;
  oldPetX = x;
  oldPetY = y;
}

void updatePet(uint32_t now) {
  if (!petBuffer || now - lastMotion < 33) return;
  float dt = fminf((now - lastMotion) / 1000.0f, 0.1f);
  lastMotion = now;
  if (!imuAddress && now - lastImuRetry >= 5000) {
    lastImuRetry = now;
    initImu();
  }
  readTilt();
  float ax = shown.swapAxes ? tiltY : tiltX;
  float ay = shown.swapAxes ? tiltX : tiltY;
  if (shown.invertX) ax = -ax;
  if (shown.invertY) ay = -ay;
  bool tilted = fabsf(ax) > 0.12f || fabsf(ay) > 0.12f;
  if (static_cast<int32_t>(now - nextWander) >= 0) {
    nextWander = now + random(2000, 5000);
    wanderX = random(-18, 19);
    wanderY = random(-18, 19);
    if (random(3) == 0) {
      wanderX = wanderY = 0;
      if (!tilted) {
        setClip(random(3) == 0 ? 0 : random(2, 4));
        nextWander = now + petClips[clipIndex].frames * petFrameMs;
      }
    }
  }
  float vx = tilted ? (fabsf(ax) > 0.12f ? ax * 85 : 0) : wanderX;
  float vy = tilted ? (fabsf(ay) > 0.12f ? ay * 85 : 0) : wanderY;
  bool moving = fabsf(vx) + fabsf(vy) > 1;
  if (fabsf(vx) > 1) facingRight = vx > 0;
  if (moving && clipIndex != 1) setClip(1);
  if (!moving && clipIndex == 1) setClip(0);
  petX = constrain(petX + vx * dt, 0.0f, float(petTravel));
  petY = constrain(petY + vy * dt, 0.0f, float(petTravel));
  if (petX <= 0 || petX >= petTravel) wanderX = -wanderX;
  if (petY <= 0 || petY >= petTravel) wanderY = -wanderY;
  if (now - lastFrame >= petFrameMs) {
    lastFrame = now;
    renderPet(lroundf(petX), lroundf(petY));
    if (++frameIndex >= petClips[clipIndex].frames) {
      frameIndex = 0;
      if (!moving) setClip(random(3) == 0 ? random(2, 4) : 0);
    }
  } else if (oldPetX != lroundf(petX) || oldPetY != lroundf(petY)) {
    uint16_t nextFrame = frameIndex;
    frameIndex = frameIndex ? frameIndex - 1 : petClips[clipIndex].frames - 1;
    renderPet(lroundf(petX), lroundf(petY));
    frameIndex = nextFrame;
  }
}

void changeMode(Mode desired) {
  petFrameValid = false;
  if (desired == Mode::Pet) {
    petBuffer = static_cast<uint16_t *>(heap_caps_malloc(petBufferBytes, MALLOC_CAP_8BIT));
    if (!petBuffer) {
      Serial.println("[mode] insufficient contiguous memory; keeping balance mode");
      return;
    }
    petInflater = reinterpret_cast<tinfl_decompressor *>(reinterpret_cast<uint8_t *>(petBuffer) + petPixelBytes);
    initImu();
    lastImuRetry = millis();
    petX = petY = petTravel / 2;
    lastMotion = millis();
    nextWander = millis();
    setClip(0);
  } else {
    if (imuAddress) imuWrite(0x7E, 0x10);
    free(petBuffer);
    petBuffer = nullptr;
    petInflater = nullptr;
  }
  shown.active = desired;
  preferences.putBool("pet", desired == Mode::Pet);
  drawScene();
  Serial.printf("[mode] %s render=%u heap=%u\n", desired == Mode::Pet ? "pet" : "balance",
                petBuffer ? unsigned(petBufferBytes) : 0, ESP.getFreeHeap());
}

bool fetchBalance() {
  HTTPClient http;
  String url = String("http://") + BALANCE_HOST + ":" + BALANCE_PORT + "/balance";
  http.setConnectTimeout(1500);
  http.setTimeout(10000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  JsonDocument doc;
  bool valid = code == HTTP_CODE_OK && !deserializeJson(doc, http.getString()) && doc["balances"].is<JsonArray>();
  http.end();
  if (!valid) return false;
  String amount, currency;
  for (JsonObject item : doc["balances"].as<JsonArray>()) {
    String value = item["total_balance"] | "";
    String name = item["currency"] | "";
    if (!value.isEmpty() && (amount.isEmpty() || name == "CNY")) {
      amount = value;
      currency = name;
      if (name == "CNY") break;
    }
  }
  if (amount.isEmpty() || amount.length() >= sizeof(shared.amount) || currency.length() >= sizeof(shared.currency)) return false;
  portENTER_CRITICAL(&stateLock);
  strlcpy(shared.amount, amount.c_str(), sizeof(shared.amount));
  strlcpy(shared.currency, currency.c_str(), sizeof(shared.currency));
  shared.known = true;
  shared.stale = doc["stale"] | false;
  shared.gatewayFailed = false;
  ++shared.balanceRevision;
  portEXIT_CRITICAL(&stateLock);
  return true;
}

void syncControl() {
  State current = snapshot();
  JsonDocument report;
  report["mode"] = current.active == Mode::Pet ? "pet" : "balance";
  report["imu"] = current.imu;
  report["heap"] = current.heap;
  report["render_bytes"] = current.renderBytes;
  report["frames"] = current.frames;
  report["x"] = current.x;
  report["y"] = current.y;
  report["ax"] = current.ax;
  report["ay"] = current.ay;
  report["uptime"] = millis();
  String body;
  serializeJson(report, body);
  HTTPClient http;
  http.setConnectTimeout(750);
  http.setTimeout(1000);
  String url = String("http://") + CONTROL_HOST + ":" + CONTROL_PORT + "/api/device";
  if (!http.begin(url)) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  JsonDocument response;
  if (code == HTTP_CODE_OK && !deserializeJson(response, http.getString())) {
    String mode = response["mode"] | "";
    if (mode == "balance" || mode == "pet") {
      portENTER_CRITICAL(&stateLock);
      shared.desired = mode == "pet" ? Mode::Pet : Mode::Balance;
      shared.swapAxes = response["swap_axes"] | false;
      shared.invertX = response["invert_x"] | false;
      shared.invertY = response["invert_y"] | false;
      shared.calibration = response["calibration"] | 0U;
      portEXIT_CRITICAL(&stateLock);
    }
  }
  http.end();
}

void networkTask(void *) {
  uint32_t lastBalance = 0, lastControl = 0, lastReconnect = millis();
  bool fetched = false;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastReconnect >= 15000) { lastReconnect = now; WiFi.reconnect(); }
    } else {
      if (now - lastControl >= 1000) { lastControl = now; syncControl(); }
      State current = snapshot();
      uint32_t interval = current.known && !current.gatewayFailed ? balanceIntervalMs : 10000;
      if (!fetched || now - lastBalance >= interval) {
        lastBalance = now;
        fetched = true;
        if (!fetchBalance()) {
          portENTER_CRITICAL(&stateLock);
          shared.gatewayFailed = true;
          shared.stale = true;
          ++shared.balanceRevision;
          portEXIT_CRITICAL(&stateLock);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(22, OUTPUT);
  digitalWrite(22, LOW);
  displayReady = display.begin(40000000);
  if (displayReady) {
    display.setTextWrap(false);
    drawScene();
    digitalWrite(22, HIGH);
  }
  Wire.begin(2, 1);
  Wire.setTimeOut(20);
  preferences.begin("espfish", false);
  shared.desired = preferences.getBool("pet", false) ? Mode::Pet : Mode::Balance;
  randomSeed(esp_random());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
  if (xTaskCreate(networkTask, "gateway", 8192, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[network] worker allocation failed");
  }
  Serial.println("[boot] ESP Fish dual-mode display");
}

void loop() {
  static uint32_t lastStatus = 0, lastModeAttempt = 0, lastCalibration = 0;
  State current = snapshot();
  bool balanceChanged = current.balanceRevision != shown.balanceRevision;
  Mode active = shown.active;
  shown = current;
  shown.active = active;
  uint32_t now = millis();
  if (current.desired != active && now - lastModeAttempt >= 1000) {
    lastModeAttempt = now;
    changeMode(current.desired);
  }
  if (shown.active == Mode::Pet) {
    if (current.calibration != lastCalibration) {
      calibrateTilt();
      lastCalibration = current.calibration;
    }
    updatePet(now);
  }
  else if (balanceChanged) drawBalance();
  if (now - lastStatus >= 1000) { lastStatus = now; updateStatus(); }
  uint32_t freeHeap = ESP.getFreeHeap();
  portENTER_CRITICAL(&stateLock);
  shared.active = shown.active;
  shared.imu = imuAddress != 0;
  shared.renderBytes = petBuffer ? petBufferBytes : 0;
  shared.frames = renderedFrames;
  shared.heap = freeHeap;
  shared.x = petX;
  shared.y = petY;
  shared.ax = tiltX;
  shared.ay = tiltY;
  portEXIT_CRITICAL(&stateLock);
  delay(5);
}
