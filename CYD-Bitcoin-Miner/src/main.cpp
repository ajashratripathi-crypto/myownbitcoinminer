
/*
  CYD Bitcoin Lab STABLE — replacement main.cpp only
  Target: ESP32-2432S028R / ESP32-WROOM-32E / 2.8" ILI9341 CYD

  Design goals:
  - No full-screen redraw every second
  - Only changing number regions are refreshed
  - Automatically changes to the next page every 10 seconds
  - Uses factual values only
  - Real local double-SHA256 benchmark
  - Real BTC/USD price from CoinGecko
  - Real Bitcoin block height and recommended fees from mempool.space
  - No fake shares, fake blocks, fake templates, or fake earnings
  - No pool connection and no transaction submission

  Controls:
  - Short press BOOT: next page
  - Hold BOOT for 3 seconds: clear Wi-Fi and reopen setup portal

  Required libraries:
  - TFT_eSPI
  - ArduinoJson
  - WiFiManager
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
// Hardware
// -----------------------------------------------------------------------------

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();

// -----------------------------------------------------------------------------
// Colors
// -----------------------------------------------------------------------------

static constexpr uint16_t C_BG       = 0x0000;
static constexpr uint16_t C_PANEL    = 0x10A2;
static constexpr uint16_t C_PANEL2   = 0x18E3;
static constexpr uint16_t C_BORDER   = 0x3186;
static constexpr uint16_t C_TEXT     = 0xFFFF;
static constexpr uint16_t C_MUTED    = 0x8C71;
static constexpr uint16_t C_ORANGE   = 0xFD20;
static constexpr uint16_t C_CYAN     = 0x05FF;
static constexpr uint16_t C_GREEN    = 0x07E0;
static constexpr uint16_t C_RED      = 0xF800;
static constexpr uint16_t C_YELLOW   = 0xFFE0;
static constexpr uint16_t C_PURPLE   = 0xA81F;

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

static constexpr uint32_t STATS_INTERVAL_MS = 1000;
static constexpr uint32_t PAGE_INTERVAL_MS = 10000;
static constexpr uint32_t MARKET_INTERVAL_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_INTERVAL_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 250;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 3000;

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

static const char* AP_NAME = "CYD-Miner-Setup";
static const char* AP_PASSWORD = "bitcoin123";

// -----------------------------------------------------------------------------
// Application state
// -----------------------------------------------------------------------------

enum class Page : uint8_t {
  MINER = 0,
  SPEED = 1,
  MARKET = 2,
  NETWORK = 3,
  SYSTEM = 4
};

static constexpr uint8_t PAGE_COUNT = 5;

Page currentPage = Page::MINER;

uint32_t bootMs = 0;
uint32_t lastStatsMs = 0;
uint32_t lastPageMs = 0;
uint32_t lastMarketMs = 0;
uint32_t lastNetworkMs = 0;
uint32_t lastButtonMs = 0;
uint32_t buttonDownMs = 0;

bool buttonWasDown = false;
bool layoutDrawn = false;
bool miningEnabled = false;
bool pauseMining = false;

// -----------------------------------------------------------------------------
// Real mining benchmark state
// -----------------------------------------------------------------------------

struct SharedMiningState {
  uint64_t totalHashes = 0;
  uint32_t hashesWindow = 0;
  uint32_t nonce = 0;
  uint8_t bestLeadingZeroBits = 0;
};

SharedMiningState sharedMining;
portMUX_TYPE miningMux = portMUX_INITIALIZER_UNLOCKED;

struct MiningSnapshot {
  uint64_t totalHashes = 0;
  uint32_t nonce = 0;
  uint8_t bestLeadingZeroBits = 0;
  float currentKHs = 0;
  float smoothKHs = 0;
};

MiningSnapshot mining;

// -----------------------------------------------------------------------------
// Real public data
// -----------------------------------------------------------------------------

struct MarketData {
  bool valid = false;
  float priceUsd = 0;
  float change24h = 0;
  String error;
};

struct NetworkData {
  bool heightValid = false;
  bool feesValid = false;
  uint32_t blockHeight = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;
  String error;
};

MarketData market;
NetworkData network;

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

String uptimeText() {
  uint32_t seconds = (millis() - bootMs) / 1000UL;
  uint32_t hours = seconds / 3600UL;
  uint32_t minutes = (seconds % 3600UL) / 60UL;
  uint32_t remaining = seconds % 60UL;

  char buffer[20];
  snprintf(
    buffer,
    sizeof(buffer),
    "%02lu:%02lu:%02lu",
    static_cast<unsigned long>(hours),
    static_cast<unsigned long>(minutes),
    static_cast<unsigned long>(remaining)
  );

  return String(buffer);
}

// -----------------------------------------------------------------------------
// UI helpers
// -----------------------------------------------------------------------------

void panel(int x, int y, int w, int h, uint16_t fill = C_PANEL) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, C_BORDER);
}

void leftText(
  const String& value,
  int x,
  int y,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(value, x, y, font);
}

void centeredText(
  const String& value,
  int x,
  int y,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(value, x, y, font);
  tft.setTextDatum(TL_DATUM);
}

void clearValueArea(int x, int y, int w, int h, uint16_t background) {
  tft.fillRect(x, y, w, h, background);
}

void drawHeader(const String& title, const String& subtitle) {
  tft.fillRect(0, 0, 320, 34, C_PANEL);
  tft.drawFastHLine(0, 33, 320, C_BORDER);

  tft.fillCircle(17, 17, 11, C_ORANGE);
  centeredText("B", 17, 17, C_TEXT, C_ORANGE, 2);

  leftText(title, 36, 3, C_TEXT, C_PANEL, 2);
  leftText(subtitle, 36, 20, C_MUTED, C_PANEL, 1);

  bool online = WiFi.status() == WL_CONNECTED;
  tft.fillCircle(301, 10, 4, online ? C_GREEN : C_RED);
  centeredText(
    online ? "ONLINE" : "OFFLINE",
    283,
    24,
    online ? C_GREEN : C_RED,
    C_PANEL,
    1
  );
}

void drawFooter() {
  static const char* names[PAGE_COUNT] = {
    "MINER", "SPEED", "BTC", "NET", "SYS"
  };

  const int width = 64;

  tft.fillRect(0, 216, 320, 24, C_PANEL);
  tft.drawFastHLine(0, 216, 320, C_BORDER);

  for (int i = 0; i < PAGE_COUNT; i++) {
    bool active = static_cast<int>(currentPage) == i;

    if (active) {
      tft.fillRoundRect(i * width + 4, 219, 56, 18, 6, C_PANEL2);
    }

    centeredText(
      names[i],
      i * width + 32,
      228,
      active ? C_ORANGE : C_MUTED,
      active ? C_PANEL2 : C_PANEL,
      1
    );
  }
}

void statBox(
  int x,
  int y,
  int w,
  int h,
  const String& label
) {
  panel(x, y, w, h);
  centeredText(label, x + w / 2, y + 11, C_MUTED, C_PANEL, 1);
}

// -----------------------------------------------------------------------------
// Static page layouts
// -----------------------------------------------------------------------------

void drawMinerLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN HASH LAB", "REAL LOCAL DOUBLE-SHA256");

  panel(8, 42, 304, 68);
  leftText("MEASURED HASHRATE", 18, 50, C_MUTED, C_PANEL, 1);

  statBox(8, 119, 96, 87, "TOTAL HASHES");
  statBox(112, 119, 96, 87, "BEST ZERO BITS");
  statBox(216, 119, 96, 87, "UPTIME");

  drawFooter();
}

void drawSpeedLayout() {
  tft.fillScreen(C_BG);
  drawHeader("HASH ENGINE", "TWO COOPERATIVE WORKERS");

  panel(14, 47, 292, 105);
  centeredText("CURRENT SPEED", 160, 62, C_MUTED, C_PANEL, 1);

  statBox(14, 161, 138, 45, "CURRENT NONCE");
  statBox(168, 161, 138, 45, "ESP32 CORES");

  drawFooter();
}

void drawMarketLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN MARKET", "LIVE COINGECKO DATA");

  panel(10, 44, 300, 75);
  leftText("BTC / USD", 20, 53, C_MUTED, C_PANEL, 1);

  panel(10, 128, 300, 78);
  centeredText(
    "Price refreshes every 15 minutes",
    160,
    145,
    C_MUTED,
    C_PANEL,
    1
  );

  drawFooter();
}

void drawNetworkLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN NETWORK", "LIVE MEMPOOL.SPACE DATA");

  statBox(10, 45, 300, 48, "CURRENT BLOCK HEIGHT");

  statBox(10, 103, 94, 100, "FASTEST");
  statBox(113, 103, 94, 100, "30 MIN");
  statBox(216, 103, 94, 100, "1 HOUR");

  drawFooter();
}

void drawSystemLayout() {
  tft.fillScreen(C_BG);
  drawHeader("SYSTEM", "ESP32-2432S028R");

  statBox(10, 45, 145, 67, "CPU");
  statBox(165, 45, 145, 67, "FREE HEAP");
  statBox(10, 121, 145, 67, "WI-FI SIGNAL");
  statBox(165, 121, 145, 67, "FLASH");

  centeredText(
    "Short BOOT: next page   Hold BOOT: Wi-Fi setup",
    160,
    202,
    C_MUTED,
    C_BG,
    1
  );

  drawFooter();
}

void drawCurrentLayout() {
  switch (currentPage) {
    case Page::MINER:
      drawMinerLayout();
      break;

    case Page::SPEED:
      drawSpeedLayout();
      break;

    case Page::MARKET:
      drawMarketLayout();
      break;

    case Page::NETWORK:
      drawNetworkLayout();
      break;

    case Page::SYSTEM:
      drawSystemLayout();
      break;
  }

  layoutDrawn = true;
}

// -----------------------------------------------------------------------------
// Incremental value updates
// -----------------------------------------------------------------------------

void updateMinerValues() {
  // Hashrate
  clearValueArea(17, 67, 285, 35, C_PANEL);
  leftText(
    String(mining.smoothKHs, 2),
    18,
    65,
    C_ORANGE,
    C_PANEL,
    4
  );
  leftText("kH/s", 193, 79, C_TEXT, C_PANEL, 2);

  // Total hashes
  clearValueArea(17, 146, 78, 42, C_PANEL);
  centeredText(
    compactNumber(mining.totalHashes),
    56,
    166,
    C_TEXT,
    C_PANEL,
    2
  );

  // Best real leading-zero-bit score
  clearValueArea(121, 146, 78, 42, C_PANEL);
  centeredText(
    String(mining.bestLeadingZeroBits),
    160,
    166,
    C_YELLOW,
    C_PANEL,
    4
  );

  // Uptime
  clearValueArea(225, 146, 78, 42, C_PANEL);
  centeredText(
    uptimeText(),
    264,
    166,
    C_CYAN,
    C_PANEL,
    2
  );
}

void updateSpeedValues() {
  clearValueArea(28, 78, 264, 58, C_PANEL);

  centeredText(
    String(mining.smoothKHs, 2),
    160,
    98,
    C_ORANGE,
    C_PANEL,
    6
  );

  centeredText("kH/s", 160, 133, C_TEXT, C_PANEL, 2);

  clearValueArea(23, 183, 120, 16, C_PANEL);

  char nonceBuffer[16];
  snprintf(
    nonceBuffer,
    sizeof(nonceBuffer),
    "%08lX",
    static_cast<unsigned long>(mining.nonce)
  );

  centeredText(
    String(nonceBuffer),
    83,
    191,
    C_CYAN,
    C_PANEL,
    2
  );

  clearValueArea(177, 183, 120, 16, C_PANEL);
  centeredText(
    String(ESP.getChipCores()),
    237,
    191,
    C_GREEN,
    C_PANEL,
    2
  );
}

void updateMarketValues() {
  clearValueArea(19, 68, 282, 40, C_PANEL);

  if (market.valid) {
    leftText(
      "$" + String(market.priceUsd, 0),
      20,
      67,
      C_ORANGE,
      C_PANEL,
      4
    );

    uint16_t color = market.change24h >= 0 ? C_GREEN : C_RED;

    String change =
        String(market.change24h >= 0 ? "+" : "") +
        String(market.change24h, 2) +
        "%";

    centeredText(
      change,
      265,
      83,
      color,
      C_PANEL,
      2
    );
  } else {
    centeredText(
      market.error.length() ? market.error : "Loading price...",
      160,
      86,
      C_MUTED,
      C_PANEL,
      2
    );
  }

  clearValueArea(22, 161, 276, 34, C_PANEL);

  centeredText(
    market.valid
      ? "Source: CoinGecko public API"
      : "Waiting for public market data",
    160,
    178,
    market.valid ? C_GREEN : C_MUTED,
    C_PANEL,
    1
  );
}

void updateNetworkValues() {
  clearValueArea(22, 69, 276, 17, C_PANEL);

  centeredText(
    network.heightValid ? String(network.blockHeight) : "--",
    160,
    78,
    C_PURPLE,
    C_PANEL,
    2
  );

  clearValueArea(18, 132, 78, 55, C_PANEL);
  clearValueArea(121, 132, 78, 55, C_PANEL);
  clearValueArea(224, 132, 78, 55, C_PANEL);

  centeredText(
    network.feesValid
      ? String(network.fastestFee)
      : "--",
    57,
    151,
    C_ORANGE,
    C_PANEL,
    4
  );
  centeredText("sat/vB", 57, 180, C_MUTED, C_PANEL, 1);

  centeredText(
    network.feesValid
      ? String(network.halfHourFee)
      : "--",
    160,
    151,
    C_YELLOW,
    C_PANEL,
    4
  );
  centeredText("sat/vB", 160, 180, C_MUTED, C_PANEL, 1);

  centeredText(
    network.feesValid
      ? String(network.hourFee)
      : "--",
    263,
    151,
    C_CYAN,
    C_PANEL,
    4
  );
  centeredText("sat/vB", 263, 180, C_MUTED, C_PANEL, 1);
}

void updateSystemValues() {
  clearValueArea(20, 72, 125, 28, C_PANEL);
  centeredText(
    String(ESP.getCpuFreqMHz()) + " MHz",
    82,
    86,
    C_ORANGE,
    C_PANEL,
    2
  );

  clearValueArea(175, 72, 125, 28, C_PANEL);
  centeredText(
    compactNumber(ESP.getFreeHeap()),
    237,
    86,
    C_CYAN,
    C_PANEL,
    2
  );

  clearValueArea(20, 148, 125, 28, C_PANEL);
  centeredText(
    WiFi.status() == WL_CONNECTED
      ? String(WiFi.RSSI()) + " dBm"
      : "Offline",
    82,
    162,
    WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED,
    C_PANEL,
    2
  );

  clearValueArea(175, 148, 125, 28, C_PANEL);
  centeredText(
    String(ESP.getFlashChipSize() / 1024UL / 1024UL) + " MB",
    237,
    162,
    C_PURPLE,
    C_PANEL,
    2
  );
}

void updateCurrentPageValues() {
  switch (currentPage) {
    case Page::MINER:
      updateMinerValues();
      break;

    case Page::SPEED:
      updateSpeedValues();
      break;

    case Page::MARKET:
      updateMarketValues();
      break;

    case Page::NETWORK:
      updateNetworkValues();
      break;

    case Page::SYSTEM:
      updateSystemValues();
      break;
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

void drawWifiSetupScreen() {
  tft.fillScreen(C_BG);
  drawHeader("WI-FI SETUP", "FIRST-RUN CONFIGURATION");

  panel(12, 48, 296, 52);
  centeredText(
    "Connect your phone or Chromebook to:",
    160,
    61,
    C_MUTED,
    C_PANEL,
    1
  );
  centeredText(AP_NAME, 160, 82, C_ORANGE, C_PANEL, 2);

  panel(12, 109, 296, 58);
  centeredText(
    String("Password: ") + AP_PASSWORD,
    160,
    126,
    C_TEXT,
    C_PANEL,
    2
  );
  centeredText(
    "Choose your home Wi-Fi in the setup page.",
    160,
    151,
    C_CYAN,
    C_PANEL,
    1
  );

  panel(12, 176, 296, 30);
  centeredText(
    "Open 192.168.4.1 if the portal does not appear.",
    160,
    191,
    C_YELLOW,
    C_PANEL,
    1
  );
}

bool configureWifi() {
  drawWifiSetupScreen();

  WiFiManager manager;
  manager.setTitle("CYD Bitcoin Miner Setup");
  manager.setClass("invert");
  manager.setConfigPortalTimeout(240);
  manager.setConnectTimeout(30);

  bool connected = manager.autoConnect(AP_NAME, AP_PASSWORD);

  if (!connected) {
    tft.fillScreen(C_BG);
    centeredText(
      "WI-FI SETUP FAILED",
      160,
      105,
      C_RED,
      C_BG,
      4
    );
    delay(2500);
    ESP.restart();
  }

  tft.fillScreen(C_BG);
  centeredText(
    "WI-FI CONNECTED",
    160,
    100,
    C_GREEN,
    C_BG,
    4
  );
  centeredText(
    WiFi.SSID(),
    160,
    137,
    C_TEXT,
    C_BG,
    2
  );
  centeredText(
    WiFi.localIP().toString(),
    160,
    163,
    C_CYAN,
    C_BG,
    2
  );

  delay(1200);
  return true;
}

void resetWifi() {
  miningEnabled = false;

  tft.fillScreen(C_BG);
  centeredText(
    "RESETTING WI-FI",
    160,
    108,
    C_ORANGE,
    C_BG,
    4
  );

  WiFiManager manager;
  manager.resetSettings();

  delay(1500);
  ESP.restart();
}

// -----------------------------------------------------------------------------
// Public API requests
// -----------------------------------------------------------------------------

bool secureGet(const String& url, String& response) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("CYD-Bitcoin-Lab-Stable/1.0");

  if (!http.begin(client, url)) {
    return false;
  }

  int statusCode = http.GET();

  if (statusCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();

  return true;
}

void fetchMarketData() {
  pauseMining = true;

  String response;

  const String url =
      "https://api.coingecko.com/api/v3/simple/price"
      "?ids=bitcoin"
      "&vs_currencies=usd"
      "&include_24hr_change=true";

  if (secureGet(url, response)) {
    DynamicJsonDocument document(2048);

    if (!deserializeJson(document, response)) {
      market.priceUsd = document["bitcoin"]["usd"] | 0.0f;
      market.change24h =
          document["bitcoin"]["usd_24h_change"] | 0.0f;
      market.valid = market.priceUsd > 0;
      market.error = "";
    } else {
      market.valid = false;
      market.error = "Price JSON error";
    }
  } else {
    market.valid = false;
    market.error = "Price request failed";
  }

  lastMarketMs = millis();
  pauseMining = false;
}

void fetchNetworkData() {
  pauseMining = true;

  String response;

  if (secureGet(
        "https://mempool.space/api/blocks/tip/height",
        response
      )) {
    response.trim();

    network.blockHeight =
        static_cast<uint32_t>(response.toInt());

    network.heightValid = network.blockHeight > 0;
  } else {
    network.heightValid = false;
  }

  response = "";

  if (secureGet(
        "https://mempool.space/api/v1/fees/recommended",
        response
      )) {
    DynamicJsonDocument document(2048);

    if (!deserializeJson(document, response)) {
      network.fastestFee = document["fastestFee"] | 0;
      network.halfHourFee = document["halfHourFee"] | 0;
      network.hourFee = document["hourFee"] | 0;
      network.feesValid = true;
    } else {
      network.feesValid = false;
    }
  } else {
    network.feesValid = false;
  }

  lastNetworkMs = millis();
  pauseMining = false;
}

// -----------------------------------------------------------------------------
// Stable hashing workers
// -----------------------------------------------------------------------------

uint8_t leadingZeroBits(const uint8_t hash[32]) {
  uint8_t bits = 0;

  for (int index = 0; index < 32; index++) {
    if (hash[index] == 0) {
      bits += 8;
      continue;
    }

    uint8_t value = hash[index];

    while ((value & 0x80) == 0) {
      bits++;
      value <<= 1;
    }

    break;
  }

  return bits;
}

void miningWorker(void* argument) {
  uint32_t workerId =
      static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argument)
      );

  uint8_t header[80];
  uint8_t firstHash[32];
  uint8_t secondHash[32];

  for (int index = 0; index < 80; index++) {
    header[index] =
        static_cast<uint8_t>(esp_random() & 0xFF);
  }

  header[0] ^= static_cast<uint8_t>(workerId);

  uint32_t nonce = workerId * 0x70000000UL;

  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);

  for (;;) {
    if (!miningEnabled || pauseMining) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t batchHashes = 0;
    uint8_t batchBestBits = 0;

    // Moderate batch size avoids starving Wi-Fi and the UI.
    for (int batch = 0; batch < 256; batch++) {
      header[76] = nonce & 0xFF;
      header[77] = (nonce >> 8) & 0xFF;
      header[78] = (nonce >> 16) & 0xFF;
      header[79] = (nonce >> 24) & 0xFF;

      mbedtls_sha256_starts_ret(&context, 0);
      mbedtls_sha256_update_ret(&context, header, 80);
      mbedtls_sha256_finish_ret(&context, firstHash);

      mbedtls_sha256_starts_ret(&context, 0);
      mbedtls_sha256_update_ret(&context, firstHash, 32);
      mbedtls_sha256_finish_ret(&context, secondHash);

      uint8_t currentBits = leadingZeroBits(secondHash);

      if (currentBits > batchBestBits) {
        batchBestBits = currentBits;
      }

      nonce++;
      batchHashes++;
    }

    portENTER_CRITICAL(&miningMux);

    sharedMining.totalHashes += batchHashes;
    sharedMining.hashesWindow += batchHashes;
    sharedMining.nonce = nonce;

    if (batchBestBits > sharedMining.bestLeadingZeroBits) {
      sharedMining.bestLeadingZeroBits = batchBestBits;
    }

    portEXIT_CRITICAL(&miningMux);

    // Give the scheduler, Wi-Fi, and display time every batch.
    vTaskDelay(1);
  }
}

void startMiningWorkers() {
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
  uint32_t hashesThisWindow = 0;

  portENTER_CRITICAL(&miningMux);

  mining.totalHashes = sharedMining.totalHashes;
  mining.nonce = sharedMining.nonce;
  mining.bestLeadingZeroBits =
      sharedMining.bestLeadingZeroBits;

  hashesThisWindow = sharedMining.hashesWindow;
  sharedMining.hashesWindow = 0;

  portEXIT_CRITICAL(&miningMux);

  mining.currentKHs =
      hashesThisWindow /
      (STATS_INTERVAL_MS / 1000.0f) /
      1000.0f;

  if (mining.smoothKHs < 0.01f) {
    mining.smoothKHs = mining.currentKHs;
  } else {
    mining.smoothKHs =
        mining.smoothKHs * 0.72f +
        mining.currentKHs * 0.28f;
  }
}

// -----------------------------------------------------------------------------
// Page switching and button
// -----------------------------------------------------------------------------

void goToNextPage() {
  int next =
      (static_cast<int>(currentPage) + 1) %
      PAGE_COUNT;

  currentPage = static_cast<Page>(next);
  layoutDrawn = false;
  lastPageMs = millis();
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
      resetWifi();
    } else {
      goToNextPage();
    }
  }
}

void handleAutomaticPageChange() {
  if (millis() - lastPageMs >= PAGE_INTERVAL_MS) {
    goToNextPage();
  }
}

// -----------------------------------------------------------------------------
// Scheduled data
// -----------------------------------------------------------------------------

void maintainPublicData() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastMarketMs >= MARKET_INTERVAL_MS) {
    fetchMarketData();

    if (currentPage == Page::MARKET) {
      updateMarketValues();
    }
  }

  if (millis() - lastNetworkMs >= NETWORK_INTERVAL_MS) {
    fetchNetworkData();

    if (currentPage == Page::NETWORK) {
      updateNetworkValues();
    }
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

  configureWifi();

  startMiningWorkers();

  currentPage = Page::MINER;
  lastPageMs = millis();

  drawCurrentLayout();
  updateCurrentPageValues();

  // Load factual network data after the first screen is visible.
  fetchNetworkData();
  fetchMarketData();

  updateCurrentPageValues();
}

void loop() {
  handleButton();
  handleAutomaticPageChange();
  maintainPublicData();

  if (!layoutDrawn) {
    drawCurrentLayout();
    updateCurrentPageValues();
  }

  if (millis() - lastStatsMs >= STATS_INTERVAL_MS) {
    lastStatsMs = millis();
    sampleMiningStats();

    // Only repaint the small number regions.
    updateCurrentPageValues();
  }

  delay(2);
}
