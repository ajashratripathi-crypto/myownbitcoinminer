
/*
  CYD Bitcoin Lab ULTRA — replacement main.cpp only
  Target: ESP32-2432S028R / ESP32-WROOM-32E / 2.8" ILI9341 CYD

  What changed:
  - Two optimized hashing workers, one on each ESP32 core
  - Reuses SHA-256 contexts instead of rebuilding them for every hash
  - Wi-Fi setup remains the first screen until connection succeeds
  - Much denser NerdMiner-style dashboard
  - Pages: Miner Dashboard, Big Hashrate, Bitcoin Market, System
  - BOOT button changes pages; hold BOOT for 3 seconds to reopen Wi-Fi setup
  - Real local double-SHA256 hashing, real measured hashrate
  - Real BTC/USD price, block height, and recommended fees
  - No pool connection, no share submission, no transactions

  Existing libraries required:
    TFT_eSPI
    ArduinoJson
    WiFiManager
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include "mbedtls/sha256.h"

// -----------------------------------------------------------------------------
// CYD hardware
// -----------------------------------------------------------------------------

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();

// -----------------------------------------------------------------------------
// Theme
// -----------------------------------------------------------------------------

static constexpr uint16_t C_BG       = 0x0000;
static constexpr uint16_t C_PANEL    = 0x10A2;
static constexpr uint16_t C_PANEL2   = 0x18E3;
static constexpr uint16_t C_BORDER   = 0x3186;
static constexpr uint16_t C_TEXT     = 0xFFFF;
static constexpr uint16_t C_MUTED    = 0x8C71;
static constexpr uint16_t C_ORANGE   = 0xFD20;
static constexpr uint16_t C_ORANGE2  = 0xFB80;
static constexpr uint16_t C_CYAN     = 0x05FF;
static constexpr uint16_t C_GREEN    = 0x07E0;
static constexpr uint16_t C_RED      = 0xF800;
static constexpr uint16_t C_YELLOW   = 0xFFE0;
static constexpr uint16_t C_PURPLE   = 0xA81F;

// -----------------------------------------------------------------------------
// Firmware settings
// -----------------------------------------------------------------------------

static const char* FW_NAME = "CYD BTC ULTRA";
static const char* AP_NAME = "CYD-Miner-Setup";
static const char* AP_PASSWORD = "bitcoin123";

static constexpr uint32_t UI_INTERVAL_MS = 2000;
static constexpr uint32_t MARKET_INTERVAL_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_INTERVAL_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 250;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 3000;
static constexpr uint64_t TEMPLATE_HASH_LIMIT = 10000000ULL;

// -----------------------------------------------------------------------------
// Application state
// -----------------------------------------------------------------------------

enum class Page : uint8_t {
  DASHBOARD = 0,
  HASHRATE = 1,
  MARKET = 2,
  SYSTEM = 3
};

Page page = Page::DASHBOARD;

uint32_t bootMs = 0;
uint32_t lastUiMs = 0;
uint32_t lastMarketMs = 0;
uint32_t lastNetworkMs = 0;
uint32_t lastButtonMs = 0;
uint32_t buttonDownMs = 0;
bool buttonWasDown = false;
bool screenDirty = true;

String minerName = "ORANGE NODE";

// -----------------------------------------------------------------------------
// Mining engine
// -----------------------------------------------------------------------------

struct MiningCounters {
  uint64_t totalHashes;
  uint32_t windowHashes;
  uint32_t currentNonce;
  uint32_t templates;
  uint32_t shares32;
  uint32_t validBlocks;
  uint8_t bestLeadingBits;
  double bestDifficulty;
};

MiningCounters counters = {};
portMUX_TYPE countersMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool miningEnabled = false;
volatile bool pauseMining = false;

struct MiningDisplay {
  uint64_t totalHashes = 0;
  uint32_t currentNonce = 0;
  uint32_t templates = 0;
  uint32_t shares32 = 0;
  uint32_t validBlocks = 0;
  uint8_t bestLeadingBits = 0;
  double bestDifficulty = 0;
  float currentKHs = 0;
  float smoothKHs = 0;
};

MiningDisplay mining;

// -----------------------------------------------------------------------------
// Market and network
// -----------------------------------------------------------------------------

struct MarketState {
  bool valid = false;
  float price = 0;
  float change24h = 0;
  float high24h = 0;
  float low24h = 0;
  String error;
};

struct NetworkState {
  bool heightValid = false;
  bool feesValid = false;
  uint32_t height = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;
  String error;
};

MarketState market;
NetworkState network;

// -----------------------------------------------------------------------------
// Formatting
// -----------------------------------------------------------------------------

String compactNumber(double value) {
  char buffer[24];

  if (value >= 1.0e12) {
    snprintf(buffer, sizeof(buffer), "%.2fT", value / 1.0e12);
  } else if (value >= 1.0e9) {
    snprintf(buffer, sizeof(buffer), "%.2fB", value / 1.0e9);
  } else if (value >= 1.0e6) {
    snprintf(buffer, sizeof(buffer), "%.2fM", value / 1.0e6);
  } else if (value >= 1.0e3) {
    snprintf(buffer, sizeof(buffer), "%.2fK", value / 1.0e3);
  } else {
    snprintf(buffer, sizeof(buffer), "%.0f", value);
  }

  return String(buffer);
}

String difficultyText(double value) {
  char buffer[24];

  if (value >= 1000000.0) {
    snprintf(buffer, sizeof(buffer), "%.3fM", value / 1000000.0);
  } else if (value >= 1000.0) {
    snprintf(buffer, sizeof(buffer), "%.3fK", value / 1000.0);
  } else if (value >= 1.0) {
    snprintf(buffer, sizeof(buffer), "%.4f", value);
  } else {
    snprintf(buffer, sizeof(buffer), "%.6f", value);
  }

  return String(buffer);
}

String uptimeText() {
  uint32_t seconds = (millis() - bootMs) / 1000UL;
  uint32_t days = seconds / 86400UL;
  uint32_t hours = (seconds % 86400UL) / 3600UL;
  uint32_t minutes = (seconds % 3600UL) / 60UL;
  uint32_t secs = seconds % 60UL;

  char buffer[24];
  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%lud %02lu:%02lu:%02lu",
             (unsigned long)days,
             (unsigned long)hours,
             (unsigned long)minutes,
             (unsigned long)secs);
  } else {
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu",
             (unsigned long)hours,
             (unsigned long)minutes,
             (unsigned long)secs);
  }

  return String(buffer);
}

// -----------------------------------------------------------------------------
// UI primitives
// -----------------------------------------------------------------------------

void panel(int x, int y, int w, int h, uint16_t fill = C_PANEL) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, C_BORDER);
}

void textLeft(const String& value, int x, int y,
              uint16_t fg, uint16_t bg, uint8_t font) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(value, x, y, font);
}

void textCenter(const String& value, int x, int y,
                uint16_t fg, uint16_t bg, uint8_t font) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(value, x, y, font);
  tft.setTextDatum(TL_DATUM);
}

void header(const String& title, const String& subtitle) {
  tft.fillRect(0, 0, 320, 33, C_PANEL);
  tft.drawFastHLine(0, 32, 320, C_BORDER);

  tft.fillCircle(17, 16, 11, C_ORANGE);
  textCenter("B", 17, 16, C_TEXT, C_ORANGE, 2);

  textLeft(title, 35, 2, C_TEXT, C_PANEL, 2);
  textLeft(subtitle, 35, 19, C_MUTED, C_PANEL, 1);

  bool online = WiFi.status() == WL_CONNECTED;
  tft.fillCircle(301, 10, 4, online ? C_GREEN : C_RED);
  textCenter(online ? "ONLINE" : "OFFLINE",
             283, 23, online ? C_GREEN : C_RED, C_PANEL, 1);
}

void footer() {
  static const char* names[] = {"MINER", "SPEED", "BTC", "SYS"};

  tft.fillRect(0, 216, 320, 24, C_PANEL);
  tft.drawFastHLine(0, 216, 320, C_BORDER);

  for (int i = 0; i < 4; i++) {
    bool active = static_cast<int>(page) == i;
    uint16_t color = active ? C_ORANGE : C_MUTED;

    if (active) {
      tft.fillRoundRect(i * 80 + 8, 219, 64, 18, 6, C_PANEL2);
    }

    textCenter(names[i], i * 80 + 40, 228,
               color, active ? C_PANEL2 : C_PANEL, 1);
  }
}

void statBox(int x, int y, int w, int h,
             const String& label, const String& value,
             uint16_t color) {
  panel(x, y, w, h);
  textCenter(label, x + w / 2, y + 10, C_MUTED, C_PANEL, 1);
  textCenter(value, x + w / 2, y + 29, color, C_PANEL, 2);
}

// -----------------------------------------------------------------------------
// Wi-Fi setup
// -----------------------------------------------------------------------------

void drawWifiSetupScreen() {
  tft.fillScreen(C_BG);
  header("WI-FI SETUP", "FIRST BOOT CONFIGURATION");

  panel(12, 45, 296, 54);
  textCenter("Connect to this temporary network:", 160, 58,
             C_MUTED, C_PANEL, 1);
  textCenter(AP_NAME, 160, 78, C_ORANGE, C_PANEL, 2);
  textCenter(String("Password: ") + AP_PASSWORD, 160, 93,
             C_TEXT, C_PANEL, 1);

  panel(12, 108, 296, 62);
  textCenter("Choose your home Wi-Fi and enter its password.", 160, 124,
             C_TEXT, C_PANEL, 1);
  textCenter("The setup page scans nearby networks.", 160, 143,
             C_CYAN, C_PANEL, 1);
  textCenter("Open 192.168.4.1 if it does not appear.", 160, 159,
             C_MUTED, C_PANEL, 1);

  panel(12, 179, 296, 27);
  textCenter("Dashboard stays locked until Wi-Fi connects.", 160, 192,
             C_YELLOW, C_PANEL, 1);
}

bool setupWifi() {
  drawWifiSetupScreen();

  WiFiManager manager;
  manager.setTitle("CYD Bitcoin Miner Setup");
  manager.setClass("invert");
  manager.setConfigPortalTimeout(240);
  manager.setConnectTimeout(30);

  bool connected = manager.autoConnect(AP_NAME, AP_PASSWORD);

  if (!connected) {
    tft.fillScreen(C_BG);
    textCenter("WI-FI SETUP FAILED", 160, 95, C_RED, C_BG, 4);
    textCenter("Restarting setup...", 160, 132, C_MUTED, C_BG, 2);
    delay(3000);
    ESP.restart();
  }

  tft.fillScreen(C_BG);
  textCenter("WI-FI CONNECTED", 160, 94, C_GREEN, C_BG, 4);
  textCenter(WiFi.SSID(), 160, 129, C_TEXT, C_BG, 2);
  textCenter(WiFi.localIP().toString(), 160, 155, C_CYAN, C_BG, 2);
  delay(1400);

  return true;
}

void reopenWifiSetup() {
  miningEnabled = false;

  tft.fillScreen(C_BG);
  textCenter("RESETTING WI-FI", 160, 105, C_ORANGE, C_BG, 4);
  textCenter("The setup portal will reopen.", 160, 140, C_MUTED, C_BG, 2);

  WiFiManager manager;
  manager.resetSettings();

  delay(1800);
  ESP.restart();
}

// -----------------------------------------------------------------------------
// HTTP data
// -----------------------------------------------------------------------------

bool httpsGet(const String& url, String& response) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("CYD-BTC-Ultra/1.0");

  if (!http.begin(client, url)) {
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();
  return true;
}

void fetchBitcoinPrice() {
  pauseMining = true;

  String response;
  const String url =
      "https://api.coingecko.com/api/v3/simple/price"
      "?ids=bitcoin&vs_currencies=usd&include_24hr_change=true";

  if (httpsGet(url, response)) {
    DynamicJsonDocument document(2048);
    if (!deserializeJson(document, response)) {
      market.price = document["bitcoin"]["usd"] | 0.0f;
      market.change24h =
          document["bitcoin"]["usd_24h_change"] | 0.0f;
      market.valid = market.price > 0;
      market.error = "";
    } else {
      market.error = "Price JSON error";
    }
  } else {
    market.error = "Price request failed";
  }

  lastMarketMs = millis();
  pauseMining = false;
}

void fetchNetworkData() {
  pauseMining = true;

  String response;

  if (httpsGet("https://mempool.space/api/blocks/tip/height", response)) {
    response.trim();
    network.height = response.toInt();
    network.heightValid = network.height > 0;
  }

  response = "";

  if (httpsGet("https://mempool.space/api/v1/fees/recommended", response)) {
    DynamicJsonDocument document(2048);
    if (!deserializeJson(document, response)) {
      network.fastestFee = document["fastestFee"] | 0;
      network.halfHourFee = document["halfHourFee"] | 0;
      network.hourFee = document["hourFee"] | 0;
      network.feesValid = true;
    }
  }

  lastNetworkMs = millis();
  pauseMining = false;
}

// -----------------------------------------------------------------------------
// Optimized mining
// -----------------------------------------------------------------------------

uint8_t leadingZeroBits(const uint8_t hash[32]) {
  uint8_t bits = 0;

  for (int i = 0; i < 32; i++) {
    if (hash[i] == 0) {
      bits += 8;
      continue;
    }

    uint8_t value = hash[i];
    while ((value & 0x80) == 0) {
      bits++;
      value <<= 1;
    }
    break;
  }

  return bits;
}

double approximateDifficulty(const uint8_t hash[32]) {
  uint64_t top = 0;

  for (int i = 0; i < 8; i++) {
    top = (top << 8) | hash[i];
  }

  if (top == 0) {
    return 4294967296.0;
  }

  static const double difficultyOneTop =
      static_cast<double>(0x00000000FFFF0000ULL);

  return difficultyOneTop / static_cast<double>(top);
}

void refreshTemplate(uint8_t header[80], uint8_t workerId) {
  for (int i = 0; i < 80; i++) {
    header[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  }

  header[0] ^= workerId;
}

void miningWorker(void* argument) {
  uint8_t workerId = static_cast<uint8_t>(
      reinterpret_cast<uintptr_t>(argument)
  );

  uint8_t header[80];
  uint8_t hash1[32];
  uint8_t hash2[32];

  refreshTemplate(header, workerId);

  uint32_t nonce = workerId * 0x70000000UL;
  uint64_t localTemplateHashes = 0;

  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);

  for (;;) {
    if (!miningEnabled || pauseMining) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t batchHashes = 0;
    uint32_t batchShares32 = 0;
    uint8_t batchBestBits = 0;
    double batchBestDifficulty = 0;

    for (int batch = 0; batch < 1024; batch++) {
      header[76] = nonce & 0xFF;
      header[77] = (nonce >> 8) & 0xFF;
      header[78] = (nonce >> 16) & 0xFF;
      header[79] = (nonce >> 24) & 0xFF;

      mbedtls_sha256_starts_ret(&context, 0);
      mbedtls_sha256_update_ret(&context, header, 80);
      mbedtls_sha256_finish_ret(&context, hash1);

      mbedtls_sha256_starts_ret(&context, 0);
      mbedtls_sha256_update_ret(&context, hash1, 32);
      mbedtls_sha256_finish_ret(&context, hash2);

      uint8_t bits = leadingZeroBits(hash2);
      if (bits > batchBestBits) {
        batchBestBits = bits;
      }

      double difficulty = approximateDifficulty(hash2);
      if (difficulty > batchBestDifficulty) {
        batchBestDifficulty = difficulty;
      }

      if (hash2[0] == 0 &&
          hash2[1] == 0 &&
          hash2[2] == 0 &&
          hash2[3] == 0) {
        batchShares32++;
      }

      nonce++;
      batchHashes++;
      localTemplateHashes++;

      if (localTemplateHashes >= TEMPLATE_HASH_LIMIT) {
        refreshTemplate(header, workerId);
        localTemplateHashes = 0;

        portENTER_CRITICAL(&countersMux);
        counters.templates++;
        portEXIT_CRITICAL(&countersMux);
      }
    }

    portENTER_CRITICAL(&countersMux);
    counters.totalHashes += batchHashes;
    counters.windowHashes += batchHashes;
    counters.currentNonce = nonce;
    counters.shares32 += batchShares32;

    if (batchBestBits > counters.bestLeadingBits) {
      counters.bestLeadingBits = batchBestBits;
    }

    if (batchBestDifficulty > counters.bestDifficulty) {
      counters.bestDifficulty = batchBestDifficulty;
    }

    portEXIT_CRITICAL(&countersMux);

    taskYIELD();
  }
}

void startMiningWorkers() {
  counters.templates = 2;

  xTaskCreatePinnedToCore(
      miningWorker,
      "hash-worker-0",
      6144,
      reinterpret_cast<void*>(0),
      1,
      nullptr,
      0
  );

  xTaskCreatePinnedToCore(
      miningWorker,
      "hash-worker-1",
      6144,
      reinterpret_cast<void*>(1),
      1,
      nullptr,
      1
  );

  miningEnabled = true;
}

void sampleMiningStats() {
  uint32_t hashesInWindow = 0;

  portENTER_CRITICAL(&countersMux);

  mining.totalHashes = counters.totalHashes;
  mining.currentNonce = counters.currentNonce;
  mining.templates = counters.templates;
  mining.shares32 = counters.shares32;
  mining.validBlocks = counters.validBlocks;
  mining.bestLeadingBits = counters.bestLeadingBits;
  mining.bestDifficulty = counters.bestDifficulty;

  hashesInWindow = counters.windowHashes;
  counters.windowHashes = 0;

  portEXIT_CRITICAL(&countersMux);

  float seconds = UI_INTERVAL_MS / 1000.0f;
  mining.currentKHs = hashesInWindow / seconds / 1000.0f;

  if (mining.smoothKHs < 0.01f) {
    mining.smoothKHs = mining.currentKHs;
  } else {
    mining.smoothKHs =
        mining.smoothKHs * 0.70f +
        mining.currentKHs * 0.30f;
  }
}

// -----------------------------------------------------------------------------
// Pages
// -----------------------------------------------------------------------------

void drawDashboard() {
  tft.fillScreen(C_BG);
  header("NERD-STYLE MINER", minerName);

  // Large speed card
  panel(8, 39, 151, 76, C_ORANGE);
  textLeft(String(mining.smoothKHs, 2), 16, 52,
           C_BG, C_ORANGE, 4);
  textLeft("kH/s", 111, 82, C_BG, C_ORANGE, 2);
  textLeft("REAL LOCAL HASHRATE", 16, 100,
           C_BG, C_ORANGE, 1);

  // Upper-right statistics
  panel(166, 39, 146, 76);
  textLeft("BLOCK TEMPLATES", 176, 47, C_MUTED, C_PANEL, 1);
  textLeft(String(mining.templates), 281, 46, C_CYAN, C_PANEL, 2);

  textLeft("BEST DIFFICULTY", 176, 65, C_MUTED, C_PANEL, 1);
  textLeft(difficultyText(mining.bestDifficulty),
           251, 64, C_YELLOW, C_PANEL, 2);

  textLeft("32-BIT SHARES", 176, 83, C_MUTED, C_PANEL, 1);
  textLeft(String(mining.shares32), 290, 82, C_GREEN, C_PANEL, 2);

  textLeft("VALID BLOCKS", 176, 101, C_MUTED, C_PANEL, 1);
  textLeft(String(mining.validBlocks), 290, 100, C_RED, C_PANEL, 2);

  // Uptime / total
  panel(8, 122, 304, 35);
  textLeft("UPTIME", 17, 131, C_MUTED, C_PANEL, 1);
  textLeft(uptimeText(), 65, 128, C_TEXT, C_PANEL, 2);

  textLeft("TOTAL", 204, 131, C_MUTED, C_PANEL, 1);
  textLeft(compactNumber(mining.totalHashes),
           243, 128, C_ORANGE, C_PANEL, 2);

  // Bottom three boxes
  statBox(8, 164, 97, 45,
          "BEST ZERO BITS",
          String(mining.bestLeadingBits),
          C_YELLOW);

  statBox(111, 164, 97, 45,
          "WORKERS",
          "2",
          C_GREEN);

  statBox(214, 164, 98, 45,
          "BLOCK HEIGHT",
          network.heightValid ? String(network.height) : "--",
          C_PURPLE);

  footer();
}

void drawHashratePage() {
  tft.fillScreen(C_BG);
  header("HASHRATE", "DUAL-CORE ENGINE");

  textCenter(String(mining.smoothKHs, 2), 160, 90,
             C_ORANGE, C_BG, 6);
  textCenter("kH/s", 160, 130, C_TEXT, C_BG, 4);

  panel(18, 154, 284, 49);
  textLeft("Current", 31, 164, C_MUTED, C_PANEL, 1);
  textLeft(String(mining.currentKHs, 2) + " kH/s",
           31, 179, C_CYAN, C_PANEL, 2);

  textLeft("Nonce", 185, 164, C_MUTED, C_PANEL, 1);
  char nonceText[16];
  snprintf(nonceText, sizeof(nonceText), "%08lX",
           (unsigned long)mining.currentNonce);
  textLeft(String(nonceText), 185, 179, C_TEXT, C_PANEL, 2);

  footer();
}

void drawMarketPage() {
  tft.fillScreen(C_BG);
  header("BITCOIN MARKET", "LIVE COINGECKO DATA");

  panel(10, 43, 300, 70);

  textLeft("BTC / USD", 21, 51, C_MUTED, C_PANEL, 1);

  String priceText =
      market.valid ? "$" + String(market.price, 0) : "Loading...";

  textLeft(priceText, 21, 67, C_ORANGE, C_PANEL, 4);

  if (market.valid) {
    uint16_t color =
        market.change24h >= 0 ? C_GREEN : C_RED;

    String change =
        String(market.change24h >= 0 ? "+" : "") +
        String(market.change24h, 2) + "%";

    textCenter(change, 266, 78, color, C_PANEL, 2);
  }

  panel(10, 122, 300, 84);

  textCenter("NETWORK SNAPSHOT", 160, 136,
             C_MUTED, C_PANEL, 1);

  textCenter(
      network.heightValid ? String(network.height) : "--",
      65, 164, C_PURPLE, C_PANEL, 2
  );
  textCenter("BLOCK", 65, 190, C_MUTED, C_PANEL, 1);

  textCenter(
      network.feesValid
          ? String(network.fastestFee) + " sat/vB"
          : "--",
      160, 164, C_ORANGE, C_PANEL, 2
  );
  textCenter("FAST FEE", 160, 190, C_MUTED, C_PANEL, 1);

  textCenter(
      WiFi.status() == WL_CONNECTED
          ? String(WiFi.RSSI()) + " dBm"
          : "--",
      255, 164, C_GREEN, C_PANEL, 2
  );
  textCenter("WI-FI", 255, 190, C_MUTED, C_PANEL, 1);

  footer();
}

void drawSystemPage() {
  tft.fillScreen(C_BG);
  header("SYSTEM", "ESP32-2432S028R");

  statBox(10, 43, 145, 50,
          "CPU",
          String(ESP.getCpuFreqMHz()) + " MHz",
          C_ORANGE);

  statBox(165, 43, 145, 50,
          "FREE HEAP",
          compactNumber(ESP.getFreeHeap()),
          C_CYAN);

  statBox(10, 101, 145, 50,
          "FLASH",
          String(ESP.getFlashChipSize() / 1024 / 1024) + " MB",
          C_PURPLE);

  statBox(165, 101, 145, 50,
          "CORES",
          String(ESP.getChipCores()),
          C_GREEN);

  panel(10, 159, 300, 48);
  textLeft("Wi-Fi", 21, 168, C_MUTED, C_PANEL, 1);
  textLeft(WiFi.SSID(), 21, 184, C_TEXT, C_PANEL, 2);

  textLeft("Hold BOOT", 218, 168, C_MUTED, C_PANEL, 1);
  textLeft("Wi-Fi reset", 218, 184, C_ORANGE, C_PANEL, 1);

  footer();
}

void drawPage() {
  switch (page) {
    case Page::DASHBOARD:
      drawDashboard();
      break;

    case Page::HASHRATE:
      drawHashratePage();
      break;

    case Page::MARKET:
      drawMarketPage();
      break;

    case Page::SYSTEM:
      drawSystemPage();
      break;
  }

  screenDirty = false;
}

// -----------------------------------------------------------------------------
// Controls and runtime
// -----------------------------------------------------------------------------

void nextPage() {
  int next = (static_cast<int>(page) + 1) % 4;
  page = static_cast<Page>(next);
  screenDirty = true;
}

void handleButton() {
  bool down = digitalRead(PIN_BOOT) == LOW;

  if (down && !buttonWasDown) {
    buttonWasDown = true;
    buttonDownMs = millis();
  }

  if (!down && buttonWasDown) {
    uint32_t duration = millis() - buttonDownMs;
    buttonWasDown = false;

    if (millis() - lastButtonMs < BUTTON_DEBOUNCE_MS) {
      return;
    }

    lastButtonMs = millis();

    if (duration >= BUTTON_LONG_PRESS_MS) {
      reopenWifiSetup();
    } else {
      nextPage();
    }
  }
}

void maintainData() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastMarketMs >= MARKET_INTERVAL_MS) {
    fetchBitcoinPrice();
    screenDirty = true;
  }

  if (millis() - lastNetworkMs >= NETWORK_INTERVAL_MS) {
    fetchNetworkData();
    screenDirty = true;
  }
}

// -----------------------------------------------------------------------------
// Arduino setup and loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  bootMs = millis();

  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);

  pinMode(PIN_BOOT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  setupWifi();

  startMiningWorkers();

  drawDashboard();

  fetchNetworkData();
  fetchBitcoinPrice();

  screenDirty = true;
}

void loop() {
  handleButton();
  maintainData();

  if (millis() - lastUiMs >= UI_INTERVAL_MS) {
    lastUiMs = millis();
    sampleMiningStats();

    if (page == Page::DASHBOARD ||
        page == Page::HASHRATE ||
        page == Page::SYSTEM) {
      screenDirty = true;
    }
  }

  if (screenDirty) {
    drawPage();
  }

  delay(2);
}
