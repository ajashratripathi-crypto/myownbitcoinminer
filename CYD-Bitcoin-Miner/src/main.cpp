
/*
  CYD Bitcoin Lab — STABLE NO-FLICKER main.cpp
  Board: ESP32-2432S028R / ESP32-WROOM-32E / ILI9341

  Key behavior:
  - The full page is drawn only when the page changes.
  - Between page changes, only the number fields are overwritten.
  - Pages rotate every 10 seconds.
  - No background mining tasks, no dual-core contention, no watchdog crashes.
  - Hashing runs in small cooperative batches inside loop().
  - All displayed values are factual:
      * measured local double-SHA256 hashrate
      * total hashes
      * current nonce
      * best leading-zero-bit result
      * uptime
      * live BTC/USD and 24h change
      * live Bitcoin block height and recommended fees
      * real ESP32 system values
  - No pool, no shares, no fake blocks, no transactions.

  Required libraries:
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
// Hardware
// -----------------------------------------------------------------------------

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();

// -----------------------------------------------------------------------------
// Colors
// -----------------------------------------------------------------------------

static constexpr uint16_t C_BG      = 0x0000;
static constexpr uint16_t C_PANEL   = 0x10A2;
static constexpr uint16_t C_PANEL2  = 0x18E3;
static constexpr uint16_t C_BORDER  = 0x3186;
static constexpr uint16_t C_TEXT    = 0xFFFF;
static constexpr uint16_t C_MUTED   = 0x8C71;
static constexpr uint16_t C_ORANGE  = 0xFD20;
static constexpr uint16_t C_CYAN    = 0x05FF;
static constexpr uint16_t C_GREEN   = 0x07E0;
static constexpr uint16_t C_RED     = 0xF800;
static constexpr uint16_t C_YELLOW  = 0xFFE0;
static constexpr uint16_t C_PURPLE  = 0xA81F;

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

static constexpr uint32_t VALUE_UPDATE_MS = 1000;
static constexpr uint32_t PAGE_CHANGE_MS = 10000;
static constexpr uint32_t MARKET_UPDATE_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_UPDATE_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 250;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 3000;

// Keep the hash batch small so loop(), Wi-Fi and display stay responsive.
static constexpr uint16_t HASH_BATCH_SIZE = 64;

// -----------------------------------------------------------------------------
// Wi-Fi setup
// -----------------------------------------------------------------------------

static const char* AP_NAME = "CYD-Miner-Setup";
static const char* AP_PASSWORD = "bitcoin123";

// -----------------------------------------------------------------------------
// Pages
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

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

uint32_t bootMs = 0;
uint32_t lastValueUpdateMs = 0;
uint32_t lastPageChangeMs = 0;
uint32_t lastMarketUpdateMs = 0;
uint32_t lastNetworkUpdateMs = 0;
uint32_t lastButtonActionMs = 0;
uint32_t buttonPressedMs = 0;

bool buttonWasPressed = false;
bool pageNeedsFullDraw = true;

// -----------------------------------------------------------------------------
// Mining state
// -----------------------------------------------------------------------------

uint8_t blockHeader[80];
uint32_t nonceValue = 0;
uint64_t totalHashes = 0;
uint32_t hashesInCurrentSecond = 0;
uint8_t bestLeadingZeroBits = 0;

float currentHashrateKHs = 0.0f;
float smoothedHashrateKHs = 0.0f;

mbedtls_sha256_context shaContext;

// -----------------------------------------------------------------------------
// Public data
// -----------------------------------------------------------------------------

struct MarketData {
  bool valid = false;
  float priceUsd = 0;
  float change24h = 0;
  String error;
};

struct NetworkData {
  bool blockHeightValid = false;
  bool feesValid = false;
  uint32_t blockHeight = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;
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
// Drawing helpers
// -----------------------------------------------------------------------------

void panel(int x, int y, int w, int h, uint16_t fill = C_PANEL) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, C_BORDER);
}

void drawLeft(
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

void drawCenter(
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

/*
  Draw a changing value without clearing or redrawing the page.

  setTextPadding() makes TFT_eSPI repaint the unused portion of the old
  text field using the selected background color. This prevents flashing
  and prevents old digits remaining behind the new value.
*/
void drawChangingCenter(
  const String& value,
  int centerX,
  int centerY,
  int fieldWidth,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(foreground, background);
  tft.setTextPadding(fieldWidth);
  tft.drawString(value, centerX, centerY, font);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

void drawChangingLeft(
  const String& value,
  int x,
  int y,
  int fieldWidth,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(foreground, background);
  tft.setTextPadding(fieldWidth);
  tft.drawString(value, x, y, font);
  tft.setTextPadding(0);
}

void drawHeader(const String& title, const String& subtitle) {
  tft.fillRect(0, 0, 320, 34, C_PANEL);
  tft.drawFastHLine(0, 33, 320, C_BORDER);

  tft.fillCircle(17, 17, 11, C_ORANGE);
  drawCenter("B", 17, 17, C_TEXT, C_ORANGE, 2);

  drawLeft(title, 36, 3, C_TEXT, C_PANEL, 2);
  drawLeft(subtitle, 36, 20, C_MUTED, C_PANEL, 1);

  bool online = WiFi.status() == WL_CONNECTED;
  tft.fillCircle(301, 10, 4, online ? C_GREEN : C_RED);

  drawCenter(
    online ? "ONLINE" : "OFFLINE",
    283,
    24,
    online ? C_GREEN : C_RED,
    C_PANEL,
    1
  );
}

void drawFooter() {
  static const char* pageNames[PAGE_COUNT] = {
    "MINER", "SPEED", "BTC", "NET", "SYS"
  };

  const int itemWidth = 64;

  tft.fillRect(0, 216, 320, 24, C_PANEL);
  tft.drawFastHLine(0, 216, 320, C_BORDER);

  for (int index = 0; index < PAGE_COUNT; index++) {
    bool selected = static_cast<int>(currentPage) == index;

    if (selected) {
      tft.fillRoundRect(
        index * itemWidth + 4,
        219,
        56,
        18,
        6,
        C_PANEL2
      );
    }

    drawCenter(
      pageNames[index],
      index * itemWidth + 32,
      228,
      selected ? C_ORANGE : C_MUTED,
      selected ? C_PANEL2 : C_PANEL,
      1
    );
  }
}

void drawStatBox(
  int x,
  int y,
  int w,
  int h,
  const String& label
) {
  panel(x, y, w, h);
  drawCenter(label, x + w / 2, y + 11, C_MUTED, C_PANEL, 1);
}

// -----------------------------------------------------------------------------
// Full page layouts — called only when page changes
// -----------------------------------------------------------------------------

void drawMinerPageLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN HASH LAB", "REAL LOCAL DOUBLE-SHA256");

  panel(8, 42, 304, 68);
  drawLeft("MEASURED HASHRATE", 18, 50, C_MUTED, C_PANEL, 1);

  drawStatBox(8, 119, 96, 87, "TOTAL HASHES");
  drawStatBox(112, 119, 96, 87, "BEST ZERO BITS");
  drawStatBox(216, 119, 96, 87, "UPTIME");

  drawFooter();
}

void drawSpeedPageLayout() {
  tft.fillScreen(C_BG);
  drawHeader("HASH ENGINE", "COOPERATIVE STABLE MODE");

  panel(14, 47, 292, 105);
  drawCenter("CURRENT SPEED", 160, 62, C_MUTED, C_PANEL, 1);

  drawStatBox(14, 161, 138, 45, "CURRENT NONCE");
  drawStatBox(168, 161, 138, 45, "CPU FREQUENCY");

  drawFooter();
}

void drawMarketPageLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN MARKET", "LIVE COINGECKO DATA");

  panel(10, 44, 300, 75);
  drawLeft("BTC / USD", 20, 53, C_MUTED, C_PANEL, 1);

  panel(10, 128, 300, 78);
  drawCenter(
    "Real public data, refreshed every 15 minutes",
    160,
    145,
    C_MUTED,
    C_PANEL,
    1
  );

  drawFooter();
}

void drawNetworkPageLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN NETWORK", "LIVE MEMPOOL.SPACE DATA");

  drawStatBox(10, 45, 300, 48, "CURRENT BLOCK HEIGHT");

  drawStatBox(10, 103, 94, 100, "FASTEST");
  drawStatBox(113, 103, 94, 100, "30 MIN");
  drawStatBox(216, 103, 94, 100, "1 HOUR");

  drawFooter();
}

void drawSystemPageLayout() {
  tft.fillScreen(C_BG);
  drawHeader("SYSTEM", "ESP32-2432S028R");

  drawStatBox(10, 45, 145, 67, "CPU");
  drawStatBox(165, 45, 145, 67, "FREE HEAP");
  drawStatBox(10, 121, 145, 67, "WI-FI SIGNAL");
  drawStatBox(165, 121, 145, 67, "FLASH");

  drawCenter(
    "Short BOOT: next page   Hold BOOT: Wi-Fi setup",
    160,
    202,
    C_MUTED,
    C_BG,
    1
  );

  drawFooter();
}

void drawCurrentPageLayout() {
  switch (currentPage) {
    case Page::MINER:
      drawMinerPageLayout();
      break;

    case Page::SPEED:
      drawSpeedPageLayout();
      break;

    case Page::MARKET:
      drawMarketPageLayout();
      break;

    case Page::NETWORK:
      drawNetworkPageLayout();
      break;

    case Page::SYSTEM:
      drawSystemPageLayout();
      break;
  }

  pageNeedsFullDraw = false;
}

// -----------------------------------------------------------------------------
// Number-only updates — no page redraw
// -----------------------------------------------------------------------------

void updateMinerNumbers() {
  drawChangingLeft(
    String(smoothedHashrateKHs, 2),
    18,
    66,
    170,
    C_ORANGE,
    C_PANEL,
    4
  );

  drawChangingLeft(
    "kH/s",
    202,
    80,
    70,
    C_TEXT,
    C_PANEL,
    2
  );

  drawChangingCenter(
    compactNumber(totalHashes),
    56,
    166,
    78,
    C_TEXT,
    C_PANEL,
    2
  );

  drawChangingCenter(
    String(bestLeadingZeroBits),
    160,
    166,
    78,
    C_YELLOW,
    C_PANEL,
    4
  );

  drawChangingCenter(
    uptimeText(),
    264,
    166,
    78,
    C_CYAN,
    C_PANEL,
    2
  );
}

void updateSpeedNumbers() {
  drawChangingCenter(
    String(smoothedHashrateKHs, 2),
    160,
    99,
    250,
    C_ORANGE,
    C_PANEL,
    6
  );

  drawChangingCenter(
    "kH/s",
    160,
    134,
    100,
    C_TEXT,
    C_PANEL,
    2
  );

  char nonceBuffer[16];
  snprintf(
    nonceBuffer,
    sizeof(nonceBuffer),
    "%08lX",
    static_cast<unsigned long>(nonceValue)
  );

  drawChangingCenter(
    String(nonceBuffer),
    83,
    191,
    120,
    C_CYAN,
    C_PANEL,
    2
  );

  drawChangingCenter(
    String(ESP.getCpuFreqMHz()) + " MHz",
    237,
    191,
    120,
    C_GREEN,
    C_PANEL,
    2
  );
}

void updateMarketNumbers() {
  if (market.valid) {
    drawChangingLeft(
      "$" + String(market.priceUsd, 0),
      20,
      67,
      185,
      C_ORANGE,
      C_PANEL,
      4
    );

    uint16_t changeColor =
        market.change24h >= 0 ? C_GREEN : C_RED;

    String changeText =
        String(market.change24h >= 0 ? "+" : "") +
        String(market.change24h, 2) +
        "%";

    drawChangingCenter(
      changeText,
      264,
      83,
      85,
      changeColor,
      C_PANEL,
      2
    );

    drawChangingCenter(
      "Source: CoinGecko public API",
      160,
      178,
      270,
      C_GREEN,
      C_PANEL,
      1
    );
  } else {
    drawChangingCenter(
      market.error.length() ? market.error : "Loading price...",
      160,
      86,
      270,
      C_MUTED,
      C_PANEL,
      2
    );

    drawChangingCenter(
      "Waiting for public market data",
      160,
      178,
      270,
      C_MUTED,
      C_PANEL,
      1
    );
  }
}

void updateNetworkNumbers() {
  drawChangingCenter(
    network.blockHeightValid
      ? String(network.blockHeight)
      : "--",
    160,
    78,
    270,
    C_PURPLE,
    C_PANEL,
    2
  );

  drawChangingCenter(
    network.feesValid
      ? String(network.fastestFee)
      : "--",
    57,
    151,
    76,
    C_ORANGE,
    C_PANEL,
    4
  );

  drawChangingCenter(
    "sat/vB",
    57,
    180,
    76,
    C_MUTED,
    C_PANEL,
    1
  );

  drawChangingCenter(
    network.feesValid
      ? String(network.halfHourFee)
      : "--",
    160,
    151,
    76,
    C_YELLOW,
    C_PANEL,
    4
  );

  drawChangingCenter(
    "sat/vB",
    160,
    180,
    76,
    C_MUTED,
    C_PANEL,
    1
  );

  drawChangingCenter(
    network.feesValid
      ? String(network.hourFee)
      : "--",
    263,
    151,
    76,
    C_CYAN,
    C_PANEL,
    4
  );

  drawChangingCenter(
    "sat/vB",
    263,
    180,
    76,
    C_MUTED,
    C_PANEL,
    1
  );
}

void updateSystemNumbers() {
  drawChangingCenter(
    String(ESP.getCpuFreqMHz()) + " MHz",
    82,
    86,
    125,
    C_ORANGE,
    C_PANEL,
    2
  );

  drawChangingCenter(
    compactNumber(ESP.getFreeHeap()),
    237,
    86,
    125,
    C_CYAN,
    C_PANEL,
    2
  );

  drawChangingCenter(
    WiFi.status() == WL_CONNECTED
      ? String(WiFi.RSSI()) + " dBm"
      : "Offline",
    82,
    162,
    125,
    WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED,
    C_PANEL,
    2
  );

  drawChangingCenter(
    String(ESP.getFlashChipSize() / 1024UL / 1024UL) + " MB",
    237,
    162,
    125,
    C_PURPLE,
    C_PANEL,
    2
  );
}

void updateCurrentPageNumbers() {
  switch (currentPage) {
    case Page::MINER:
      updateMinerNumbers();
      break;

    case Page::SPEED:
      updateSpeedNumbers();
      break;

    case Page::MARKET:
      updateMarketNumbers();
      break;

    case Page::NETWORK:
      updateNetworkNumbers();
      break;

    case Page::SYSTEM:
      updateSystemNumbers();
      break;
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi setup
// -----------------------------------------------------------------------------

void drawWifiSetupScreen() {
  tft.fillScreen(C_BG);
  drawHeader("WI-FI SETUP", "FIRST-RUN CONFIGURATION");

  panel(12, 48, 296, 52);

  drawCenter(
    "Connect your phone or Chromebook to:",
    160,
    61,
    C_MUTED,
    C_PANEL,
    1
  );

  drawCenter(AP_NAME, 160, 82, C_ORANGE, C_PANEL, 2);

  panel(12, 109, 296, 58);

  drawCenter(
    String("Password: ") + AP_PASSWORD,
    160,
    126,
    C_TEXT,
    C_PANEL,
    2
  );

  drawCenter(
    "Choose your home Wi-Fi in the setup page.",
    160,
    151,
    C_CYAN,
    C_PANEL,
    1
  );

  panel(12, 176, 296, 30);

  drawCenter(
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

    drawCenter(
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

  drawCenter(
    "WI-FI CONNECTED",
    160,
    100,
    C_GREEN,
    C_BG,
    4
  );

  drawCenter(WiFi.SSID(), 160, 137, C_TEXT, C_BG, 2);
  drawCenter(WiFi.localIP().toString(), 160, 163, C_CYAN, C_BG, 2);

  delay(1200);

  return true;
}

void resetWifi() {
  miningEnabled = false;

  tft.fillScreen(C_BG);

  drawCenter(
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
// HTTPS data
// -----------------------------------------------------------------------------

bool secureGet(const String& url, String& response) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("CYD-Bitcoin-Lab-NoFlicker/1.0");

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

  lastMarketUpdateMs = millis();
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

    network.blockHeightValid = network.blockHeight > 0;
  } else {
    network.blockHeightValid = false;
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

  lastNetworkUpdateMs = millis();
  pauseMining = false;
}

// -----------------------------------------------------------------------------
// Cooperative local hashing
// -----------------------------------------------------------------------------

uint8_t countLeadingZeroBits(const uint8_t hash[32]) {
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

void initializeHashing() {
  for (int index = 0; index < 80; index++) {
    blockHeader[index] =
        static_cast<uint8_t>(esp_random() & 0xFF);
  }

  mbedtls_sha256_init(&shaContext);
  miningEnabled = true;
}

void hashSmallBatch() {
  if (!miningEnabled || pauseMining) {
    return;
  }

  uint8_t firstHash[32];
  uint8_t secondHash[32];

  for (uint16_t batch = 0; batch < HASH_BATCH_SIZE; batch++) {
    blockHeader[76] = nonceValue & 0xFF;
    blockHeader[77] = (nonceValue >> 8) & 0xFF;
    blockHeader[78] = (nonceValue >> 16) & 0xFF;
    blockHeader[79] = (nonceValue >> 24) & 0xFF;

    mbedtls_sha256_starts_ret(&shaContext, 0);
    mbedtls_sha256_update_ret(&shaContext, blockHeader, 80);
    mbedtls_sha256_finish_ret(&shaContext, firstHash);

    mbedtls_sha256_starts_ret(&shaContext, 0);
    mbedtls_sha256_update_ret(&shaContext, firstHash, 32);
    mbedtls_sha256_finish_ret(&shaContext, secondHash);

    uint8_t leadingBits = countLeadingZeroBits(secondHash);

    if (leadingBits > bestLeadingZeroBits) {
      bestLeadingZeroBits = leadingBits;
    }

    nonceValue++;
    totalHashes++;
    hashesInCurrentSecond++;
  }
}

void sampleHashrate() {
  currentHashrateKHs =
      hashesInCurrentSecond /
      (VALUE_UPDATE_MS / 1000.0f) /
      1000.0f;

  hashesInCurrentSecond = 0;

  if (smoothedHashrateKHs < 0.01f) {
    smoothedHashrateKHs = currentHashrateKHs;
  } else {
    smoothedHashrateKHs =
        smoothedHashrateKHs * 0.75f +
        currentHashrateKHs * 0.25f;
  }
}

// -----------------------------------------------------------------------------
// Page switching and button
// -----------------------------------------------------------------------------

void switchToNextPage() {
  int nextPage =
      (static_cast<int>(currentPage) + 1) %
      PAGE_COUNT;

  currentPage = static_cast<Page>(nextPage);
  pageNeedsFullDraw = true;
  lastPageChangeMs = millis();
}

void handleButton() {
  bool pressed = digitalRead(PIN_BOOT) == LOW;

  if (pressed && !buttonWasPressed) {
    buttonWasPressed = true;
    buttonPressedMs = millis();
  }

  if (!pressed && buttonWasPressed) {
    uint32_t pressDuration = millis() - buttonPressedMs;
    buttonWasPressed = false;

    if (millis() - lastButtonActionMs < BUTTON_DEBOUNCE_MS) {
      return;
    }

    lastButtonActionMs = millis();

    if (pressDuration >= BUTTON_LONG_PRESS_MS) {
      resetWifi();
    } else {
      switchToNextPage();
    }
  }
}

void handleAutomaticPageChange() {
  if (millis() - lastPageChangeMs >= PAGE_CHANGE_MS) {
    switchToNextPage();
  }
}

// -----------------------------------------------------------------------------
// Scheduled public data
// -----------------------------------------------------------------------------

void maintainPublicData() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastMarketUpdateMs >= MARKET_UPDATE_MS) {
    fetchMarketData();

    if (currentPage == Page::MARKET) {
      updateMarketNumbers();
    }
  }

  if (millis() - lastNetworkUpdateMs >= NETWORK_UPDATE_MS) {
    fetchNetworkData();

    if (currentPage == Page::NETWORK) {
      updateNetworkNumbers();
    }
  }
}

// -----------------------------------------------------------------------------
// Setup and loop
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
  initializeHashing();

  currentPage = Page::MINER;
  lastPageChangeMs = millis();

  drawCurrentPageLayout();
  updateCurrentPageNumbers();

  // Fetch real public data after a working first screen is already visible.
  fetchNetworkData();
  fetchMarketData();
}

void loop() {
  // Real hashing is cooperative, not a separate task.
  hashSmallBatch();

  handleButton();
  handleAutomaticPageChange();
  maintainPublicData();

  // Full-screen drawing occurs only here, after a page change.
  if (pageNeedsFullDraw) {
    drawCurrentPageLayout();
    updateCurrentPageNumbers();
  }

  // Every second only the numbers are overwritten.
  if (millis() - lastValueUpdateMs >= VALUE_UPDATE_MS) {
    lastValueUpdateMs = millis();

    sampleHashrate();
    updateCurrentPageNumbers();
  }

  delay(1);
}
