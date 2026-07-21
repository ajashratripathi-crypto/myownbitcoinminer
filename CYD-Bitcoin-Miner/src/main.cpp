
/*
  CYD Bitcoin Lab V6 — MAIN.CPP ONLY
  Replace only:
    CYD-Bitcoin-Miner/src/main.cpp

  Target board:
    ESP32-2432S028R
    ESP32-WROOM-32E
    ILI9341 display

  Existing platformio.ini must already include:
    bodmer/TFT_eSPI
    bblanchon/ArduinoJson
    tzapu/WiFiManager

  Features:
    - First-run Wi-Fi setup portal
    - Optional device name and Bitcoin address
    - Stable local double-SHA256 benchmark
    - Coinbase BTC-USD ticker, stats and daily candles
    - mempool.space block height and recommended fees
    - Lifetime hashes, runtime, boot count, best score saved in Preferences
    - Automatic page change every 10 seconds by default
    - Full screen redraw only on page change
    - Only changing values update every second
    - Short BOOT press = next page
    - Hold BOOT 3 seconds = reset Wi-Fi and reopen setup

  This firmware does NOT:
    - connect to a mining pool
    - submit shares
    - move Bitcoin
    - claim earnings
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

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

// -----------------------------------------------------------------------------
// Theme
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
// Constants
// -----------------------------------------------------------------------------

static const char* FW_VERSION = "6.0.0";
static const char* AP_NAME = "CYD-Miner-Setup";
static const char* AP_PASSWORD = "bitcoin123";

static constexpr uint32_t VALUE_UPDATE_MS = 1000;
static constexpr uint32_t MARKET_UPDATE_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_UPDATE_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t SAVE_UPDATE_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t WIFI_RECONNECT_MS = 15UL * 1000UL;
static constexpr uint32_t BUTTON_LONG_MS = 3000;
static constexpr uint16_t HASH_BATCH = 64;
static constexpr uint8_t MAX_CANDLES = 14;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

struct AppConfig {
  String deviceName = "ORANGE NODE";
  String bitcoinAddress = "";
  int utcOffsetHours = -4;
  bool autoRotate = true;
  uint16_t pageSeconds = 10;
};

AppConfig config;
Preferences prefs;

char portalDeviceName[25] = "ORANGE NODE";
char portalBitcoinAddress[91] = "";
char portalUtcOffset[5] = "-4";
char portalAutoRotate[2] = "1";
char portalPageSeconds[4] = "10";

bool savePortalConfig = false;

// -----------------------------------------------------------------------------
// Lifetime stats
// -----------------------------------------------------------------------------

struct LifetimeStats {
  uint64_t hashes = 0;
  uint64_t runtimeSeconds = 0;
  uint32_t boots = 0;
  uint8_t bestZeroBits = 0;
};

LifetimeStats lifetime;

// -----------------------------------------------------------------------------
// Pages and timing
// -----------------------------------------------------------------------------

enum class Page : uint8_t {
  HOME = 0,
  MARKET = 1,
  NETWORK = 2,
  DEVICE = 3,
  SETTINGS = 4
};

static constexpr uint8_t PAGE_COUNT = 5;
Page currentPage = Page::HOME;

bool pageNeedsDraw = true;
bool marketChartNeedsDraw = true;

uint32_t bootMs = 0;
uint32_t lastValueMs = 0;
uint32_t lastPageMs = 0;
uint32_t lastMarketMs = 0;
uint32_t lastNetworkMs = 0;
uint32_t lastSaveMs = 0;
uint32_t lastWifiReconnectMs = 0;
uint32_t buttonDownMs = 0;
bool buttonWasDown = false;

// -----------------------------------------------------------------------------
// Hash benchmark
// -----------------------------------------------------------------------------

uint8_t blockHeader[80];
uint32_t nonceValue = 0;
uint64_t sessionHashes = 0;
uint32_t hashesThisSecond = 0;
uint8_t sessionBestZeroBits = 0;

float currentKHs = 0;
float smoothKHs = 0;

mbedtls_sha256_context shaContext;

// -----------------------------------------------------------------------------
// Coinbase market data
// -----------------------------------------------------------------------------

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

  Candle candles[MAX_CANDLES];
  uint8_t candleCount = 0;

  uint32_t lastSuccessEpoch = 0;
  String error = "Waiting for Coinbase";
};

MarketData market;

// -----------------------------------------------------------------------------
// Bitcoin network data
// -----------------------------------------------------------------------------

struct NetworkData {
  bool heightValid = false;
  bool feesValid = false;

  uint32_t blockHeight = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;

  uint32_t lastSuccessEpoch = 0;
  String error = "Waiting for mempool.space";
};

NetworkData network;

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

struct Diagnostics {
  uint32_t minFreeHeap = UINT32_MAX;
  uint32_t wifiReconnects = 0;
  uint32_t marketFailures = 0;
  uint32_t networkFailures = 0;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
};

Diagnostics diagnostics;

// -----------------------------------------------------------------------------
// Utility
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
      "%lud %02lu:%02lu:%02lu",
      (unsigned long)days,
      (unsigned long)hours,
      (unsigned long)minutes,
      (unsigned long)remaining
    );
  } else {
    snprintf(
      buffer,
      sizeof(buffer),
      "%02lu:%02lu:%02lu",
      (unsigned long)hours,
      (unsigned long)minutes,
      (unsigned long)remaining
    );
  }

  return String(buffer);
}

String resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_EXT: return "External";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Crash";
    case ESP_RST_INT_WDT: return "Interrupt WDT";
    case ESP_RST_TASK_WDT: return "Task WDT";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    default: return "Unknown";
  }
}

String abbreviatedAddress(const String& address) {
  if (address.length() == 0) {
    return "Not set";
  }

  if (address.length() <= 20) {
    return address;
  }

  return address.substring(0, 9) + "..." +
         address.substring(address.length() - 7);
}

String isoUtcTime(time_t value) {
  struct tm timeInfo;
  gmtime_r(&value, &timeInfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);

  return String(buffer);
}

// -----------------------------------------------------------------------------
// Preferences
// -----------------------------------------------------------------------------

void loadPreferences() {
  prefs.begin("btc-lab", true);

  config.deviceName = prefs.getString("name", "ORANGE NODE");
  config.bitcoinAddress = prefs.getString("btc", "");
  config.utcOffsetHours = prefs.getInt("utc", -4);
  config.autoRotate = prefs.getBool("rotate", true);
  config.pageSeconds = prefs.getUShort("pageSec", 10);

  lifetime.hashes = prefs.getULong64("lifeHash", 0);
  lifetime.runtimeSeconds = prefs.getULong64("lifeTime", 0);
  lifetime.boots = prefs.getUInt("boots", 0);
  lifetime.bestZeroBits = prefs.getUChar("bestBits", 0);

  prefs.end();

  config.pageSeconds = constrain(config.pageSeconds, 5, 60);
  lifetime.boots++;
}

void saveConfiguration() {
  prefs.begin("btc-lab", false);

  prefs.putString("name", config.deviceName);
  prefs.putString("btc", config.bitcoinAddress);
  prefs.putInt("utc", config.utcOffsetHours);
  prefs.putBool("rotate", config.autoRotate);
  prefs.putUShort("pageSec", config.pageSeconds);

  prefs.end();
}

void saveLifetimeStats() {
  prefs.begin("btc-lab", false);

  prefs.putULong64("lifeHash", lifetime.hashes + sessionHashes);
  prefs.putULong64(
    "lifeTime",
    lifetime.runtimeSeconds + ((millis() - bootMs) / 1000ULL)
  );

  prefs.putUInt("boots", lifetime.boots);

  prefs.putUChar(
    "bestBits",
    max(lifetime.bestZeroBits, sessionBestZeroBits)
  );

  prefs.end();

  lastSaveMs = millis();
}

// -----------------------------------------------------------------------------
// UI helpers
// -----------------------------------------------------------------------------

void panel(int x, int y, int w, int h, uint16_t fill = C_PANEL) {
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.drawRoundRect(x, y, w, h, 8, C_BORDER);
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

void centerText(
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

void spriteValue(
  const String& value,
  int x,
  int y,
  int width,
  int height,
  uint16_t foreground,
  uint16_t background,
  uint8_t font,
  uint8_t datum = MC_DATUM
) {
  sprite.deleteSprite();

  if (!sprite.createSprite(width, height)) {
    return;
  }

  sprite.fillSprite(background);
  sprite.setTextDatum(datum);
  sprite.setTextColor(foreground, background);

  int drawX = datum == MC_DATUM ? width / 2 : 0;
  int drawY = datum == MC_DATUM ? height / 2 : 0;

  sprite.drawString(value, drawX, drawY, font);
  sprite.pushSprite(x, y);
  sprite.deleteSprite();
}

void drawHeader(const String& title, const String& subtitle) {
  tft.fillRect(0, 0, 320, 34, C_PANEL);
  tft.drawFastHLine(0, 33, 320, C_BORDER);

  tft.fillCircle(17, 17, 11, C_ORANGE);
  centerText("B", 17, 17, C_TEXT, C_ORANGE, 2);

  leftText(title, 36, 3, C_TEXT, C_PANEL, 2);
  leftText(subtitle, 36, 20, C_MUTED, C_PANEL, 1);

  bool online = WiFi.status() == WL_CONNECTED;

  tft.fillCircle(301, 10, 4, online ? C_GREEN : C_RED);

  centerText(
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
    "HOME", "BTC", "NET", "DEV", "SET"
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

    centerText(
      names[index],
      index * itemWidth + 32,
      228,
      selected ? C_ORANGE : C_MUTED,
      selected ? C_PANEL2 : C_PANEL,
      1
    );
  }
}

void statBox(int x, int y, int w, int h, const String& label) {
  panel(x, y, w, h);

  centerText(
    label,
    x + w / 2,
    y + 11,
    C_MUTED,
    C_PANEL,
    1
  );
}

// -----------------------------------------------------------------------------
// Full layouts
// -----------------------------------------------------------------------------

void drawHomeLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN LAB", config.deviceName);

  panel(8, 42, 304, 67);
  leftText("LOCAL DOUBLE-SHA256", 18, 50, C_MUTED, C_PANEL, 1);

  statBox(8, 118, 96, 88, "SESSION HASHES");
  statBox(112, 118, 96, 88, "BEST ZERO BITS");
  statBox(216, 118, 96, 88, "UPTIME");

  drawFooter();
}

void drawMarketLayout() {
  tft.fillScreen(C_BG);
  drawHeader("COINBASE BTC-USD", "REAL MARKET DATA");

  panel(8, 42, 304, 54);
  leftText("LAST PRICE", 18, 50, C_MUTED, C_PANEL, 1);

  panel(8, 104, 304, 102);

  drawFooter();
}

void drawNetworkLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN NETWORK", "MEMPOOL.SPACE");

  statBox(8, 43, 304, 48, "BLOCK HEIGHT");

  statBox(8, 100, 96, 105, "FASTEST");
  statBox(112, 100, 96, 105, "30 MIN");
  statBox(216, 100, 96, 105, "1 HOUR");

  drawFooter();
}

void drawDeviceLayout() {
  tft.fillScreen(C_BG);
  drawHeader("DEVICE HEALTH", "LIVE DIAGNOSTICS");

  statBox(8, 43, 148, 66, "FREE HEAP");
  statBox(164, 43, 148, 66, "MIN HEAP");
  statBox(8, 117, 148, 66, "WI-FI SIGNAL");
  statBox(164, 117, 148, 66, "CPU");

  leftText("Reset:", 14, 191, C_MUTED, C_BG, 1);
  leftText(resetReasonText(diagnostics.resetReason), 56, 191, C_TEXT, C_BG, 1);

  leftText("Reconnects:", 176, 191, C_MUTED, C_BG, 1);

  drawFooter();
}

void drawSettingsLayout() {
  tft.fillScreen(C_BG);
  drawHeader("SETTINGS", FW_VERSION);

  panel(8, 43, 304, 58);
  leftText("DEVICE", 18, 52, C_MUTED, C_PANEL, 1);
  leftText(config.deviceName, 18, 68, C_TEXT, C_PANEL, 2);

  panel(8, 109, 304, 58);
  leftText("BITCOIN ADDRESS (DISPLAY ONLY)", 18, 118, C_MUTED, C_PANEL, 1);
  leftText(abbreviatedAddress(config.bitcoinAddress), 18, 135, C_ORANGE, C_PANEL, 2);

  panel(8, 175, 304, 31);

  centerText(
    config.autoRotate
      ? "Auto pages: ON / " + String(config.pageSeconds) + " sec"
      : "Auto pages: OFF",
    160,
    190,
    C_CYAN,
    C_PANEL,
    1
  );

  drawFooter();
}

void drawCurrentLayout() {
  switch (currentPage) {
    case Page::HOME:
      drawHomeLayout();
      break;

    case Page::MARKET:
      drawMarketLayout();
      break;

    case Page::NETWORK:
      drawNetworkLayout();
      break;

    case Page::DEVICE:
      drawDeviceLayout();
      break;

    case Page::SETTINGS:
      drawSettingsLayout();
      break;
  }

  pageNeedsDraw = false;
}

// -----------------------------------------------------------------------------
// Number-only updates
// -----------------------------------------------------------------------------

void updateHomeValues() {
  spriteValue(
    String(smoothKHs, 2),
    18,
    65,
    178,
    35,
    C_ORANGE,
    C_PANEL,
    4,
    ML_DATUM
  );

  spriteValue(
    "kH/s",
    204,
    74,
    70,
    25,
    C_TEXT,
    C_PANEL,
    2,
    ML_DATUM
  );

  spriteValue(
    compactNumber(sessionHashes),
    17,
    145,
    78,
    42,
    C_TEXT,
    C_PANEL,
    2
  );

  spriteValue(
    String(sessionBestZeroBits),
    121,
    145,
    78,
    42,
    C_YELLOW,
    C_PANEL,
    4
  );

  spriteValue(
    uptimeText((millis() - bootMs) / 1000ULL),
    225,
    145,
    78,
    42,
    C_CYAN,
    C_PANEL,
    2
  );
}

void updateMarketValues() {
  if (market.tickerValid) {
    spriteValue(
      "$" + String(market.price, 2),
      18,
      65,
      190,
      24,
      C_ORANGE,
      C_PANEL,
      4,
      ML_DATUM
    );
  } else {
    spriteValue(
      "Unavailable",
      18,
      65,
      190,
      24,
      C_RED,
      C_PANEL,
      2,
      ML_DATUM
    );
  }

  uint16_t changeColor =
      market.change24h >= 0 ? C_GREEN : C_RED;

  String changeText =
      market.statsValid
      ? String(market.change24h >= 0 ? "+" : "") +
        String(market.change24h, 2) + "%"
      : "--";

  spriteValue(
    changeText,
    222,
    62,
    80,
    28,
    changeColor,
    C_PANEL,
    2
  );
}

void updateNetworkValues() {
  spriteValue(
    network.heightValid ? String(network.blockHeight) : "--",
    20,
    66,
    280,
    18,
    C_PURPLE,
    C_PANEL,
    2
  );

  spriteValue(
    network.feesValid ? String(network.fastestFee) : "--",
    17,
    130,
    78,
    42,
    C_ORANGE,
    C_PANEL,
    4
  );

  spriteValue(
    "sat/vB",
    17,
    175,
    78,
    16,
    C_MUTED,
    C_PANEL,
    1
  );

  spriteValue(
    network.feesValid ? String(network.halfHourFee) : "--",
    121,
    130,
    78,
    42,
    C_YELLOW,
    C_PANEL,
    4
  );

  spriteValue(
    "sat/vB",
    121,
    175,
    78,
    16,
    C_MUTED,
    C_PANEL,
    1
  );

  spriteValue(
    network.feesValid ? String(network.hourFee) : "--",
    225,
    130,
    78,
    42,
    C_CYAN,
    C_PANEL,
    4
  );

  spriteValue(
    "sat/vB",
    225,
    175,
    78,
    16,
    C_MUTED,
    C_PANEL,
    1
  );
}

void updateDeviceValues() {
  diagnostics.minFreeHeap =
      min(diagnostics.minFreeHeap, ESP.getFreeHeap());

  spriteValue(
    compactNumber(ESP.getFreeHeap()),
    18,
    70,
    128,
    28,
    C_CYAN,
    C_PANEL,
    2
  );

  spriteValue(
    compactNumber(diagnostics.minFreeHeap),
    174,
    70,
    128,
    28,
    C_YELLOW,
    C_PANEL,
    2
  );

  spriteValue(
    WiFi.status() == WL_CONNECTED
      ? String(WiFi.RSSI()) + " dBm"
      : "Offline",
    18,
    144,
    128,
    28,
    WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED,
    C_PANEL,
    2
  );

  spriteValue(
    String(ESP.getCpuFreqMHz()) + " MHz",
    174,
    144,
    128,
    28,
    C_ORANGE,
    C_PANEL,
    2
  );

  spriteValue(
    String(diagnostics.wifiReconnects),
    249,
    186,
    54,
    15,
    C_TEXT,
    C_BG,
    1
  );
}

void updateCurrentValues() {
  switch (currentPage) {
    case Page::HOME:
      updateHomeValues();
      break;

    case Page::MARKET:
      updateMarketValues();
      break;

    case Page::NETWORK:
      updateNetworkValues();
      break;

    case Page::DEVICE:
      updateDeviceValues();
      break;

    case Page::SETTINGS:
      break;
  }
}

// -----------------------------------------------------------------------------
// Real candlestick chart
// -----------------------------------------------------------------------------

void drawRealCandles() {
  int chartX = 17;
  int chartY = 118;
  int chartW = 286;
  int chartH = 78;

  tft.fillRect(chartX, chartY, chartW, chartH, C_PANEL);

  if (!market.candlesValid || market.candleCount < 2) {
    centerText(
      market.error,
      160,
      157,
      C_MUTED,
      C_PANEL,
      1
    );
    return;
  }

  float minimum = market.candles[0].low;
  float maximum = market.candles[0].high;

  for (uint8_t index = 1; index < market.candleCount; index++) {
    minimum = min(minimum, market.candles[index].low);
    maximum = max(maximum, market.candles[index].high);
  }

  float range = maximum - minimum;
  if (range < 1.0f) {
    range = 1.0f;
  }

  for (int line = 1; line < 4; line++) {
    int y = chartY + line * chartH / 4;
    tft.drawFastHLine(chartX, y, chartW, C_PANEL2);
  }

  int spacing = chartW / market.candleCount;
  int bodyWidth = max(4, spacing / 2);

  for (uint8_t index = 0; index < market.candleCount; index++) {
    const Candle& candle = market.candles[index];

    auto priceY = [&](float price) {
      return chartY + chartH - 4 -
             static_cast<int>(
               ((price - minimum) / range) *
               (chartH - 8)
             );
    };

    int centerX =
        chartX + index * spacing + spacing / 2;

    int highY = priceY(candle.high);
    int lowY = priceY(candle.low);
    int openY = priceY(candle.open);
    int closeY = priceY(candle.close);

    uint16_t color =
        candle.close >= candle.open
        ? C_GREEN
        : C_RED;

    tft.drawFastVLine(
      centerX,
      highY,
      max(1, lowY - highY),
      color
    );

    int bodyTop = min(openY, closeY);
    int bodyHeight = max(3, abs(closeY - openY));

    tft.fillRect(
      centerX - bodyWidth / 2,
      bodyTop,
      bodyWidth,
      bodyHeight,
      color
    );
  }

  marketChartNeedsDraw = false;
}

// -----------------------------------------------------------------------------
// Wi-Fi portal
// -----------------------------------------------------------------------------

void savePortalCallback() {
  savePortalConfig = true;
}

void configureWifi() {
  snprintf(
    portalDeviceName,
    sizeof(portalDeviceName),
    "%s",
    config.deviceName.c_str()
  );

  snprintf(
    portalBitcoinAddress,
    sizeof(portalBitcoinAddress),
    "%s",
    config.bitcoinAddress.c_str()
  );

  snprintf(
    portalUtcOffset,
    sizeof(portalUtcOffset),
    "%d",
    config.utcOffsetHours
  );

  snprintf(
    portalAutoRotate,
    sizeof(portalAutoRotate),
    "%d",
    config.autoRotate ? 1 : 0
  );

  snprintf(
    portalPageSeconds,
    sizeof(portalPageSeconds),
    "%u",
    config.pageSeconds
  );

  tft.fillScreen(C_BG);
  drawHeader("WI-FI SETUP", "CONNECT AND CONFIGURE");

  panel(12, 48, 296, 52);
  centerText("Join this temporary network:", 160, 62, C_MUTED, C_PANEL, 1);
  centerText(AP_NAME, 160, 82, C_ORANGE, C_PANEL, 2);

  panel(12, 109, 296, 58);

  centerText(
    String("Password: ") + AP_PASSWORD,
    160,
    127,
    C_TEXT,
    C_PANEL,
    2
  );

  centerText(
    "Open 192.168.4.1 if needed",
    160,
    151,
    C_CYAN,
    C_PANEL,
    1
  );

  WiFiManager manager;

  WiFiManagerParameter pDeviceName(
    "deviceName",
    "Device name",
    portalDeviceName,
    24
  );

  WiFiManagerParameter pBitcoinAddress(
    "bitcoinAddress",
    "Bitcoin address (optional, display only)",
    portalBitcoinAddress,
    90
  );

  WiFiManagerParameter pUtcOffset(
    "utcOffset",
    "UTC offset hours",
    portalUtcOffset,
    4
  );

  WiFiManagerParameter pAutoRotate(
    "autoRotate",
    "Auto rotate pages: 1=yes, 0=no",
    portalAutoRotate,
    1
  );

  WiFiManagerParameter pPageSeconds(
    "pageSeconds",
    "Seconds per page (5-60)",
    portalPageSeconds,
    3
  );

  manager.addParameter(&pDeviceName);
  manager.addParameter(&pBitcoinAddress);
  manager.addParameter(&pUtcOffset);
  manager.addParameter(&pAutoRotate);
  manager.addParameter(&pPageSeconds);

  manager.setSaveConfigCallback(savePortalCallback);
  manager.setTitle("CYD Bitcoin Lab Setup");
  manager.setClass("invert");
  manager.setConnectTimeout(30);
  manager.setConfigPortalTimeout(240);

  bool connected =
      manager.autoConnect(AP_NAME, AP_PASSWORD);

  if (!connected) {
    tft.fillScreen(C_BG);

    centerText(
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

  if (savePortalConfig) {
    config.deviceName = pDeviceName.getValue();
    config.bitcoinAddress = pBitcoinAddress.getValue();
    config.utcOffsetHours = atoi(pUtcOffset.getValue());
    config.autoRotate = atoi(pAutoRotate.getValue()) != 0;
    config.pageSeconds =
        constrain(atoi(pPageSeconds.getValue()), 5, 60);

    config.deviceName.trim();
    config.bitcoinAddress.trim();

    if (config.deviceName.length() == 0) {
      config.deviceName = "ORANGE NODE";
    }

    saveConfiguration();
  }

  tft.fillScreen(C_BG);

  centerText(
    "WI-FI CONNECTED",
    160,
    98,
    C_GREEN,
    C_BG,
    4
  );

  centerText(WiFi.SSID(), 160, 135, C_TEXT, C_BG, 2);
  centerText(WiFi.localIP().toString(), 160, 161, C_CYAN, C_BG, 2);

  delay(1200);
}

void reopenConfiguration() {
  saveLifetimeStats();

  tft.fillScreen(C_BG);
  centerText("OPENING SETUP", 160, 105, C_ORANGE, C_BG, 4);

  WiFiManager manager;
  manager.resetSettings();

  delay(1500);
  ESP.restart();
}

// -----------------------------------------------------------------------------
// Clock
// -----------------------------------------------------------------------------

void configureClock() {
  long utcOffsetSeconds =
      static_cast<long>(config.utcOffsetHours) * 3600L;

  configTime(
    utcOffsetSeconds,
    0,
    "pool.ntp.org",
    "time.cloudflare.com"
  );
}

// -----------------------------------------------------------------------------
// HTTPS
// -----------------------------------------------------------------------------

bool secureGet(const String& url, String& response) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  http.setUserAgent(String("CYD-Bitcoin-Lab/") + FW_VERSION);

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

// -----------------------------------------------------------------------------
// Coinbase
// -----------------------------------------------------------------------------

bool fetchCoinbaseTicker() {
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/ticker",
        response
      )) {
    market.error = "Coinbase ticker failed";
    return false;
  }

  DynamicJsonDocument document(2048);

  if (deserializeJson(document, response)) {
    market.error = "Ticker JSON error";
    return false;
  }

  market.price =
      String(document["price"] | "0").toFloat();

  market.bid =
      String(document["bid"] | "0").toFloat();

  market.ask =
      String(document["ask"] | "0").toFloat();

  market.tickerValid = market.price > 0;

  return market.tickerValid;
}

bool fetchCoinbaseStats() {
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/stats",
        response
      )) {
    market.error = "Coinbase stats failed";
    return false;
  }

  DynamicJsonDocument document(3072);

  if (deserializeJson(document, response)) {
    market.error = "Stats JSON error";
    return false;
  }

  market.open24h =
      String(document["open"] | "0").toFloat();

  market.high24h =
      String(document["high"] | "0").toFloat();

  market.low24h =
      String(document["low"] | "0").toFloat();

  market.volume24h =
      String(document["volume"] | "0").toFloat();

  if (market.open24h > 0 && market.price > 0) {
    market.change24h =
        ((market.price - market.open24h) /
         market.open24h) *
        100.0f;

    market.statsValid = true;
  }

  return market.statsValid;
}

bool fetchCoinbaseCandles() {
  time_t now = time(nullptr);

  if (now < 100000) {
    market.error = "Clock not ready";
    return false;
  }

  time_t startTime =
      now - static_cast<time_t>(MAX_CANDLES) * 86400L;

  String url =
      "https://api.exchange.coinbase.com/products/BTC-USD/candles"
      "?granularity=86400"
      "&start=" + isoUtcTime(startTime) +
      "&end=" + isoUtcTime(now);

  String response;

  if (!secureGet(url, response)) {
    market.error = "Coinbase candles failed";
    return false;
  }

  DynamicJsonDocument document(12288);

  if (deserializeJson(document, response) ||
      !document.is<JsonArray>()) {
    market.error = "Candle JSON error";
    return false;
  }

  JsonArray rows = document.as<JsonArray>();

  uint8_t available =
      min(
        static_cast<int>(rows.size()),
        static_cast<int>(MAX_CANDLES)
      );

  market.candleCount = 0;

  for (int destination = 0;
       destination < available;
       destination++) {

    int sourceIndex =
        available - 1 - destination;

    JsonArray row =
        rows[sourceIndex].as<JsonArray>();

    if (row.size() < 5) {
      continue;
    }

    Candle& candle =
        market.candles[market.candleCount++];

    candle.timestamp = row[0] | 0;
    candle.low = row[1] | 0.0f;
    candle.high = row[2] | 0.0f;
    candle.open = row[3] | 0.0f;
    candle.close = row[4] | 0.0f;
    candle.volume =
        row.size() > 5
        ? row[5].as<float>()
        : 0.0f;
  }

  market.candlesValid =
      market.candleCount >= 2;

  return market.candlesValid;
}

void fetchMarketData() {
  bool tickerOk = fetchCoinbaseTicker();
  bool statsOk = fetchCoinbaseStats();
  bool candlesOk = fetchCoinbaseCandles();

  if (tickerOk || statsOk || candlesOk) {
    market.lastSuccessEpoch =
        static_cast<uint32_t>(time(nullptr));

    market.error = "";
  } else {
    diagnostics.marketFailures++;
  }

  lastMarketMs = millis();
  marketChartNeedsDraw = true;

  if (currentPage == Page::MARKET) {
    updateMarketValues();
    drawRealCandles();
  }
}

// -----------------------------------------------------------------------------
// mempool.space
// -----------------------------------------------------------------------------

void fetchNetworkData() {
  bool anySuccess = false;
  String response;

  if (secureGet(
        "https://mempool.space/api/blocks/tip/height",
        response
      )) {
    response.trim();

    network.blockHeight =
        static_cast<uint32_t>(response.toInt());

    network.heightValid =
        network.blockHeight > 0;

    anySuccess |= network.heightValid;
  }

  response = "";

  if (secureGet(
        "https://mempool.space/api/v1/fees/recommended",
        response
      )) {
    DynamicJsonDocument document(2048);

    if (!deserializeJson(document, response)) {
      network.fastestFee =
          document["fastestFee"] | 0;

      network.halfHourFee =
          document["halfHourFee"] | 0;

      network.hourFee =
          document["hourFee"] | 0;

      network.feesValid = true;
      anySuccess = true;
    }
  }

  if (anySuccess) {
    network.lastSuccessEpoch =
        static_cast<uint32_t>(time(nullptr));

    network.error = "";
  } else {
    diagnostics.networkFailures++;
    network.error = "Network API failed";
  }

  lastNetworkMs = millis();

  if (currentPage == Page::NETWORK) {
    updateNetworkValues();
  }
}

// -----------------------------------------------------------------------------
// Local double-SHA256 benchmark
// -----------------------------------------------------------------------------

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

void initializeBenchmark() {
  for (uint8_t index = 0; index < 80; index++) {
    blockHeader[index] =
        static_cast<uint8_t>(esp_random() & 0xFF);
  }

  mbedtls_sha256_init(&shaContext);
}

void runHashBatch() {
  uint8_t firstHash[32];
  uint8_t secondHash[32];

  for (uint16_t batch = 0;
       batch < HASH_BATCH;
       batch++) {

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

    uint8_t zeroBits =
        countLeadingZeroBits(secondHash);

    if (zeroBits > sessionBestZeroBits) {
      sessionBestZeroBits = zeroBits;
    }

    nonceValue++;
    sessionHashes++;
    hashesThisSecond++;
  }
}

void sampleHashrate() {
  currentKHs =
      hashesThisSecond /
      (VALUE_UPDATE_MS / 1000.0f) /
      1000.0f;

  hashesThisSecond = 0;

  if (smoothKHs < 0.01f) {
    smoothKHs = currentKHs;
  } else {
    smoothKHs =
        smoothKHs * 0.75f +
        currentKHs * 0.25f;
  }
}

// -----------------------------------------------------------------------------
// Navigation
// -----------------------------------------------------------------------------

void changePage(Page newPage) {
  currentPage = newPage;
  pageNeedsDraw = true;
  lastPageMs = millis();
}

void nextPage() {
  int next =
      (static_cast<int>(currentPage) + 1) %
      PAGE_COUNT;

  changePage(static_cast<Page>(next));
}

void handleButton() {
  bool pressed =
      digitalRead(PIN_BOOT) == LOW;

  if (pressed && !buttonWasDown) {
    buttonWasDown = true;
    buttonDownMs = millis();
  }

  if (!pressed && buttonWasDown) {
    uint32_t duration =
        millis() - buttonDownMs;

    buttonWasDown = false;

    if (duration >= BUTTON_LONG_MS) {
      reopenConfiguration();
    } else {
      nextPage();
    }
  }
}

void handleAutoRotation() {
  if (!config.autoRotate) {
    return;
  }

  uint32_t interval =
      static_cast<uint32_t>(config.pageSeconds) *
      1000UL;

  if (millis() - lastPageMs >= interval) {
    nextPage();
  }
}

// -----------------------------------------------------------------------------
// Scheduled maintenance
// -----------------------------------------------------------------------------

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWifiReconnectMs <
      WIFI_RECONNECT_MS) {
    return;
  }

  lastWifiReconnectMs = millis();
  diagnostics.wifiReconnects++;

  WiFi.reconnect();
}

void maintainData() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastMarketMs >= MARKET_UPDATE_MS) {
    fetchMarketData();
  }

  if (millis() - lastNetworkMs >= NETWORK_UPDATE_MS) {
    fetchNetworkData();
  }
}

void maintainPersistence() {
  if (millis() - lastSaveMs >= SAVE_UPDATE_MS) {
    saveLifetimeStats();
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

  diagnostics.resetReason = esp_reset_reason();
  diagnostics.minFreeHeap = ESP.getFreeHeap();

  loadPreferences();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  configureWifi();
  configureClock();
  initializeBenchmark();

  currentPage = Page::HOME;
  lastPageMs = millis();
  lastSaveMs = millis();

  drawCurrentLayout();
  updateCurrentValues();

  fetchNetworkData();
  fetchMarketData();
}

void loop() {
  runHashBatch();

  handleButton();
  handleAutoRotation();
  maintainWifi();
  maintainData();
  maintainPersistence();

  if (pageNeedsDraw) {
    drawCurrentLayout();
    updateCurrentValues();

    if (currentPage == Page::MARKET) {
      drawRealCandles();
    }
  }

  if (millis() - lastValueMs >= VALUE_UPDATE_MS) {
    lastValueMs = millis();

    sampleHashrate();
    updateCurrentValues();
  }

  delay(1);
}
