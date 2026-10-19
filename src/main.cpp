#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include <time.h>

#include "secrets_local.h"
#include "whale_bitmap.h"

constexpr int pinMosi = 6;
constexpr int pinSclk = 7;
constexpr int pinCs = 14;
constexpr int pinDc = 15;
constexpr int pinReset = 21;
constexpr int pinBacklight = 22;
constexpr uint16_t background = 0x08A5;
constexpr uint16_t panel = 0x10E8;
constexpr uint32_t balanceIntervalMs = 60000;
constexpr uint32_t statusCheckIntervalMs = 1000;

Arduino_HWSPI displayBus(pinDc, pinCs, pinSclk, pinMosi, -1);
Arduino_ST7789 display(&displayBus, pinReset, 0, true, 240, 240, 0, 0);
String displayAmount = "--";
String displayCurrency = "CNY";
uint32_t lastBalanceFetch = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastStatusCheck = 0;
bool displayReady = false;
bool balanceKnown = false;
bool balanceStale = true;
bool gatewayFailed = false;
String lastShownTime;
int lastShownState = -1;

enum class GatewayState { Offline, Degraded, Ready };

void drawCentered(const String &text, int y, uint8_t scale, uint16_t color) {
  display.setTextSize(scale);
  display.setTextColor(color);
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text.c_str(), 0, y, &x1, &y1, &width, &height);
  display.setCursor((240 - width) / 2 - x1, y);
  display.print(text);
}

void drawBalance() {
  if (!displayReady) return;
  display.fillRoundRect(14, 180, 212, 51, 10, panel);
  display.drawRoundRect(14, 180, 212, 51, 10, 0x5D9F);
  String balance = (balanceStale && balanceKnown ? "~" : "") + displayCurrency + " " + displayAmount;
  drawCentered(balance, 196, balance.length() > 11 ? 2 : 3, 0xFFFF);
}

GatewayState gatewayState() {
  if (WiFi.status() != WL_CONNECTED) return GatewayState::Offline;
  return gatewayFailed || balanceStale ? GatewayState::Degraded : GatewayState::Ready;
}

void drawStatus(const String &timeText, GatewayState state) {
  if (!displayReady) return;
  const int statusX = 166;
  const int statusY = 6;
  display.fillRoundRect(statusX, statusY, 68, 29, 7, panel);
  display.drawRoundRect(statusX, statusY, 68, 29, 7, 0x5D9F);
  uint16_t stateColor = state == GatewayState::Ready ? 0x07E0
                        : state == GatewayState::Degraded ? 0xFEA0
                                                          : 0xF800;
  display.fillCircle(statusX + 10, statusY + 14, 4, stateColor);
  display.setTextSize(2);
  display.setTextColor(0xFFFF);
  display.setCursor(statusX + 20, statusY + 7);
  display.print(timeText);
}

void updateStatus() {
  struct tm localTime;
  char timeText[6] = "--:--";
  if (getLocalTime(&localTime, 0)) {
    strftime(timeText, sizeof(timeText), "%H:%M", &localTime);
  }
  GatewayState state = gatewayState();
  int stateValue = static_cast<int>(state);
  if (String(timeText) == lastShownTime && stateValue == lastShownState) return;
  lastShownTime = timeText;
  lastShownState = stateValue;
  drawStatus(lastShownTime, state);
}

void drawScene() {
  display.fillScreen(background);
  display.draw16bitRGBBitmap((240 - whaleWidth) / 2, 7, whaleBitmap, whaleWidth, whaleHeight);
  drawBalance();
  updateStatus();
}

bool fetchBalance() {
  if (WiFi.status() != WL_CONNECTED) {
    gatewayFailed = true;
    Serial.println("[balance] Wi-Fi offline");
    return false;
  }
  HTTPClient http;
  String url = String("http://") + BALANCE_HOST + ":" + BALANCE_PORT + "/balance";
  http.setConnectTimeout(2000);
  http.setTimeout(2500);
  if (!http.begin(url)) {
    gatewayFailed = true;
    return false;
  }
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[balance] Orange Pi HTTP %d\n", status);
    http.end();
    gatewayFailed = true;
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getString());
  http.end();
  if (error || !doc["balances"].is<JsonArray>()) {
    Serial.println("[balance] invalid response");
    gatewayFailed = true;
    return false;
  }

  String amount;
  String currency;
  for (JsonObject item : doc["balances"].as<JsonArray>()) {
    String code = item["currency"] | "";
    String value = item["total_balance"] | "";
    if (code == "CNY") {
      amount = value;
      currency = "CNY";
      break;
    }
    if (amount.isEmpty() && !value.isEmpty()) {
      amount = value;
      currency = code;
    }
  }
  if (amount.isEmpty()) {
    gatewayFailed = true;
    return false;
  }

  displayAmount = amount;
  displayCurrency = currency;
  balanceKnown = true;
  balanceStale = doc["stale"] | false;
  gatewayFailed = false;
  Serial.printf("[balance] refreshed currency=%s%s\n", currency.c_str(),
                (doc["stale"] | false) ? " (cached)" : "");
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(pinBacklight, OUTPUT);
  digitalWrite(pinBacklight, LOW);
  displayReady = display.begin(40000000);
  if (displayReady) {
    display.setRotation(0);
    display.setTextWrap(false);
    drawScene();
    digitalWrite(pinBacklight, HIGH);
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
  lastWifiAttempt = millis();
  lastBalanceFetch = 0;
  Serial.println("[boot] ESP Fish balance display");
}

void loop() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttempt >= 15000) {
    lastWifiAttempt = now;
    WiFi.reconnect();
  }
  uint32_t retryInterval = (balanceKnown && !gatewayFailed) ? balanceIntervalMs : 10000;
  if (lastBalanceFetch == 0 || now - lastBalanceFetch >= retryInterval) {
    lastBalanceFetch = now;
    if (fetchBalance()) {
      drawBalance();
    } else {
      Serial.println("[balance] unavailable");
    }
    updateStatus();
  }
  if (now - lastStatusCheck >= statusCheckIntervalMs) {
    lastStatusCheck = now;
    updateStatus();
  }
  delay(10);
}
