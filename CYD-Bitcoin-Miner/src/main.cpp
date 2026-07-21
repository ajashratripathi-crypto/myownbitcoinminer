/*
  CYD Bitcoin Miner Dashboard V2
  Board: ESP32-2432S028R / ESP32-WROOM-32E
  Display: 2.8" ILI9341 320x240
  Single-file Arduino sketch

  What this does:
  - Real local double-SHA256 hashing benchmark
  - Real measured hashrate and total hashes
  - Real BTC/USD price from CoinGecko
  - Multi-page polished interface
  - Animated mining screen
  - Candlestick-style BTC chart
  - System diagnostics page
  - No pool, wallet, transactions, or share submission

  Required libraries:
  - TFT_eSPI
  - ArduinoJson

  TFT_eSPI configuration:
    #define ILI9341_DRIVER
    #define TFT_MISO 12
    #define TFT_MOSI 13
    #define TFT_SCLK 14
    #define TFT_CS   15
    #define TFT_DC   2
    #define TFT_RST  -1
    #define SPI_FREQUENCY 55000000
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include "mbedtls/sha256.h"

// ---------------- USER SETTINGS ----------------

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MINER_NAME = "ORANGE NODE";
const uint32_t PRICE_UPDATE_MS = 86400000UL;
const uint32_t PAGE_ROTATE_MS  = 12000UL;

// ---------------- DISPLAY ----------------

TFT_eSPI tft = TFT_eSPI();

static const uint16_t C_BG        = TFT_BLACK;
static const uint16_t C_PANEL     = 0x10A2;
static const uint16_t C_PANEL_2   = 0x18E3;
static const uint16_t C_BORDER    = 0x2945;
static const uint16_t C_TEXT      = TFT_WHITE;
static const uint16_t C_MUTED     = 0x8C71;
static const uint16_t C_ORANGE    = 0xFD20;
static const uint16_t C_ORANGE_2  = 0xFB80;
static const uint16_t C_GREEN     = 0x07E0;
static const uint16_t C_RED       = 0xF800;
static const uint16_t C_BLUE      = 0x05FF;
static const uint16_t C_YELLOW    = 0xFFE0;

enum Page {
  PAGE_MINER,
  PAGE_MARKET,
  PAGE_SYSTEM
};

Page currentPage = PAGE_MINER;

// ---------------- MINING STATE ----------------

uint8_t workHeader[80];
uint32_t nonceValue = 0;

uint64_t totalHashes = 0;
uint32_t hashesThisSecond = 0;
float hashRateKHs = 0.0f;
float smoothedHashRate = 0.0f;
float bestDifficulty = 0.0f;

uint32_t acceptedShares = 0;
uint32_t rejectedShares = 0;

unsigned long bootMs = 0;
unsigned long lastStatsUpdate = 0;
unsigned long lastPriceUpdate = 0;
unsigned long lastPageChange = 0;
unsigned long lastAnimation = 0;

// ---------------- MARKET STATE ----------------

double btcPrice = 0.0;
double btcChange24h = 0.0;
double btcHigh24h = 0.0;
double btcLow24h = 0.0;

float candlesOpen[12];
float candlesClose[12];
float candlesHigh[12];
float candlesLow[12];

// ---------------- UI HELPERS ----------------

void fillRoundedPanel(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.drawRoundRect(x, y, w, h, 8, border);
}

void drawTinyLabel(int x, int y, const char* text) {
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(x, y);
  tft.print(text);
}

void drawHeader(const char* pageTitle, bool connected) {
  tft.fillRect(0, 0, 320, 34, C_PANEL);

  tft.fillCircle(17, 17, 11, C_ORANGE);
  tft.setTextColor(TFT_WHITE, C_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(11, 9);
  tft.print("B");

  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextSize(2);
  tft.setCursor(36, 6);
  tft.print(pageTitle);

  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setCursor(37, 23);
  tft.print(MINER_NAME);

  uint16_t statusColor = connected ? C_GREEN : C_RED;
  tft.fillCircle(300, 10, 4, statusColor);
  tft.setTextColor(statusColor, C_PANEL);
  tft.setCursor(271, 20);
  tft.print(connected ? "ONLINE" : "OFFLINE");
}

void drawFooterDots() {
  int y = 232;
  for (int i = 0; i < 3; i++) {
    int x = 145 + i * 15;
    bool active = ((int)currentPage == i);
    tft.fillCircle(x, y, active ? 4 : 2, active ? C_ORANGE : C_MUTED);
  }
}

void drawProgressBar(int x, int y, int w, int h, float value, float maxValue, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, h / 2, C_PANEL_2);
  float ratio = maxValue > 0 ? value / maxValue : 0;
  if (ratio < 0) ratio = 0;
  if (ratio > 1) ratio = 1;
  int filled = (int)(ratio * w);
  if (filled > 0) {
    tft.fillRoundRect(x, y, filled, h, h / 2, color);
  }
}

void drawStatTile(int x, int y, int w, int h, const char* label, const String& value, uint16_t valueColor) {
  fillRoundedPanel(x, y, w, h, C_PANEL, C_BORDER);

  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(x + 9, y + 8);
  tft.print(label);

  tft.setTextColor(valueColor, C_PANEL);
  tft.setTextSize(2);
  tft.setCursor(x + 9, y + 24);
  tft.print(value);
}

void formatLargeNumber(uint64_t value, char* out, size_t len) {
  if (value >= 1000000000ULL) {
    snprintf(out, len, "%.2fB", value / 1000000000.0);
  } else if (value >= 1000000ULL) {
    snprintf(out, len, "%.2fM", value / 1000000.0);
  } else if (value >= 1000ULL) {
    snprintf(out, len, "%.2fK", value / 1000.0);
  } else {
    snprintf(out, len, "%llu", (unsigned long long)value);
  }
}

String uptimeText() {
  unsigned long sec = (millis() - bootMs) / 1000;
  unsigned int hh = sec / 3600;
  unsigned int mm = (sec % 3600) / 60;
  unsigned int ss = sec % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
  return String(buf);
}

// ---------------- MINER PAGE ----------------

void drawHashRing(int cx, int cy, int radius, float value) {
  tft.drawCircle(cx, cy, radius, C_BORDER);
  tft.drawCircle(cx, cy, radius - 1, C_BORDER);
  tft.drawCircle(cx, cy, radius - 2, C_PANEL_2);

  float normalized = value / 100.0f;
  if (normalized > 1.0f) normalized = 1.0f;
  if (normalized < 0.0f) normalized = 0.0f;

  int segments = 40;
  int active = (int)(segments * normalized);

  for (int i = 0; i < segments; i++) {
    float angle = (-135.0f + (270.0f * i / (segments - 1))) * DEG_TO_RAD;
    int x1 = cx + cos(angle) * (radius - 5);
    int y1 = cy + sin(angle) * (radius - 5);
    int x2 = cx + cos(angle) * radius;
    int y2 = cy + sin(angle) * radius;
    tft.drawLine(x1, y1, x2, y2, i < active ? C_ORANGE : C_BORDER);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.setTextSize(4);
  tft.drawString(String(smoothedHashRate, 1), cx, cy - 3);

  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextSize(1);
  tft.drawString("KH/s", cx, cy + 25);
  tft.setTextDatum(TL_DATUM);
}

void drawMinerPage() {
  tft.fillScreen(C_BG);
  drawHeader("MINING", WiFi.status() == WL_CONNECTED);

  drawHashRing(78, 101, 56, smoothedHashRate);

  fillRoundedPanel(145, 43, 165, 116, C_PANEL, C_BORDER);

  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(158, 54);
  tft.print("LIVE HASH ENGINE");

  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextSize(2);
  tft.setCursor(158, 72);
  tft.print("DOUBLE SHA-256");

  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(158, 103);
  tft.print("NONCE");

  tft.setTextColor(C_BLUE, C_PANEL);
  tft.setTextSize(2);
  tft.setCursor(158, 118);
  tft.printf("%08X", nonceValue);

  drawProgressBar(158, 145, 138, 7, smoothedHashRate, 100.0f, C_ORANGE);

  char totalBuf[18];
  formatLargeNumber(totalHashes, totalBuf, sizeof(totalBuf));

  drawStatTile(10, 168, 95, 51, "TOTAL HASHES", String(totalBuf), C_TEXT);
  drawStatTile(112, 168, 95, 51, "BEST DIFF", String(bestDifficulty, 5), C_YELLOW);
  drawStatTile(214, 168, 96, 51, "UPTIME", uptimeText(), C_BLUE);

  drawFooterDots();
}

// ---------------- MARKET PAGE ----------------

void generateCandles() {
  float base = btcPrice > 0 ? btcPrice : 100000.0f;
  float value = base * 0.97f;

  for (int i = 0; i < 12; i++) {
    float swing = random(-180, 181) / 10000.0f;
    float open = value;
    float close = open * (1.0f + swing);
    float high = max(open, close) * (1.0f + random(20, 100) / 10000.0f);
    float low = min(open, close) * (1.0f - random(20, 100) / 10000.0f);

    candlesOpen[i] = open;
    candlesClose[i] = close;
    candlesHigh[i] = high;
    candlesLow[i] = low;
    value = close;
  }
}

void drawCandlestickChart(int x, int y, int w, int h) {
  float minP = candlesLow[0];
  float maxP = candlesHigh[0];

  for (int i = 1; i < 12; i++) {
    minP = min(minP, candlesLow[i]);
    maxP = max(maxP, candlesHigh[i]);
  }

  float range = maxP - minP;
  if (range <= 0) range = 1;

  tft.drawRoundRect(x, y, w, h, 8, C_BORDER);

  for (int i = 1; i < 4; i++) {
    int gy = y + i * h / 4;
    tft.drawFastHLine(x + 5, gy, w - 10, C_PANEL_2);
  }

  int candleSpacing = (w - 20) / 12;

  for (int i = 0; i < 12; i++) {
    int cx = x + 10 + i * candleSpacing + candleSpacing / 2;

    int yHigh = y + h - 8 - (int)(((candlesHigh[i] - minP) / range) * (h - 16));
    int yLow  = y + h - 8 - (int)(((candlesLow[i] - minP) / range) * (h - 16));
    int yOpen = y + h - 8 - (int)(((candlesOpen[i] - minP) / range) * (h - 16));
    int yClose = y + h - 8 - (int)(((candlesClose[i] - minP) / range) * (h - 16));

    uint16_t color = candlesClose[i] >= candlesOpen[i] ? C_GREEN : C_RED;

    tft.drawFastVLine(cx, yHigh, max(1, yLow - yHigh), color);

    int top = min(yOpen, yClose);
    int bodyH = max(3, abs(yClose - yOpen));
    tft.fillRect(cx - 3, top, 7, bodyH, color);
  }
}

void drawMarketPage() {
  tft.fillScreen(C_BG);
  drawHeader("MARKET", WiFi.status() == WL_CONNECTED);

  fillRoundedPanel(10, 43, 300, 61, C_PANEL, C_BORDER);

  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(20, 52);
  tft.print("BITCOIN / US DOLLAR");

  tft.setTextColor(C_ORANGE, C_PANEL);
  tft.setTextSize(3);
  tft.setCursor(20, 69);
  tft.printf("$%.0f", btcPrice);

  tft.setTextSize(2);
  tft.setTextColor(btcChange24h >= 0 ? C_GREEN : C_RED, C_PANEL);
  tft.setCursor(205, 73);
  tft.printf("%+.2f%%", btcChange24h);

  drawCandlestickChart(10, 112, 300, 105);
  drawFooterDots();
}

// ---------------- SYSTEM PAGE ----------------

void drawGaugeBar(int x, int y, const char* label, float value, float maxValue, const String& suffix, uint16_t color) {
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextSize(1);
  tft.setCursor(x, y);
  tft.print(label);

  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(x + 200, y);
  tft.print(String(value, 0) + suffix);

  drawProgressBar(x, y + 13, 280, 9, value, maxValue, color);
}

void drawSystemPage() {
  tft.fillScreen(C_BG);
  drawHeader("SYSTEM", WiFi.status() == WL_CONNECTED);

  fillRoundedPanel(10, 44, 300, 44, C_PANEL, C_BORDER);
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(20, 53);
  tft.print("DEVICE");
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextSize(2);
  tft.setCursor(20, 67);
  tft.print(ESP.getChipModel());

  drawGaugeBar(20, 105, "CPU FREQUENCY", ESP.getCpuFreqMHz(), 240.0f, " MHz", C_ORANGE);
  drawGaugeBar(20, 139, "FREE HEAP", ESP.getFreeHeap() / 1024.0f, 320.0f, " KB", C_BLUE);
  drawGaugeBar(20, 173, "WI-FI SIGNAL", max(0, WiFi.RSSI() + 100), 70.0f, " dBm", C_GREEN);

  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextSize(1);
  tft.setCursor(20, 211);
  tft.printf("FLASH %uMB   CORES %u   SDK %s",
             ESP.getFlashChipSize() / 1024 / 1024,
             ESP.getChipCores(),
             ESP.getSdkVersion());

  drawFooterDots();
}

void drawCurrentPage() {
  if (currentPage == PAGE_MINER) drawMinerPage();
  else if (currentPage == PAGE_MARKET) drawMarketPage();
  else drawSystemPage();
}

// ---------------- NETWORK ----------------

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.setTextSize(3);
  tft.drawString("ORANGE NODE", 160, 88);

  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextSize(1);
  tft.drawString("CONNECTING TO WI-FI", 160, 124);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 24) {
    tft.fillCircle(112 + (attempts % 9) * 12, 150, 3, C_ORANGE);
    delay(350);
    attempts++;
  }

  tft.setTextDatum(TL_DATUM);
}

void fetchBitcoinPrice() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true");
  http.setTimeout(8000);

  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    DynamicJsonDocument doc(1536);
    DeserializationError err = deserializeJson(doc, http.getString());

    if (!err) {
      btcPrice = doc["bitcoin"]["usd"] | btcPrice;
      btcChange24h = doc["bitcoin"]["usd_24h_change"] | btcChange24h;
      generateCandles();
    }
  }

  http.end();
  lastPriceUpdate = millis();
}

// ---------------- HASH ENGINE ----------------

void makeWorkHeader() {
  for (int i = 0; i < 80; i++) {
    workHeader[i] = (uint8_t)random(0, 256);
  }
  nonceValue = 0;
}

float estimateDifficulty(const uint8_t hash[32]) {
  uint32_t leadingBits = 0;

  for (int i = 0; i < 32; i++) {
    if (hash[i] == 0) {
      leadingBits += 8;
    } else {
      uint8_t value = hash[i];
      while ((value & 0x80) == 0) {
        leadingBits++;
        value <<= 1;
      }
      break;
    }
  }

  return powf(2.0f, (float)leadingBits - 32.0f);
}

void mineBatch(uint32_t count) {
  uint8_t firstHash[32];
  uint8_t secondHash[32];

  for (uint32_t i = 0; i < count; i++) {
    workHeader[76] = nonceValue & 0xFF;
    workHeader[77] = (nonceValue >> 8) & 0xFF;
    workHeader[78] = (nonceValue >> 16) & 0xFF;
    workHeader[79] = (nonceValue >> 24) & 0xFF;
    nonceValue++;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    mbedtls_sha256_starts_ret(&ctx, 0);
    mbedtls_sha256_update_ret(&ctx, workHeader, 80);
    mbedtls_sha256_finish_ret(&ctx, firstHash);

    mbedtls_sha256_starts_ret(&ctx, 0);
    mbedtls_sha256_update_ret(&ctx, firstHash, 32);
    mbedtls_sha256_finish_ret(&ctx, secondHash);

    mbedtls_sha256_free(&ctx);

    float currentDifficulty = estimateDifficulty(secondHash);
    if (currentDifficulty > bestDifficulty) {
      bestDifficulty = currentDifficulty;
    }

    totalHashes++;
    hashesThisSecond++;
  }
}

// ---------------- SETUP / LOOP ----------------

void setup() {
  Serial.begin(115200);
  bootMs = millis();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  randomSeed(esp_random());
  makeWorkHeader();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    fetchBitcoinPrice();
  } else {
    btcPrice = 0;
    btcChange24h = 0;
    generateCandles();
  }

  drawCurrentPage();
}

void loop() {
  mineBatch(300);

  if (millis() - lastStatsUpdate >= 1000) {
    hashRateKHs = hashesThisSecond / 1000.0f;
    hashesThisSecond = 0;

    smoothedHashRate = smoothedHashRate == 0
      ? hashRateKHs
      : (smoothedHashRate * 0.78f + hashRateKHs * 0.22f);

    lastStatsUpdate = millis();

    if (currentPage == PAGE_MINER) {
      drawMinerPage();
    }
  }

  if (millis() - lastPriceUpdate >= PRICE_UPDATE_MS) {
    fetchBitcoinPrice();

    if (currentPage == PAGE_MARKET) {
      drawMarketPage();
    }
  }

  if (millis() - lastPageChange >= PAGE_ROTATE_MS) {
    currentPage = (Page)(((int)currentPage + 1) % 3);
    lastPageChange = millis();
    drawCurrentPage();
  }
}
