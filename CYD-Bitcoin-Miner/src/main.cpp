/*
  CYD Bitcoin Lab V8 — MAIN.CPP ONLY
  ----------------------------------
  Target: ESP32-2432S028R / ESP32-WROOM-32E / ILI9341 320x240 landscape

  Replace only:
    CYD-Bitcoin-Miner/src/main.cpp

  Existing platformio.ini libraries:
    - TFT_eSPI
    - ArduinoJson
    - WiFiManager

  What this firmware displays
  ---------------------------
  MINER PAGE
    - measured local double-SHA256 hashrate
    - factual local shares at a defined 16-bit target
    - best leading-zero-bit score
    - total hashes
    - current nonce
    - uptime
    - current Bitcoin block height

  MARKET PAGE
    - Coinbase BTC-USD price
    - factual 24-hour change, high and low
    - hourly candlesticks for the last 24 hours
    - close-price line over the candlesticks
    - volume bars

  NETWORK PAGE
    - mempool.space block height
    - fastest, 30-minute and 1-hour fee estimates
    - Wi-Fi signal and API status

  DEVICE PAGE
    - CPU, heap, minimum heap, flash, boot count and reset reason

  Important honesty note
  ----------------------
  This is a local SHA-256 benchmark/lottery display. It does not connect to a
  mining pool, submit shares, move Bitcoin or claim earnings. "Local shares"
  are hashes meeting this firmware's explicitly defined 16-bit local target.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_system.h"
#include "mbedtls/sha256.h"

// ============================================================================
// Hardware
// ============================================================================

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();
Preferences preferences;

// ============================================================================
// Display theme
// ============================================================================

static constexpr uint16_t COL_BG          = 0x0000;
static constexpr uint16_t COL_HEADER      = 0x0841;
static constexpr uint16_t COL_PANEL       = 0x1082;
static constexpr uint16_t COL_PANEL_ALT   = 0x18C3;
static constexpr uint16_t COL_BORDER      = 0x31A6;
static constexpr uint16_t COL_GRID        = 0x2124;
static constexpr uint16_t COL_TEXT        = 0xFFFF;
static constexpr uint16_t COL_MUTED       = 0x9CD3;
static constexpr uint16_t COL_ORANGE      = 0xFD20;
static constexpr uint16_t COL_ORANGE_DARK = 0xA300;
static constexpr uint16_t COL_CYAN        = 0x05FF;
static constexpr uint16_t COL_GREEN       = 0x07E0;
static constexpr uint16_t COL_RED         = 0xF800;
static constexpr uint16_t COL_YELLOW      = 0xFFE0;
static constexpr uint16_t COL_PURPLE      = 0xA81F;

// ============================================================================
// Firmware and timing
// ============================================================================

static const char* FIRMWARE_VERSION = "8.0.0";
static const char* SETUP_AP_NAME = "CYD-Miner-Setup";
static const char* SETUP_AP_PASSWORD = "bitcoin123";

static constexpr uint32_t VALUE_REFRESH_MS = 1000;
static constexpr uint32_t DEFAULT_PAGE_MS = 10000;
static constexpr uint32_t TICKER_REFRESH_MS = 60UL * 1000UL;
static constexpr uint32_t MARKET_REFRESH_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_REFRESH_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t SAVE_REFRESH_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t WIFI_RECONNECT_MS = 15UL * 1000UL;
static constexpr uint32_t LONG_BUTTON_PRESS_MS = 3000;
static constexpr uint16_t HASH_BATCH_SIZE = 64;
static constexpr uint8_t CHART_CANDLE_COUNT = 24;

// ============================================================================
// Configuration and persistent data
// ============================================================================

struct AppConfig {
  String deviceName = "ORANGE NODE";
  String bitcoinAddress = "";
  int utcOffsetHours = -4;
  bool autoRotate = true;
  uint16_t pageSeconds = 10;
};

struct LifetimeStats {
  uint64_t hashesBeforeBoot = 0;
  uint64_t runtimeBeforeBoot = 0;
  uint64_t localSharesBeforeBoot = 0;
  uint32_t bootCount = 0;
  uint8_t bestBitsBeforeBoot = 0;
};

AppConfig config;
LifetimeStats lifetime;

char portalDeviceName[25] = "ORANGE NODE";
char portalBitcoinAddress[91] = "";
char portalUtcOffset[5] = "-4";
char portalAutoRotate[2] = "1";
char portalPageSeconds[4] = "10";
bool portalSaveRequested = false;

// ============================================================================
// Pages and runtime state
// ============================================================================

enum class Page : uint8_t {
  MINER = 0,
  MARKET = 1,
  NETWORK = 2,
  DEVICE = 3
};

static constexpr uint8_t PAGE_COUNT = 4;
Page currentPage = Page::MINER;

bool pageNeedsFullDraw = true;
bool chartNeedsDraw = true;
bool buttonWasDown = false;

uint32_t bootMillis = 0;
uint32_t lastValueRefresh = 0;
uint32_t lastPageChange = 0;
uint32_t lastTickerRefresh = 0;
uint32_t lastMarketRefresh = 0;
uint32_t lastNetworkRefresh = 0;
uint32_t lastSaveRefresh = 0;
uint32_t lastWifiReconnect = 0;
uint32_t buttonDownMillis = 0;

// Stagger the first API calls so startup remains responsive.
enum class StartupFetchStep : uint8_t {
  WAITING = 0,
  BLOCK_HEIGHT = 1,
  FEES = 2,
  TICKER = 3,
  STATS = 4,
  CANDLES = 5,
  COMPLETE = 6
};

enum class PendingApiTask : uint8_t {
  NONE = 0,
  MARKET_STATS = 1,
  MARKET_CANDLES = 2,
  NETWORK_HEIGHT = 3,
  NETWORK_FEES = 4
};

StartupFetchStep startupFetchStep = StartupFetchStep::WAITING;
PendingApiTask pendingApiTask = PendingApiTask::NONE;
uint32_t nextStartupFetchMillis = 0;
uint32_t pendingApiMillis = 0;

// ============================================================================
// Local SHA-256 benchmark
// ============================================================================

uint8_t benchmarkHeader[80];
uint32_t currentNonce = 0;
uint64_t sessionHashes = 0;
uint64_t sessionLocalShares16 = 0;
uint32_t hashesInWindow = 0;
uint8_t sessionBestLeadingBits = 0;
float currentHashrateKHs = 0.0f;
float smoothedHashrateKHs = 0.0f;

mbedtls_sha256_context headerMidstate;
mbedtls_sha256_context firstHashContext;
mbedtls_sha256_context secondHashContext;

// ============================================================================
// Coinbase market data
// ============================================================================

struct Candle {
  uint32_t timestamp = 0;
  float low = 0;
  float high = 0;
  float open = 0;
  float close = 0;
  float volume = 0;
};

struct MarketData {
  bool tickerValid = false;
  bool statsValid = false;
  bool candlesValid = false;

  float price = 0;
  float bid = 0;
  float ask = 0;
  float open24h = 0;
  float high24h = 0;
  float low24h = 0;
  float volume24h = 0;
  float change24h = 0;

  Candle candles[CHART_CANDLE_COUNT];
  uint8_t candleCount = 0;

  String tickerStatus = "WAITING";
  String chartStatus = "WAITING";
};

MarketData market;

// ============================================================================
// Bitcoin network data
// ============================================================================

struct NetworkData {
  bool heightValid = false;
  bool feesValid = false;

  uint32_t blockHeight = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;

  String status = "WAITING";
};

NetworkData network;

// ============================================================================
// Diagnostics
// ============================================================================

struct Diagnostics {
  uint32_t minimumFreeHeap = UINT32_MAX;
  uint32_t wifiReconnectCount = 0;
  uint32_t apiFailureCount = 0;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
};

Diagnostics diagnostics;

// ============================================================================
// Formatting helpers
// ============================================================================

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

String uptimeText(uint64_t seconds) {
  uint32_t days = seconds / 86400ULL;
  uint32_t hours = (seconds % 86400ULL) / 3600ULL;
  uint32_t minutes = (seconds % 3600ULL) / 60ULL;
  uint32_t remaining = seconds % 60ULL;

  char buffer[24];

  if (days > 0) {
    snprintf(
      buffer,
      sizeof(buffer),
      "%lud %02lu:%02lu",
      static_cast<unsigned long>(days),
      static_cast<unsigned long>(hours),
      static_cast<unsigned long>(minutes)
    );
  } else {
    snprintf(
      buffer,
      sizeof(buffer),
      "%02lu:%02lu:%02lu",
      static_cast<unsigned long>(hours),
      static_cast<unsigned long>(minutes),
      static_cast<unsigned long>(remaining)
    );
  }

  return String(buffer);
}

String nonceText(uint32_t nonce) {
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(nonce));
  return String(buffer);
}

String resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER ON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT: return "INT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "UNKNOWN";
  }
}

String shortAddress(const String& address) {
  if (address.isEmpty()) {
    return "NOT SET";
  }

  if (address.length() <= 20) {
    return address;
  }

  return address.substring(0, 8) + "..." + address.substring(address.length() - 6);
}

String isoUtcTime(time_t value) {
  struct tm timeInfo;
  gmtime_r(&value, &timeInfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return String(buffer);
}

uint64_t totalHashesAllTime() {
  return lifetime.hashesBeforeBoot + sessionHashes;
}

uint64_t totalLocalSharesAllTime() {
  return lifetime.localSharesBeforeBoot + sessionLocalShares16;
}

uint8_t bestBitsAllTime() {
  return max(lifetime.bestBitsBeforeBoot, sessionBestLeadingBits);
}

// ============================================================================
// Persistent storage
// ============================================================================

void loadPersistentData() {
  preferences.begin("btc-lab", true);

  config.deviceName = preferences.getString("name", "ORANGE NODE");
  config.bitcoinAddress = preferences.getString("btc", "");
  config.utcOffsetHours = preferences.getInt("utc", -4);
  config.autoRotate = preferences.getBool("rotate", true);
  config.pageSeconds = preferences.getUShort("pageSec", 10);

  lifetime.hashesBeforeBoot = preferences.getULong64("hashes", 0);
  lifetime.runtimeBeforeBoot = preferences.getULong64("runtime", 0);
  lifetime.localSharesBeforeBoot = preferences.getULong64("shares16", 0);
  lifetime.bootCount = preferences.getUInt("boots", 0);
  lifetime.bestBitsBeforeBoot = preferences.getUChar("bestBits", 0);

  preferences.end();

  config.pageSeconds = constrain(config.pageSeconds, 5, 60);
  lifetime.bootCount++;
}

void saveConfiguration() {
  preferences.begin("btc-lab", false);

  preferences.putString("name", config.deviceName);
  preferences.putString("btc", config.bitcoinAddress);
  preferences.putInt("utc", config.utcOffsetHours);
  preferences.putBool("rotate", config.autoRotate);
  preferences.putUShort("pageSec", config.pageSeconds);

  preferences.end();
}

void saveLifetimeStats() {
  preferences.begin("btc-lab", false);

  preferences.putULong64("hashes", totalHashesAllTime());
  preferences.putULong64(
    "runtime",
    lifetime.runtimeBeforeBoot + ((millis() - bootMillis) / 1000ULL)
  );
  preferences.putULong64("shares16", totalLocalSharesAllTime());
  preferences.putUInt("boots", lifetime.bootCount);
  preferences.putUChar("bestBits", bestBitsAllTime());

  preferences.end();
  lastSaveRefresh = millis();
}

// ============================================================================
// Drawing primitives
// ============================================================================

void drawLeftText(
  const String& text,
  int x,
  int y,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(text, x, y, font);
}

void drawCenteredText(
  const String& text,
  int centerX,
  int centerY,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(text, centerX, centerY, font);
  tft.setTextDatum(TL_DATUM);
}

void drawPanel(int x, int y, int width, int height, uint16_t fill = COL_PANEL) {
  tft.fillRoundRect(x, y, width, height, 8, fill);
  tft.drawRoundRect(x, y, width, height, 8, COL_BORDER);
}

/*
  These two helpers completely clear a fixed rectangle before drawing a new
  value. That is the key to avoiding outlined/ghost digits and overlapping text.
*/
void drawValueCentered(
  const String& text,
  int x,
  int y,
  int width,
  int height,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.fillRect(x, y, width, height, background);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(text, x + width / 2, y + height / 2, font);
  tft.setTextDatum(TL_DATUM);
}

void drawValueLeft(
  const String& text,
  int x,
  int y,
  int width,
  int height,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.fillRect(x, y, width, height, background);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(text, x, y + height / 2, font);
  tft.setTextDatum(TL_DATUM);
}

void drawHeader(const String& title, const String& subtitle) {
  tft.fillRect(0, 0, 320, 33, COL_HEADER);
  tft.drawFastHLine(0, 32, 320, COL_ORANGE_DARK);

  tft.fillCircle(17, 16, 11, COL_ORANGE);
  drawCenteredText("B", 17, 16, COL_BG, COL_ORANGE, 2);

  drawLeftText(title, 35, 2, COL_TEXT, COL_HEADER, 2);
  drawLeftText(subtitle, 35, 19, COL_MUTED, COL_HEADER, 1);

  bool online = WiFi.status() == WL_CONNECTED;
  tft.fillCircle(302, 10, 4, online ? COL_GREEN : COL_RED);
  drawCenteredText(
    online ? "LIVE" : "OFF",
    291,
    24,
    online ? COL_GREEN : COL_RED,
    COL_HEADER,
    1
  );
}

void drawFooter() {
  static const char* pageLabels[PAGE_COUNT] = {"MINER", "MARKET", "NET", "DEVICE"};
  static const int pageWidths[PAGE_COUNT] = {80, 80, 80, 80};

  tft.fillRect(0, 215, 320, 25, COL_HEADER);
  tft.drawFastHLine(0, 215, 320, COL_BORDER);

  int cursorX = 0;
  for (uint8_t index = 0; index < PAGE_COUNT; index++) {
    bool selected = static_cast<uint8_t>(currentPage) == index;
    int itemWidth = pageWidths[index];

    if (selected) {
      tft.fillRoundRect(cursorX + 6, 219, itemWidth - 12, 17, 5, COL_PANEL_ALT);
      tft.drawRoundRect(cursorX + 6, 219, itemWidth - 12, 17, 5, COL_ORANGE_DARK);
    }

    drawCenteredText(
      pageLabels[index],
      cursorX + itemWidth / 2,
      227,
      selected ? COL_ORANGE : COL_MUTED,
      selected ? COL_PANEL_ALT : COL_HEADER,
      1
    );

    cursorX += itemWidth;
  }
}

void drawMetricLabel(const String& label, int x, int y, int width) {
  drawCenteredText(label, x + width / 2, y, COL_MUTED, COL_PANEL, 1);
}

// ============================================================================
// Static page layouts — only drawn when changing pages
// ============================================================================

void drawMinerLayout() {
  tft.fillScreen(COL_BG);
  drawHeader("NERD LAB", "LOCAL BENCHMARK • NO POOL");

  // Main hashrate card.
  drawPanel(6, 38, 188, 91);
  drawLeftText("MEASURED HASHRATE", 15, 46, COL_MUTED, COL_PANEL, 1);
  tft.fillRoundRect(14, 105, 172, 16, 5, COL_ORANGE_DARK);
  drawCenteredText("DOUBLE-SHA256 • STABLE MODE", 100, 113, COL_TEXT, COL_ORANGE_DARK, 1);

  // Right-side live metrics.
  drawPanel(199, 38, 115, 91);
  drawLeftText("BLOCK", 207, 45, COL_MUTED, COL_PANEL, 1);
  drawLeftText("LOCAL SHARES 16B", 207, 73, COL_MUTED, COL_PANEL, 1);
  drawLeftText("BEST BITS", 207, 101, COL_MUTED, COL_PANEL, 1);

  // Bottom metrics strip.
  drawPanel(6, 135, 308, 73);

  tft.drawFastVLine(82, 143, 56, COL_BORDER);
  tft.drawFastVLine(159, 143, 56, COL_BORDER);
  tft.drawFastVLine(236, 143, 56, COL_BORDER);

  drawMetricLabel("TOTAL HASHES", 8, 148, 72);
  drawMetricLabel("UPTIME", 85, 148, 72);
  drawMetricLabel("NONCE", 162, 148, 72);
  drawMetricLabel("WI-FI", 239, 148, 72);

  drawFooter();
}

void drawMarketLayout() {
  tft.fillScreen(COL_BG);
  drawHeader("BTC / USD", "COINBASE • 24 HOURLY CANDLES");

  drawPanel(6, 38, 308, 57);
  drawLeftText("LAST PRICE", 15, 45, COL_MUTED, COL_PANEL, 1);
  drawLeftText("24H CHANGE", 223, 45, COL_MUTED, COL_PANEL, 1);

  drawPanel(6, 101, 308, 107);
  drawLeftText("HIGH", 15, 107, COL_MUTED, COL_PANEL, 1);
  drawLeftText("LOW", 112, 107, COL_MUTED, COL_PANEL, 1);
  drawLeftText("24 x 1H", 252, 107, COL_MUTED, COL_PANEL, 1);

  drawFooter();
}

void drawNetworkLayout() {
  tft.fillScreen(COL_BG);
  drawHeader("BITCOIN NETWORK", "MEMPOOL.SPACE LIVE DATA");

  drawPanel(6, 38, 308, 50);
  drawCenteredText("CURRENT BLOCK HEIGHT", 160, 49, COL_MUTED, COL_PANEL, 1);

  drawPanel(6, 95, 98, 90);
  drawPanel(111, 95, 98, 90);
  drawPanel(216, 95, 98, 90);

  drawCenteredText("FASTEST", 55, 108, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("30 MIN", 160, 108, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("1 HOUR", 265, 108, COL_MUTED, COL_PANEL, 1);

  drawPanel(6, 192, 308, 16, COL_PANEL_ALT);
  drawFooter();
}

void drawDeviceLayout() {
  tft.fillScreen(COL_BG);
  drawHeader("DEVICE HEALTH", "ESP32-32E TELEMETRY");

  drawPanel(6, 38, 150, 70);
  drawPanel(164, 38, 150, 70);
  drawPanel(6, 116, 150, 70);
  drawPanel(164, 116, 150, 70);

  drawCenteredText("FREE HEAP", 81, 51, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("MIN HEAP", 239, 51, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("CPU / FLASH", 81, 129, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("LIFETIME", 239, 129, COL_MUTED, COL_PANEL, 1);

  drawPanel(6, 193, 308, 15, COL_PANEL_ALT);
  drawFooter();
}

void drawCurrentPageLayout() {
  switch (currentPage) {
    case Page::MINER: drawMinerLayout(); break;
    case Page::MARKET: drawMarketLayout(); break;
    case Page::NETWORK: drawNetworkLayout(); break;
    case Page::DEVICE: drawDeviceLayout(); break;
  }

  pageNeedsFullDraw = false;
}

// ============================================================================
// Number-only updates — no full page refresh
// ============================================================================

void updateMinerValues() {
  drawValueLeft(
    String(smoothedHashrateKHs, 2),
    15,
    63,
    125,
    35,
    COL_ORANGE,
    COL_PANEL,
    4
  );

  drawValueLeft("kH/s", 143, 72, 43, 24, COL_TEXT, COL_PANEL, 2);

  drawValueLeft(
    network.heightValid ? String(network.blockHeight) : "--",
    207,
    55,
    98,
    16,
    COL_PURPLE,
    COL_PANEL,
    2
  );

  drawValueLeft(
    compactNumber(totalLocalSharesAllTime()),
    207,
    83,
    98,
    16,
    COL_GREEN,
    COL_PANEL,
    2
  );

  drawValueLeft(
    String(bestBitsAllTime()),
    207,
    111,
    98,
    15,
    COL_YELLOW,
    COL_PANEL,
    2
  );

  drawValueCentered(
    compactNumber(totalHashesAllTime()),
    10,
    164,
    68,
    30,
    COL_TEXT,
    COL_PANEL,
    2
  );

  drawValueCentered(
    uptimeText((millis() - bootMillis) / 1000ULL),
    87,
    164,
    68,
    30,
    COL_CYAN,
    COL_PANEL,
    1
  );

  drawValueCentered(
    nonceText(currentNonce),
    164,
    164,
    68,
    30,
    COL_ORANGE,
    COL_PANEL,
    1
  );

  drawValueCentered(
    WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "OFF",
    241,
    164,
    68,
    30,
    WiFi.status() == WL_CONNECTED ? COL_GREEN : COL_RED,
    COL_PANEL,
    1
  );
}

void updateMarketHeaderValues() {
  drawValueLeft(
    market.tickerValid ? "$" + String(market.price, 2) : "UNAVAILABLE",
    15,
    56,
    196,
    31,
    market.tickerValid ? COL_ORANGE : COL_RED,
    COL_PANEL,
    market.tickerValid ? 4 : 2
  );

  String changeText = market.statsValid
    ? String(market.change24h >= 0 ? "+" : "") + String(market.change24h, 2) + "%"
    : "--";

  drawValueCentered(
    changeText,
    223,
    57,
    82,
    29,
    market.change24h >= 0 ? COL_GREEN : COL_RED,
    COL_PANEL,
    2
  );
}

void updateNetworkValues() {
  drawValueCentered(
    network.heightValid ? String(network.blockHeight) : "--",
    20,
    61,
    280,
    22,
    COL_PURPLE,
    COL_PANEL,
    2
  );

  drawValueCentered(
    network.feesValid ? String(network.fastestFee) : "--",
    18,
    125,
    74,
    38,
    COL_ORANGE,
    COL_PANEL,
    4
  );
  drawValueCentered("sat/vB", 18, 163, 74, 14, COL_MUTED, COL_PANEL, 1);

  drawValueCentered(
    network.feesValid ? String(network.halfHourFee) : "--",
    123,
    125,
    74,
    38,
    COL_YELLOW,
    COL_PANEL,
    4
  );
  drawValueCentered("sat/vB", 123, 163, 74, 14, COL_MUTED, COL_PANEL, 1);

  drawValueCentered(
    network.feesValid ? String(network.hourFee) : "--",
    228,
    125,
    74,
    38,
    COL_CYAN,
    COL_PANEL,
    4
  );
  drawValueCentered("sat/vB", 228, 163, 74, 14, COL_MUTED, COL_PANEL, 1);

  String statusText = network.status;
  if (WiFi.status() == WL_CONNECTED) {
    statusText += "  •  WI-FI " + String(WiFi.RSSI()) + " dBm";
  }

  drawValueCentered(statusText, 12, 193, 296, 13, COL_MUTED, COL_PANEL_ALT, 1);
}

void updateDeviceValues() {
  diagnostics.minimumFreeHeap = min(diagnostics.minimumFreeHeap, ESP.getFreeHeap());

  drawValueCentered(
    compactNumber(ESP.getFreeHeap()),
    18,
    67,
    126,
    29,
    COL_CYAN,
    COL_PANEL,
    2
  );

  drawValueCentered(
    compactNumber(diagnostics.minimumFreeHeap),
    176,
    67,
    126,
    29,
    COL_YELLOW,
    COL_PANEL,
    2
  );

  drawValueCentered(
    String(ESP.getCpuFreqMHz()) + " MHz",
    18,
    143,
    126,
    22,
    COL_ORANGE,
    COL_PANEL,
    2
  );

  drawValueCentered(
    String(ESP.getFlashChipSize() / 1024UL / 1024UL) + " MB FLASH",
    18,
    166,
    126,
    14,
    COL_MUTED,
    COL_PANEL,
    1
  );

  drawValueCentered(
    compactNumber(totalHashesAllTime()),
    176,
    143,
    126,
    22,
    COL_TEXT,
    COL_PANEL,
    2
  );

  drawValueCentered(
    "BOOTS " + String(lifetime.bootCount) + "  •  " + uptimeText(
      lifetime.runtimeBeforeBoot + ((millis() - bootMillis) / 1000ULL)
    ),
    176,
    166,
    126,
    14,
    COL_MUTED,
    COL_PANEL,
    1
  );

  drawValueCentered(
    "RESET " + resetReasonText(diagnostics.resetReason) +
    "  •  API ERR " + String(diagnostics.apiFailureCount),
    12,
    194,
    296,
    13,
    COL_MUTED,
    COL_PANEL_ALT,
    1
  );
}

void updateCurrentPageValues() {
  switch (currentPage) {
    case Page::MINER: updateMinerValues(); break;
    case Page::MARKET: updateMarketHeaderValues(); break;
    case Page::NETWORK: updateNetworkValues(); break;
    case Page::DEVICE: updateDeviceValues(); break;
  }
}

// ============================================================================
// Market chart: hourly candles + close line + volume bars
// ============================================================================

void drawMarketChart() {
  // Everything stays safely inside the market chart panel.
  static constexpr int CHART_X = 14;
  static constexpr int CHART_Y = 119;
  static constexpr int CHART_W = 292;
  static constexpr int CHART_H = 81;
  static constexpr int PRICE_H = 60;
  static constexpr int VOLUME_Y = CHART_Y + 63;
  static constexpr int VOLUME_H = 9;

  tft.fillRect(CHART_X, CHART_Y, CHART_W, CHART_H, COL_PANEL);

  drawValueLeft(
    market.statsValid ? "$" + String(market.high24h, 0) : "--",
    48,
    105,
    58,
    12,
    COL_GREEN,
    COL_PANEL,
    1
  );

  drawValueLeft(
    market.statsValid ? "$" + String(market.low24h, 0) : "--",
    142,
    105,
    58,
    12,
    COL_RED,
    COL_PANEL,
    1
  );

  if (!market.candlesValid || market.candleCount < 2) {
    drawCenteredText(
      market.chartStatus,
      CHART_X + CHART_W / 2,
      CHART_Y + CHART_H / 2,
      COL_MUTED,
      COL_PANEL,
      1
    );
    chartNeedsDraw = false;
    return;
  }

  float minimumPrice = market.candles[0].low;
  float maximumPrice = market.candles[0].high;
  float maximumVolume = market.candles[0].volume;

  for (uint8_t index = 1; index < market.candleCount; index++) {
    minimumPrice = min(minimumPrice, market.candles[index].low);
    maximumPrice = max(maximumPrice, market.candles[index].high);
    maximumVolume = max(maximumVolume, market.candles[index].volume);
  }

  float priceRange = maximumPrice - minimumPrice;
  if (priceRange < 1.0f) {
    priceRange = 1.0f;
  }
  if (maximumVolume < 0.0001f) {
    maximumVolume = 1.0f;
  }

  auto priceToY = [&](float price) -> int {
    float normalized = (price - minimumPrice) / priceRange;
    return CHART_Y + PRICE_H - 4 - static_cast<int>(normalized * (PRICE_H - 8));
  };

  // Subtle grid.
  for (int row = 1; row < 4; row++) {
    int gridY = CHART_Y + row * PRICE_H / 4;
    tft.drawFastHLine(CHART_X, gridY, CHART_W, COL_GRID);
  }

  for (int column = 1; column < 6; column++) {
    int gridX = CHART_X + column * CHART_W / 6;
    tft.drawFastVLine(gridX, CHART_Y, PRICE_H, COL_GRID);
  }

  int spacing = CHART_W / market.candleCount;
  int candleBodyWidth = max(3, spacing / 2);

  // Volume bars and candlesticks.
  for (uint8_t index = 0; index < market.candleCount; index++) {
    const Candle& candle = market.candles[index];

    int centerX = CHART_X + index * spacing + spacing / 2;
    int highY = priceToY(candle.high);
    int lowY = priceToY(candle.low);
    int openY = priceToY(candle.open);
    int closeY = priceToY(candle.close);

    uint16_t candleColor = candle.close >= candle.open ? COL_GREEN : COL_RED;

    tft.drawFastVLine(
      centerX,
      highY,
      max(1, lowY - highY + 1),
      candleColor
    );

    int bodyTop = min(openY, closeY);
    int bodyHeight = max(2, abs(closeY - openY));

    tft.fillRect(
      centerX - candleBodyWidth / 2,
      bodyTop,
      candleBodyWidth,
      bodyHeight,
      candleColor
    );

    int volumeHeight = static_cast<int>((candle.volume / maximumVolume) * VOLUME_H);
    volumeHeight = constrain(volumeHeight, 1, VOLUME_H);

    tft.fillRect(
      centerX - candleBodyWidth / 2,
      VOLUME_Y + VOLUME_H - volumeHeight,
      candleBodyWidth,
      volumeHeight,
      candleColor
    );
  }

  // Close-price line drawn over the candles.
  for (uint8_t index = 1; index < market.candleCount; index++) {
    int previousX = CHART_X + (index - 1) * spacing + spacing / 2;
    int currentX = CHART_X + index * spacing + spacing / 2;
    int previousY = priceToY(market.candles[index - 1].close);
    int currentY = priceToY(market.candles[index].close);

    tft.drawLine(previousX, previousY, currentX, currentY, COL_CYAN);
  }

  // Current price reference line.
  if (market.tickerValid &&
      market.price >= minimumPrice &&
      market.price <= maximumPrice) {
    int currentPriceY = priceToY(market.price);

    for (int dashX = CHART_X; dashX < CHART_X + CHART_W; dashX += 8) {
      tft.drawFastHLine(dashX, currentPriceY, 4, COL_YELLOW);
    }
  }

  // Clean range labels and time labels.
  tft.fillRect(CHART_X + 2, CHART_Y + 2, 61, 10, COL_PANEL);
  drawLeftText("$" + String(maximumPrice, 0), CHART_X + 3, CHART_Y + 2, COL_MUTED, COL_PANEL, 1);

  tft.fillRect(CHART_X + 2, CHART_Y + PRICE_H - 11, 61, 10, COL_PANEL);
  drawLeftText("$" + String(minimumPrice, 0), CHART_X + 3, CHART_Y + PRICE_H - 11, COL_MUTED, COL_PANEL, 1);

  drawLeftText("-24H", CHART_X + 2, CHART_Y + 73, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("-12H", CHART_X + CHART_W / 2, CHART_Y + 77, COL_MUTED, COL_PANEL, 1);
  drawCenteredText("NOW", CHART_X + CHART_W - 15, CHART_Y + 77, COL_MUTED, COL_PANEL, 1);

  chartNeedsDraw = false;
}

// ============================================================================
// Wi-Fi setup portal
// ============================================================================

void portalSaveCallback() {
  portalSaveRequested = true;
}

void configureWifi() {
  snprintf(portalDeviceName, sizeof(portalDeviceName), "%s", config.deviceName.c_str());
  snprintf(portalBitcoinAddress, sizeof(portalBitcoinAddress), "%s", config.bitcoinAddress.c_str());
  snprintf(portalUtcOffset, sizeof(portalUtcOffset), "%d", config.utcOffsetHours);
  snprintf(portalAutoRotate, sizeof(portalAutoRotate), "%d", config.autoRotate ? 1 : 0);
  snprintf(portalPageSeconds, sizeof(portalPageSeconds), "%u", config.pageSeconds);

  tft.fillScreen(COL_BG);
  drawHeader("WI-FI SETUP", "CONNECT AND CONFIGURE");

  drawPanel(12, 48, 296, 52);
  drawCenteredText("JOIN TEMPORARY NETWORK", 160, 61, COL_MUTED, COL_PANEL, 1);
  drawCenteredText(SETUP_AP_NAME, 160, 82, COL_ORANGE, COL_PANEL, 2);

  drawPanel(12, 109, 296, 58);
  drawCenteredText(
    String("PASSWORD: ") + SETUP_AP_PASSWORD,
    160,
    127,
    COL_TEXT,
    COL_PANEL,
    2
  );
  drawCenteredText("OPEN 192.168.4.1 IF NEEDED", 160, 151, COL_CYAN, COL_PANEL, 1);

  drawPanel(12, 176, 296, 30, COL_PANEL_ALT);
  drawCenteredText("DASHBOARD WAITS UNTIL WI-FI CONNECTS", 160, 191, COL_YELLOW, COL_PANEL_ALT, 1);

  WiFiManager manager;

  WiFiManagerParameter deviceNameParameter(
    "deviceName",
    "Device name",
    portalDeviceName,
    24
  );

  WiFiManagerParameter bitcoinAddressParameter(
    "bitcoinAddress",
    "Bitcoin address (optional, display only)",
    portalBitcoinAddress,
    90
  );

  WiFiManagerParameter utcOffsetParameter(
    "utcOffset",
    "UTC offset hours",
    portalUtcOffset,
    4
  );

  WiFiManagerParameter autoRotateParameter(
    "autoRotate",
    "Auto rotate pages: 1=yes, 0=no",
    portalAutoRotate,
    1
  );

  WiFiManagerParameter pageSecondsParameter(
    "pageSeconds",
    "Seconds per page (5-60)",
    portalPageSeconds,
    3
  );

  manager.addParameter(&deviceNameParameter);
  manager.addParameter(&bitcoinAddressParameter);
  manager.addParameter(&utcOffsetParameter);
  manager.addParameter(&autoRotateParameter);
  manager.addParameter(&pageSecondsParameter);

  manager.setSaveConfigCallback(portalSaveCallback);
  manager.setTitle("CYD Bitcoin Lab Setup");
  manager.setClass("invert");
  manager.setConnectTimeout(30);
  manager.setConfigPortalTimeout(240);

  bool connected = manager.autoConnect(SETUP_AP_NAME, SETUP_AP_PASSWORD);

  if (!connected) {
    tft.fillScreen(COL_BG);
    drawCenteredText("WI-FI SETUP FAILED", 160, 105, COL_RED, COL_BG, 4);
    drawCenteredText("RESTARTING", 160, 139, COL_MUTED, COL_BG, 2);
    delay(2500);
    ESP.restart();
  }

  if (portalSaveRequested) {
    config.deviceName = deviceNameParameter.getValue();
    config.bitcoinAddress = bitcoinAddressParameter.getValue();
    config.utcOffsetHours = atoi(utcOffsetParameter.getValue());
    config.autoRotate = atoi(autoRotateParameter.getValue()) != 0;
    config.pageSeconds = constrain(atoi(pageSecondsParameter.getValue()), 5, 60);

    config.deviceName.trim();
    config.bitcoinAddress.trim();

    if (config.deviceName.isEmpty()) {
      config.deviceName = "ORANGE NODE";
    }

    saveConfiguration();
  }

  tft.fillScreen(COL_BG);
  drawCenteredText("WI-FI CONNECTED", 160, 99, COL_GREEN, COL_BG, 4);
  drawCenteredText(WiFi.SSID(), 160, 136, COL_TEXT, COL_BG, 2);
  drawCenteredText(WiFi.localIP().toString(), 160, 162, COL_CYAN, COL_BG, 2);
  delay(1200);
}

void reopenWifiSetup() {
  saveLifetimeStats();

  tft.fillScreen(COL_BG);
  drawCenteredText("OPENING WI-FI SETUP", 160, 105, COL_ORANGE, COL_BG, 4);

  WiFiManager manager;
  manager.resetSettings();

  delay(1500);
  ESP.restart();
}

// ============================================================================
// Clock and HTTPS
// ============================================================================

void configureClock() {
  long utcOffsetSeconds = static_cast<long>(config.utcOffsetHours) * 3600L;
  configTime(utcOffsetSeconds, 0, "pool.ntp.org", "time.cloudflare.com");
}

bool clockReady() {
  return time(nullptr) > 100000;
}

bool secureGet(const String& url, String& response) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(9000);
  http.useHTTP10(true);
  http.setUserAgent(String("CYD-Bitcoin-Lab/") + FIRMWARE_VERSION);

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

// ============================================================================
// Coinbase requests
// ============================================================================

bool fetchCoinbaseTicker() {
  lastTickerRefresh = millis();
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/ticker",
        response
      )) {
    market.tickerValid = false;
    market.tickerStatus = "TICKER ERROR";
    diagnostics.apiFailureCount++;
    return false;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, response);

  if (error) {
    market.tickerValid = false;
    market.tickerStatus = "TICKER JSON";
    diagnostics.apiFailureCount++;
    return false;
  }

  market.price = String(document["price"] | "0").toFloat();
  market.bid = String(document["bid"] | "0").toFloat();
  market.ask = String(document["ask"] | "0").toFloat();
  market.tickerValid = market.price > 0;
  market.tickerStatus = market.tickerValid ? "LIVE" : "NO PRICE";

  lastTickerRefresh = millis();

  if (currentPage == Page::MARKET) {
    updateMarketHeaderValues();
  }

  return market.tickerValid;
}

bool fetchCoinbaseStats() {
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/stats",
        response
      )) {
    market.statsValid = false;
    diagnostics.apiFailureCount++;
    return false;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, response);

  if (error) {
    market.statsValid = false;
    diagnostics.apiFailureCount++;
    return false;
  }

  market.open24h = String(document["open"] | "0").toFloat();
  market.high24h = String(document["high"] | "0").toFloat();
  market.low24h = String(document["low"] | "0").toFloat();
  market.volume24h = String(document["volume"] | "0").toFloat();

  if (market.open24h > 0 && market.price > 0) {
    market.change24h = ((market.price - market.open24h) / market.open24h) * 100.0f;
    market.statsValid = true;
  } else {
    market.statsValid = false;
  }

  if (currentPage == Page::MARKET) {
    updateMarketHeaderValues();
  }

  return market.statsValid;
}

bool fetchCoinbaseHourlyCandles() {
  lastMarketRefresh = millis();

  if (!clockReady()) {
    market.candlesValid = false;
    market.chartStatus = "WAITING FOR CLOCK";
    return false;
  }

  time_t endTime = time(nullptr);
  time_t startTime = endTime - 24L * 60L * 60L;

  String url =
    "https://api.exchange.coinbase.com/products/BTC-USD/candles"
    "?granularity=3600"
    "&start=" + isoUtcTime(startTime) +
    "&end=" + isoUtcTime(endTime);

  String response;

  if (!secureGet(url, response)) {
    market.candlesValid = false;
    market.chartStatus = "CANDLE API ERROR";
    diagnostics.apiFailureCount++;
    return false;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, response);

  if (error || !document.is<JsonArray>()) {
    market.candlesValid = false;
    market.chartStatus = "CANDLE JSON ERROR";
    diagnostics.apiFailureCount++;
    return false;
  }

  JsonArray rows = document.as<JsonArray>();
  int availableRows = min(static_cast<int>(rows.size()), static_cast<int>(CHART_CANDLE_COUNT));
  market.candleCount = 0;

  // Coinbase returns newest first. Reverse for oldest-to-newest charting.
  for (int destination = 0; destination < availableRows; destination++) {
    int sourceIndex = availableRows - 1 - destination;
    JsonArray row = rows[sourceIndex].as<JsonArray>();

    if (row.size() < 5) {
      continue;
    }

    Candle& candle = market.candles[market.candleCount++];
    candle.timestamp = row[0] | 0;
    candle.low = row[1] | 0.0f;
    candle.high = row[2] | 0.0f;
    candle.open = row[3] | 0.0f;
    candle.close = row[4] | 0.0f;
    candle.volume = row.size() > 5 ? row[5].as<float>() : 0.0f;
  }

  market.candlesValid = market.candleCount >= 2;
  market.chartStatus = market.candlesValid ? "LIVE 24H" : "NO CANDLE DATA";
  chartNeedsDraw = true;

  if (currentPage == Page::MARKET) {
    drawMarketChart();
  }

  return market.candlesValid;
}

// ============================================================================
// mempool.space requests
// ============================================================================

void updateNetworkStatus() {
  if (network.heightValid && network.feesValid) {
    network.status = "API LIVE";
  } else if (network.heightValid || network.feesValid) {
    network.status = "PARTIAL DATA";
  } else {
    network.status = "API ERROR";
  }
}

bool fetchBlockHeight() {
  String response;

  if (!secureGet("https://mempool.space/api/blocks/tip/height", response)) {
    network.heightValid = false;
    diagnostics.apiFailureCount++;
    updateNetworkStatus();
    return false;
  }

  response.trim();
  network.blockHeight = static_cast<uint32_t>(response.toInt());
  network.heightValid = network.blockHeight > 0;

  if (!network.heightValid) {
    diagnostics.apiFailureCount++;
  }

  updateNetworkStatus();

  if (currentPage == Page::NETWORK) {
    updateNetworkValues();
  }
  if (currentPage == Page::MINER) {
    updateMinerValues();
  }

  return network.heightValid;
}

bool fetchRecommendedFees() {
  String response;

  if (!secureGet("https://mempool.space/api/v1/fees/recommended", response)) {
    network.feesValid = false;
    diagnostics.apiFailureCount++;
    updateNetworkStatus();
    return false;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, response);

  if (error) {
    network.feesValid = false;
    diagnostics.apiFailureCount++;
    updateNetworkStatus();
    return false;
  }

  network.fastestFee = document["fastestFee"] | 0;
  network.halfHourFee = document["halfHourFee"] | 0;
  network.hourFee = document["hourFee"] | 0;
  network.feesValid = true;
  updateNetworkStatus();

  if (currentPage == Page::NETWORK) {
    updateNetworkValues();
  }

  return true;
}

// ============================================================================
// Stable, midstate-optimized local double-SHA256 benchmark
// ============================================================================

uint8_t countLeadingZeroBits(const uint8_t hash[32]) {
  uint8_t bits = 0;

  for (uint8_t index = 0; index < 32; index++) {
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

void rebuildHeaderMidstate() {
  mbedtls_sha256_starts_ret(&headerMidstate, 0);
  mbedtls_sha256_update_ret(&headerMidstate, benchmarkHeader, 64);
}

void initializeBenchmark() {
  for (uint8_t index = 0; index < 80; index++) {
    benchmarkHeader[index] = static_cast<uint8_t>(esp_random() & 0xFF);
  }

  mbedtls_sha256_init(&headerMidstate);
  mbedtls_sha256_init(&firstHashContext);
  mbedtls_sha256_init(&secondHashContext);

  rebuildHeaderMidstate();
}

void runHashBatch() {
  uint8_t firstHash[32];
  uint8_t finalHash[32];

  for (uint16_t index = 0; index < HASH_BATCH_SIZE; index++) {
    benchmarkHeader[76] = currentNonce & 0xFF;
    benchmarkHeader[77] = (currentNonce >> 8) & 0xFF;
    benchmarkHeader[78] = (currentNonce >> 16) & 0xFF;
    benchmarkHeader[79] = (currentNonce >> 24) & 0xFF;

    // Reuse the SHA-256 state after hashing the constant first 64 bytes.
    mbedtls_sha256_clone(&firstHashContext, &headerMidstate);
    mbedtls_sha256_update_ret(&firstHashContext, benchmarkHeader + 64, 16);
    mbedtls_sha256_finish_ret(&firstHashContext, firstHash);

    mbedtls_sha256_starts_ret(&secondHashContext, 0);
    mbedtls_sha256_update_ret(&secondHashContext, firstHash, 32);
    mbedtls_sha256_finish_ret(&secondHashContext, finalHash);

    uint8_t leadingBits = countLeadingZeroBits(finalHash);
    if (leadingBits > sessionBestLeadingBits) {
      sessionBestLeadingBits = leadingBits;
    }

    // A factual local share target: first 16 result bits are zero.
    if (finalHash[0] == 0 && finalHash[1] == 0) {
      sessionLocalShares16++;
    }

    currentNonce++;
    sessionHashes++;
    hashesInWindow++;
  }
}

void sampleHashrate() {
  currentHashrateKHs = hashesInWindow / (VALUE_REFRESH_MS / 1000.0f) / 1000.0f;
  hashesInWindow = 0;

  if (smoothedHashrateKHs < 0.01f) {
    smoothedHashrateKHs = currentHashrateKHs;
  } else {
    smoothedHashrateKHs = smoothedHashrateKHs * 0.72f + currentHashrateKHs * 0.28f;
  }
}

// ============================================================================
// Navigation and scheduled work
// ============================================================================

void changePage(Page nextPage) {
  currentPage = nextPage;
  pageNeedsFullDraw = true;
  lastPageChange = millis();
}

void nextPage() {
  uint8_t next = (static_cast<uint8_t>(currentPage) + 1) % PAGE_COUNT;
  changePage(static_cast<Page>(next));
}

void handleBootButton() {
  bool down = digitalRead(PIN_BOOT) == LOW;

  if (down && !buttonWasDown) {
    buttonWasDown = true;
    buttonDownMillis = millis();
  }

  if (!down && buttonWasDown) {
    uint32_t heldMillis = millis() - buttonDownMillis;
    buttonWasDown = false;

    if (heldMillis >= LONG_BUTTON_PRESS_MS) {
      reopenWifiSetup();
    } else {
      nextPage();
    }
  }
}

void maintainPageRotation() {
  if (!config.autoRotate) {
    return;
  }

  uint32_t pageInterval = static_cast<uint32_t>(config.pageSeconds) * 1000UL;
  if (millis() - lastPageChange >= pageInterval) {
    nextPage();
  }
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWifiReconnect < WIFI_RECONNECT_MS) {
    return;
  }

  lastWifiReconnect = millis();
  diagnostics.wifiReconnectCount++;
  WiFi.reconnect();
}

void maintainStartupFetches() {
  if (WiFi.status() != WL_CONNECTED || millis() < nextStartupFetchMillis) {
    return;
  }

  switch (startupFetchStep) {
    case StartupFetchStep::WAITING:
      startupFetchStep = StartupFetchStep::BLOCK_HEIGHT;
      nextStartupFetchMillis = millis() + 250;
      break;

    case StartupFetchStep::BLOCK_HEIGHT:
      fetchBlockHeight();
      startupFetchStep = StartupFetchStep::FEES;
      nextStartupFetchMillis = millis() + 700;
      break;

    case StartupFetchStep::FEES:
      fetchRecommendedFees();
      lastNetworkRefresh = millis();
      startupFetchStep = StartupFetchStep::TICKER;
      nextStartupFetchMillis = millis() + 700;
      break;

    case StartupFetchStep::TICKER:
      fetchCoinbaseTicker();
      startupFetchStep = StartupFetchStep::STATS;
      nextStartupFetchMillis = millis() + 700;
      break;

    case StartupFetchStep::STATS:
      fetchCoinbaseStats();
      startupFetchStep = StartupFetchStep::CANDLES;
      nextStartupFetchMillis = millis() + 1000;
      break;

    case StartupFetchStep::CANDLES:
      if (clockReady()) {
        fetchCoinbaseHourlyCandles();
        lastMarketRefresh = millis();
        startupFetchStep = StartupFetchStep::COMPLETE;
      } else {
        nextStartupFetchMillis = millis() + 1000;
      }
      break;

    case StartupFetchStep::COMPLETE:
      break;
  }
}

void maintainScheduledData() {
  if (WiFi.status() != WL_CONNECTED || startupFetchStep != StartupFetchStep::COMPLETE) {
    return;
  }

  // Execute at most one HTTPS request per scheduler step.
  if (pendingApiTask != PendingApiTask::NONE) {
    if (millis() < pendingApiMillis) {
      return;
    }

    switch (pendingApiTask) {
      case PendingApiTask::MARKET_STATS:
        fetchCoinbaseStats();
        pendingApiTask = PendingApiTask::MARKET_CANDLES;
        pendingApiMillis = millis() + 800;
        return;

      case PendingApiTask::MARKET_CANDLES:
        fetchCoinbaseHourlyCandles();
        pendingApiTask = PendingApiTask::NONE;
        return;

      case PendingApiTask::NETWORK_HEIGHT:
        fetchBlockHeight();
        pendingApiTask = PendingApiTask::NETWORK_FEES;
        pendingApiMillis = millis() + 800;
        return;

      case PendingApiTask::NETWORK_FEES:
        fetchRecommendedFees();
        pendingApiTask = PendingApiTask::NONE;
        return;

      case PendingApiTask::NONE:
        break;
    }
  }

  if (millis() - lastTickerRefresh >= TICKER_REFRESH_MS) {
    fetchCoinbaseTicker();
    return;
  }

  if (millis() - lastMarketRefresh >= MARKET_REFRESH_MS) {
    lastMarketRefresh = millis();
    pendingApiTask = PendingApiTask::MARKET_STATS;
    pendingApiMillis = millis();
    return;
  }

  if (millis() - lastNetworkRefresh >= NETWORK_REFRESH_MS) {
    lastNetworkRefresh = millis();
    pendingApiTask = PendingApiTask::NETWORK_HEIGHT;
    pendingApiMillis = millis();
  }
}

void maintainPersistence() {
  if (millis() - lastSaveRefresh >= SAVE_REFRESH_MS) {
    saveLifetimeStats();
  }
}

// ============================================================================
// Arduino setup and loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  diagnostics.resetReason = esp_reset_reason();
  diagnostics.minimumFreeHeap = ESP.getFreeHeap();

  loadPersistentData();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  configureWifi();
  configureClock();
  initializeBenchmark();

  currentPage = Page::MINER;
  lastPageChange = millis();
  lastSaveRefresh = millis();
  nextStartupFetchMillis = millis() + 1500;

  drawCurrentPageLayout();
  updateCurrentPageValues();
}

void loop() {
  // Cooperative small batches keep the UI and Wi-Fi responsive.
  runHashBatch();

  handleBootButton();
  maintainPageRotation();
  maintainWifi();
  maintainStartupFetches();
  maintainScheduledData();
  maintainPersistence();

  // The complete screen is redrawn only after a page change.
  if (pageNeedsFullDraw) {
    drawCurrentPageLayout();
    updateCurrentPageValues();

    if (currentPage == Page::MARKET) {
      drawMarketChart();
    }
  }

  // Once per second, only the changing value rectangles are updated.
  if (millis() - lastValueRefresh >= VALUE_REFRESH_MS) {
    lastValueRefresh = millis();
    sampleHashrate();

    if (currentPage == Page::MINER || currentPage == Page::DEVICE) {
      updateCurrentPageValues();
    }
  }

  if (currentPage == Page::MARKET && chartNeedsDraw) {
    drawMarketChart();
  }

  delay(1);
}
