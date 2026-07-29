/*
  CYD Bitcoin Lab V9 Pool Edition (BETA)
  ======================================
  Target: ESP32-2432S028R / ESP32-WROOM-32E / ILI9341 320x240 landscape

  Replace only:
    CYD-Bitcoin-Miner/src/main.cpp

  Existing platformio.ini libraries:
    - TFT_eSPI
    - ArduinoJson 7
    - WiFiManager

  Setup portal fields:
    - Wi-Fi SSID/password (handled by WiFiManager)
    - public Bitcoin receiving address
    - OpenWeather API key
    - optional device name
    - page rotation settings

  Pool preset:
    Host: mine.ocean.xyz
    Port: 3334
    Username: <public Bitcoin address>.CYD01
    Password: x

  Pages:
    1. Pool Miner
    2. Mining Statistics
    3. BTC Market (Coinbase)
    4. Bitcoin Network (mempool.space)
    5. New Jersey Time & Date
    6. Paramus Weather (OpenWeather)
    7. Device Health
    8. Settings

  Important:
    - Only a PUBLIC Bitcoin receiving address is requested.
    - Never enter a seed phrase, private key, wallet password, or recovery words.
    - Pool traffic uses Stratum V1 over unencrypted TCP.
    - This ESP32 is an educational/lottery miner, not a practical income device.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include <math.h>
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"

// ============================================================================
// Hardware and theme
// ============================================================================

static constexpr uint8_t PIN_BACKLIGHT = 21;
static constexpr uint8_t PIN_BOOT = 0;

TFT_eSPI tft = TFT_eSPI();
Preferences preferences;

static constexpr uint16_t C_BG          = 0x0000;
static constexpr uint16_t C_HEADER      = 0x0841;
static constexpr uint16_t C_PANEL       = 0x1082;
static constexpr uint16_t C_PANEL_ALT   = 0x18C3;
static constexpr uint16_t C_BORDER      = 0x31A6;
static constexpr uint16_t C_GRID        = 0x2124;
static constexpr uint16_t C_TEXT        = 0xFFFF;
static constexpr uint16_t C_MUTED       = 0x9CD3;
static constexpr uint16_t C_ORANGE      = 0xFD20;
static constexpr uint16_t C_ORANGE_DARK = 0xA300;
static constexpr uint16_t C_CYAN        = 0x05FF;
static constexpr uint16_t C_GREEN       = 0x07E0;
static constexpr uint16_t C_RED         = 0xF800;
static constexpr uint16_t C_YELLOW      = 0xFFE0;
static constexpr uint16_t C_PURPLE      = 0xA81F;
static constexpr uint16_t C_BLUE        = 0x049F;

// ============================================================================
// Firmware constants
// ============================================================================

static const char* FW_VERSION = "9.0.0-beta";
static const char* AP_NAME = "CYD-Miner-Setup";
static const char* AP_PASSWORD = "bitcoin123";

static const char* POOL_HOST = "mine.ocean.xyz";
static constexpr uint16_t POOL_PORT = 3334;
static const char* POOL_PASSWORD = "x";
static const char* WORKER_SUFFIX = "CYD01";

static const char* NJ_TZ = "EST5EDT,M3.2.0/2,M11.1.0/2";
static constexpr double PARAMUS_LAT = 40.9445428;
static constexpr double PARAMUS_LON = -74.0754189;

static constexpr uint32_t VALUE_REFRESH_MS = 1000;
static constexpr uint32_t SERIAL_JSON_MS = 1000;
static constexpr uint32_t TICKER_REFRESH_MS = 60UL * 1000UL;
static constexpr uint32_t MARKET_REFRESH_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t NETWORK_REFRESH_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t SAVE_REFRESH_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t WIFI_RECONNECT_MS = 15UL * 1000UL;
static constexpr uint32_t POOL_RECONNECT_MS = 10UL * 1000UL;
static constexpr uint32_t POOL_SILENCE_MS = 120UL * 1000UL;
static constexpr uint32_t LONG_PRESS_MS = 3000;
static constexpr uint16_t HASH_BATCH_SIZE = 96;
static constexpr uint8_t CANDLE_COUNT = 24;
static constexpr uint8_t MAX_MERKLE_BRANCHES = 20;

// ============================================================================
// Configuration and persistent statistics
// ============================================================================

struct AppConfig {
  String deviceName = "ORANGE NODE";
  String bitcoinAddress = "";
  String openWeatherKey = "";
  bool autoRotate = true;
  uint16_t pageSeconds = 10;
};

struct LifetimeStats {
  uint64_t hashesBeforeBoot = 0;
  uint64_t runtimeBeforeBoot = 0;
  uint32_t acceptedBeforeBoot = 0;
  uint32_t rejectedBeforeBoot = 0;
  uint32_t bootCount = 0;
  uint8_t bestBitsBeforeBoot = 0;
};

AppConfig config;
LifetimeStats lifetime;

char portalDeviceName[25] = "ORANGE NODE";
char portalBitcoinAddress[91] = "";
char portalWeatherKey[65] = "";
char portalAutoRotate[2] = "1";
char portalPageSeconds[4] = "10";
bool portalSaveRequested = false;

// ============================================================================
// Pages and timers
// ============================================================================

enum class Page : uint8_t {
  POOL = 0,
  MINING = 1,
  MARKET = 2,
  NETWORK = 3,
  CLOCK = 4,
  WEATHER = 5,
  DEVICE = 6,
  SETTINGS = 7
};

static constexpr uint8_t PAGE_COUNT = 8;
Page currentPage = Page::POOL;

bool pageNeedsDraw = true;
bool marketChartNeedsDraw = true;
bool buttonWasDown = false;

uint32_t bootMillis = 0;
uint32_t lastValueRefresh = 0;
uint32_t lastSerialJson = 0;
uint32_t lastPageChange = 0;
uint32_t lastTickerRefresh = 0;
uint32_t lastMarketRefresh = 0;
uint32_t lastNetworkRefresh = 0;
uint32_t lastWeatherRefresh = 0;
uint32_t lastSaveRefresh = 0;
uint32_t lastWifiReconnect = 0;
uint32_t buttonDownMillis = 0;

// ============================================================================
// Utility helpers
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

String shortAddress(const String& address) {
  if (address.isEmpty()) {
    return "NOT CONFIGURED";
  }

  if (address.length() <= 22) {
    return address;
  }

  return address.substring(0, 10) + "..." + address.substring(address.length() - 7);
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

String titleCase(String value) {
  bool capitalize = true;

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];

    if (capitalize && c >= 'a' && c <= 'z') {
      value.setCharAt(i, c - 32);
    }

    capitalize = c == ' ' || c == '-' || c == '/';
  }

  return value;
}

String isoUtc(time_t value) {
  struct tm timeInfo;
  gmtime_r(&value, &timeInfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);

  return String(buffer);
}

String timeFromEpoch(uint32_t epoch) {
  if (epoch < 100000) {
    return "--:--";
  }

  time_t raw = static_cast<time_t>(epoch);
  struct tm info;
  localtime_r(&raw, &info);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%I:%M %p", &info);

  return String(buffer);
}

bool looksLikeBitcoinAddress(const String& address) {
  if (address.length() < 26 || address.length() > 90) {
    return false;
  }

  return address.startsWith("1") ||
         address.startsWith("3") ||
         address.startsWith("bc1") ||
         address.startsWith("BC1");
}

// ============================================================================
// Preferences
// ============================================================================

void loadPreferences() {
  preferences.begin("btc-v9", true);

  config.deviceName = preferences.getString("name", "ORANGE NODE");
  config.bitcoinAddress = preferences.getString("btc", "");
  config.openWeatherKey = preferences.getString("weather", "");
  config.autoRotate = preferences.getBool("rotate", true);
  config.pageSeconds = preferences.getUShort("pageSec", 10);

  lifetime.hashesBeforeBoot = preferences.getULong64("hashes", 0);
  lifetime.runtimeBeforeBoot = preferences.getULong64("runtime", 0);
  lifetime.acceptedBeforeBoot = preferences.getUInt("accepted", 0);
  lifetime.rejectedBeforeBoot = preferences.getUInt("rejected", 0);
  lifetime.bootCount = preferences.getUInt("boots", 0);
  lifetime.bestBitsBeforeBoot = preferences.getUChar("bestBits", 0);

  preferences.end();

  config.pageSeconds = constrain(config.pageSeconds, 5, 60);
  lifetime.bootCount++;
}

void saveConfiguration() {
  preferences.begin("btc-v9", false);

  preferences.putString("name", config.deviceName);
  preferences.putString("btc", config.bitcoinAddress);
  preferences.putString("weather", config.openWeatherKey);
  preferences.putBool("rotate", config.autoRotate);
  preferences.putUShort("pageSec", config.pageSeconds);

  preferences.end();
}

// Forward declarations for runtime values used by saveStats().
uint64_t sessionHashes = 0;
uint32_t acceptedShares = 0;
uint32_t rejectedShares = 0;
uint8_t sessionBestBits = 0;

void saveStats() {
  preferences.begin("btc-v9", false);

  preferences.putULong64("hashes", lifetime.hashesBeforeBoot + sessionHashes);
  preferences.putULong64(
    "runtime",
    lifetime.runtimeBeforeBoot + ((millis() - bootMillis) / 1000ULL)
  );
  preferences.putUInt("accepted", lifetime.acceptedBeforeBoot + acceptedShares);
  preferences.putUInt("rejected", lifetime.rejectedBeforeBoot + rejectedShares);
  preferences.putUInt("boots", lifetime.bootCount);
  preferences.putUChar("bestBits", max(lifetime.bestBitsBeforeBoot, sessionBestBits));

  preferences.end();
  lastSaveRefresh = millis();
}

// ============================================================================
// UI primitives
// ============================================================================

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

void panel(int x, int y, int w, int h, uint16_t fill = C_PANEL) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, C_BORDER);
}

void valueBox(
  const String& value,
  int x,
  int y,
  int w,
  int h,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.fillRect(x, y, w, h, background);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(value, x + w / 2, y + h / 2, font);
  tft.setTextDatum(TL_DATUM);
}

void leftValueBox(
  const String& value,
  int x,
  int y,
  int w,
  int h,
  uint16_t foreground,
  uint16_t background,
  uint8_t font
) {
  tft.fillRect(x, y, w, h, background);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(foreground, background);
  tft.drawString(value, x, y + h / 2, font);
  tft.setTextDatum(TL_DATUM);
}

String pageName(Page value) {
  switch (value) {
    case Page::POOL: return "POOL MINER";
    case Page::MINING: return "MINING STATS";
    case Page::MARKET: return "BTC MARKET";
    case Page::NETWORK: return "BITCOIN NETWORK";
    case Page::CLOCK: return "NEW JERSEY TIME";
    case Page::WEATHER: return "PARAMUS WEATHER";
    case Page::DEVICE: return "DEVICE HEALTH";
    case Page::SETTINGS: return "SETTINGS";
  }

  return "CYD BITCOIN LAB";
}

void drawHeader(const String& title, const String& subtitle, bool online) {
  tft.fillRect(0, 0, 320, 34, C_HEADER);
  tft.drawFastHLine(0, 33, 320, C_ORANGE_DARK);

  tft.fillCircle(17, 17, 11, C_ORANGE);
  centerText("B", 17, 17, C_BG, C_ORANGE, 2);

  leftText(title, 36, 3, C_TEXT, C_HEADER, 2);
  leftText(subtitle, 36, 20, C_MUTED, C_HEADER, 1);

  tft.fillCircle(301, 10, 4, online ? C_GREEN : C_RED);
  centerText(
    online ? "LIVE" : "OFF",
    285,
    24,
    online ? C_GREEN : C_RED,
    C_HEADER,
    1
  );
}

void drawPageDots() {
  const int startX = 125;
  const int spacing = 10;

  tft.fillRect(0, 224, 320, 16, C_HEADER);

  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    uint16_t color = static_cast<uint8_t>(currentPage) == i ? C_ORANGE : C_BORDER;
    tft.fillCircle(startX + i * spacing, 232, 3, color);
  }

  centerText("BOOT: NEXT", 42, 232, C_MUTED, C_HEADER, 1);
  centerText(
    String(static_cast<uint8_t>(currentPage) + 1) + "/" + String(PAGE_COUNT),
    286,
    232,
    C_MUTED,
    C_HEADER,
    1
  );
}

void metricCard(int x, int y, int w, int h, const String& label) {
  panel(x, y, w, h, C_PANEL);
  centerText(label, x + w / 2, y + 11, C_MUTED, C_PANEL, 1);
}

// ============================================================================
// Mining, pool, market, network, weather, and diagnostics state
// ============================================================================

struct StratumJob {
  bool valid = false;
  String jobId;
  String previousHash;
  String coinbase1;
  String coinbase2;
  String merkleBranches[MAX_MERKLE_BRANCHES];
  uint8_t merkleCount = 0;
  String version;
  String nbits;
  String ntime;
  bool cleanJobs = false;
};

struct PoolState {
  bool tcpConnected = false;
  bool subscribed = false;
  bool authorized = false;
  bool workReady = false;

  String extraNonce1;
  uint8_t extraNonce2Size = 4;
  uint64_t extraNonce2Counter = 0;
  String activeExtraNonce2;

  double difficulty = 1.0;
  uint32_t jobsReceived = 0;
  uint32_t reconnects = 0;
  uint32_t submitRequestId = 100;
  uint32_t lastMessageMillis = 0;
  uint32_t lastConnectAttempt = 0;
  uint32_t connectedSinceMillis = 0;
  uint32_t lastShareMillis = 0;
  uint32_t lastJobMillis = 0;
  uint32_t latencyMs = 0;

  String lastError = "NOT CONNECTED";
  String lineBuffer;
};

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

  Candle candles[CANDLE_COUNT];
  uint8_t candleCount = 0;
  String error = "WAITING FOR COINBASE";
};

struct NetworkData {
  bool heightValid = false;
  bool feesValid = false;
  uint32_t blockHeight = 0;
  int fastestFee = 0;
  int halfHourFee = 0;
  int hourFee = 0;
  String error = "WAITING FOR MEMPOOL.SPACE";
};

struct WeatherData {
  bool valid = false;
  float temperatureF = 0;
  float feelsLikeF = 0;
  float minimumF = 0;
  float maximumF = 0;
  int humidity = 0;
  int pressureHpa = 0;
  float windMph = 0;
  int windDegrees = 0;
  int clouds = 0;
  uint32_t sunrise = 0;
  uint32_t sunset = 0;
  String condition = "--";
  String description = "Waiting for OpenWeather";
  String error = "API KEY NOT CONFIGURED";
  uint32_t updatedEpoch = 0;
};

struct Diagnostics {
  uint32_t minimumHeap = UINT32_MAX;
  uint32_t wifiReconnects = 0;
  uint32_t marketFailures = 0;
  uint32_t networkFailures = 0;
  uint32_t weatherFailures = 0;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
};

WiFiClient poolClient;
StratumJob currentJob;
PoolState pool;
MarketData market;
NetworkData network;
WeatherData weather;
Diagnostics diagnostics;

uint8_t workHeader[80];
uint8_t localHeader[80];
uint8_t shareTarget[32];
uint32_t currentNonce = 0;
uint32_t hashesInWindow = 0;
float currentHashrateKHs = 0;
float smoothedHashrateKHs = 0;
bool miningPoolJob = false;

mbedtls_sha256_context headerMidstate;

// ============================================================================
// Hex and cryptographic helpers
// ============================================================================

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool hexToBytes(const String& hex, uint8_t* output, size_t outputLength) {
  if (hex.length() != outputLength * 2) {
    return false;
  }

  for (size_t i = 0; i < outputLength; i++) {
    int high = hexValue(hex[i * 2]);
    int low = hexValue(hex[i * 2 + 1]);

    if (high < 0 || low < 0) {
      return false;
    }

    output[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

bool hexToVector(const String& hex, std::vector<uint8_t>& output) {
  if ((hex.length() & 1U) != 0) {
    return false;
  }

  output.resize(hex.length() / 2);
  return hexToBytes(hex, output.data(), output.size());
}

String paddedHex64(uint64_t value, uint8_t byteCount) {
  String result;
  result.reserve(byteCount * 2);

  for (int index = byteCount - 1; index >= 0; index--) {
    uint8_t currentByte = index < 8 ? static_cast<uint8_t>(value >> (index * 8)) : 0;

    if (currentByte < 16) {
      result += '0';
    }

    result += String(currentByte, HEX);
  }

  result.toLowerCase();
  return result;
}

void doubleSha256(const uint8_t* data, size_t length, uint8_t output[32]) {
  uint8_t firstHash[32];
  mbedtls_sha256_context context;

  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts_ret(&context, 0);
  mbedtls_sha256_update_ret(&context, data, length);
  mbedtls_sha256_finish_ret(&context, firstHash);

  mbedtls_sha256_starts_ret(&context, 0);
  mbedtls_sha256_update_ret(&context, firstHash, 32);
  mbedtls_sha256_finish_ret(&context, output);
  mbedtls_sha256_free(&context);
}

uint8_t countLeadingZeroBits(const uint8_t digest[32]) {
  uint8_t bits = 0;

  // Bitcoin hashes are displayed with the digest bytes reversed.
  for (int index = 31; index >= 0; index--) {
    uint8_t value = digest[index];

    if (value == 0) {
      bits += 8;
      continue;
    }

    while ((value & 0x80U) == 0) {
      bits++;
      value <<= 1;
    }

    break;
  }

  return bits;
}

bool digestMeetsTarget(const uint8_t digest[32], const uint8_t target[32]) {
  for (uint8_t index = 0; index < 32; index++) {
    uint8_t hashByte = digest[31 - index];

    if (hashByte < target[index]) return true;
    if (hashByte > target[index]) return false;
  }

  return true;
}

void updateShareTarget(double difficulty) {
  if (!isfinite(difficulty) || difficulty <= 0) {
    difficulty = 1.0;
  }

  // Difficulty-1 target:
  // 00000000FFFF0000000000000000000000000000000000000000000000000000
  const char* diffOneHex =
    "00000000FFFF0000000000000000000000000000000000000000000000000000";

  mbedtls_mpi diffOne;
  mbedtls_mpi numerator;
  mbedtls_mpi denominator;
  mbedtls_mpi target;

  mbedtls_mpi_init(&diffOne);
  mbedtls_mpi_init(&numerator);
  mbedtls_mpi_init(&denominator);
  mbedtls_mpi_init(&target);

  mbedtls_mpi_read_string(&diffOne, 16, diffOneHex);

  const uint64_t scale = 1000000ULL;
  uint64_t scaledDifficulty = static_cast<uint64_t>(difficulty * scale + 0.5);
  if (scaledDifficulty == 0) scaledDifficulty = 1;

  char denominatorText[32];
  snprintf(
    denominatorText,
    sizeof(denominatorText),
    "%llu",
    static_cast<unsigned long long>(scaledDifficulty)
  );

  mbedtls_mpi_mul_int(&numerator, &diffOne, scale);
  mbedtls_mpi_read_string(&denominator, 10, denominatorText);
  mbedtls_mpi_div_mpi(&target, nullptr, &numerator, &denominator);

  memset(shareTarget, 0, sizeof(shareTarget));
  mbedtls_mpi_write_binary(&target, shareTarget, sizeof(shareTarget));

  mbedtls_mpi_free(&diffOne);
  mbedtls_mpi_free(&numerator);
  mbedtls_mpi_free(&denominator);
  mbedtls_mpi_free(&target);
}

void prepareHeaderMidstate() {
  mbedtls_sha256_free(&headerMidstate);
  mbedtls_sha256_init(&headerMidstate);
  mbedtls_sha256_starts_ret(&headerMidstate, 0);
  mbedtls_sha256_update_ret(&headerMidstate, workHeader, 64);
}

// ============================================================================
// Stratum V1 client
// ============================================================================

String poolUsername() {
  if (config.bitcoinAddress.isEmpty()) {
    return "";
  }

  return config.bitcoinAddress + "." + WORKER_SUFFIX;
}

void sendStratum(const String& message) {
  if (!poolClient.connected()) {
    return;
  }

  poolClient.print(message);
  poolClient.print('\n');
}

void sendSubscribe() {
  JsonDocument document;
  document["id"] = 1;
  document["method"] = "mining.subscribe";
  JsonArray params = document["params"].to<JsonArray>();
  params.add(String("CYD-Bitcoin-Lab/") + FW_VERSION);

  String payload;
  serializeJson(document, payload);
  sendStratum(payload);
}

void sendAuthorize() {
  JsonDocument document;
  document["id"] = 2;
  document["method"] = "mining.authorize";

  JsonArray params = document["params"].to<JsonArray>();
  params.add(poolUsername());
  params.add(POOL_PASSWORD);

  String payload;
  serializeJson(document, payload);
  sendStratum(payload);
}

void disconnectPool(const String& reason) {
  if (poolClient.connected()) {
    poolClient.stop();
  }

  pool.tcpConnected = false;
  pool.subscribed = false;
  pool.authorized = false;
  pool.workReady = false;
  pool.lastError = reason;
}

bool buildPoolWork() {
  if (!currentJob.valid || pool.extraNonce1.isEmpty()) {
    return false;
  }

  pool.activeExtraNonce2 = paddedHex64(
    pool.extraNonce2Counter++,
    pool.extraNonce2Size
  );

  String coinbaseHex =
    currentJob.coinbase1 +
    pool.extraNonce1 +
    pool.activeExtraNonce2 +
    currentJob.coinbase2;

  std::vector<uint8_t> coinbase;
  if (!hexToVector(coinbaseHex, coinbase)) {
    pool.lastError = "BAD COINBASE HEX";
    return false;
  }

  uint8_t merkleRoot[32];
  doubleSha256(coinbase.data(), coinbase.size(), merkleRoot);

  for (uint8_t branchIndex = 0; branchIndex < currentJob.merkleCount; branchIndex++) {
    uint8_t branch[32];

    if (!hexToBytes(currentJob.merkleBranches[branchIndex], branch, 32)) {
      pool.lastError = "BAD MERKLE BRANCH";
      return false;
    }

    uint8_t pair[64];
    memcpy(pair, merkleRoot, 32);
    memcpy(pair + 32, branch, 32);
    doubleSha256(pair, sizeof(pair), merkleRoot);
  }

  uint8_t versionBytes[4];
  uint8_t previousHashBytes[32];
  uint8_t ntimeBytes[4];
  uint8_t nbitsBytes[4];

  if (!hexToBytes(currentJob.version, versionBytes, 4) ||
      !hexToBytes(currentJob.previousHash, previousHashBytes, 32) ||
      !hexToBytes(currentJob.ntime, ntimeBytes, 4) ||
      !hexToBytes(currentJob.nbits, nbitsBytes, 4)) {
    pool.lastError = "BAD JOB HEX";
    return false;
  }

  // Version is serialized little-endian.
  for (uint8_t i = 0; i < 4; i++) {
    workHeader[i] = versionBytes[3 - i];
  }

  // Stratum V1 previous hash uses 4-byte word swapping for the header.
  for (uint8_t word = 0; word < 8; word++) {
    for (uint8_t byteIndex = 0; byteIndex < 4; byteIndex++) {
      workHeader[4 + word * 4 + byteIndex] =
        previousHashBytes[word * 4 + (3 - byteIndex)];
    }
  }

  // The computed merkle digest is reversed for block-header serialization.
  for (uint8_t i = 0; i < 32; i++) {
    workHeader[36 + i] = merkleRoot[31 - i];
  }

  for (uint8_t i = 0; i < 4; i++) {
    workHeader[68 + i] = ntimeBytes[3 - i];
    workHeader[72 + i] = nbitsBytes[3 - i];
  }

  currentNonce = esp_random();
  workHeader[76] = currentNonce & 0xFF;
  workHeader[77] = (currentNonce >> 8) & 0xFF;
  workHeader[78] = (currentNonce >> 16) & 0xFF;
  workHeader[79] = (currentNonce >> 24) & 0xFF;

  prepareHeaderMidstate();

  pool.workReady = true;
  miningPoolJob = true;
  pool.lastError = "MINING";

  return true;
}

void submitShare(uint32_t nonce) {
  if (!poolClient.connected() || !pool.authorized || !pool.workReady) {
    return;
  }

  JsonDocument document;
  uint32_t requestId = pool.submitRequestId++;

  document["id"] = requestId;
  document["method"] = "mining.submit";

  JsonArray params = document["params"].to<JsonArray>();
  params.add(poolUsername());
  params.add(currentJob.jobId);
  params.add(pool.activeExtraNonce2);
  params.add(currentJob.ntime);

  char nonceText[9];
  snprintf(nonceText, sizeof(nonceText), "%08lx", static_cast<unsigned long>(nonce));
  params.add(nonceText);

  String payload;
  serializeJson(document, payload);
  sendStratum(payload);

  pool.lastShareMillis = millis();
}

void handleNotify(JsonArrayConst params) {
  if (params.size() < 9) {
    pool.lastError = "SHORT MINING.NOTIFY";
    return;
  }

  currentJob.jobId = String(params[0] | "");
  currentJob.previousHash = String(params[1] | "");
  currentJob.coinbase1 = String(params[2] | "");
  currentJob.coinbase2 = String(params[3] | "");

  JsonArrayConst branches = params[4].as<JsonArrayConst>();
  currentJob.merkleCount = min(
    static_cast<int>(branches.size()),
    static_cast<int>(MAX_MERKLE_BRANCHES)
  );

  for (uint8_t i = 0; i < currentJob.merkleCount; i++) {
    currentJob.merkleBranches[i] = String(branches[i] | "");
  }

  currentJob.version = String(params[5] | "");
  currentJob.nbits = String(params[6] | "");
  currentJob.ntime = String(params[7] | "");
  currentJob.cleanJobs = params[8] | false;
  currentJob.valid = true;

  pool.jobsReceived++;
  pool.lastJobMillis = millis();
  buildPoolWork();
}

void handleStratumLine(const String& line) {
  JsonDocument document;
  DeserializationError error = deserializeJson(document, line);

  if (error) {
    pool.lastError = "STRATUM JSON ERROR";
    return;
  }

  pool.lastMessageMillis = millis();

  const char* method = document["method"] | nullptr;

  if (method != nullptr) {
    JsonArrayConst params = document["params"].as<JsonArrayConst>();

    if (strcmp(method, "mining.set_difficulty") == 0) {
      double difficulty = params[0] | 1.0;
      pool.difficulty = difficulty > 0 ? difficulty : 1.0;
      updateShareTarget(pool.difficulty);
      return;
    }

    if (strcmp(method, "mining.notify") == 0) {
      handleNotify(params);
      return;
    }

    if (strcmp(method, "mining.set_extranonce") == 0 && params.size() >= 2) {
      pool.extraNonce1 = String(params[0] | "");
      pool.extraNonce2Size = params[1] | 4;
      pool.extraNonce2Size = constrain(pool.extraNonce2Size, 1, 8);
      pool.workReady = false;
      return;
    }

    if (strcmp(method, "client.reconnect") == 0) {
      disconnectPool("POOL REQUESTED RECONNECT");
      return;
    }

    return;
  }

  int requestId = document["id"] | -1;

  if (requestId == 1) {
    JsonArrayConst result = document["result"].as<JsonArrayConst>();

    if (result.size() >= 3) {
      pool.extraNonce1 = String(result[1] | "");
      pool.extraNonce2Size = result[2] | 4;
      pool.extraNonce2Size = constrain(pool.extraNonce2Size, 1, 8);
      pool.subscribed = !pool.extraNonce1.isEmpty();

      if (pool.subscribed) {
        sendAuthorize();
      } else {
        pool.lastError = "SUBSCRIBE FAILED";
      }
    }

    return;
  }

  if (requestId == 2) {
    bool authorized = document["result"] | false;
    pool.authorized = authorized;
    pool.lastError = authorized ? "AUTHORIZED" : "AUTHORIZATION FAILED";
    return;
  }

  if (requestId >= 100) {
    bool accepted = document["result"] | false;

    if (accepted) {
      acceptedShares++;
      pool.lastError = "SHARE ACCEPTED";
    } else {
      rejectedShares++;

      if (!document["error"].isNull()) {
        pool.lastError = "SHARE REJECTED";
      } else {
        pool.lastError = "SUBMIT FAILED";
      }
    }
  }
}

void readStratum() {
  while (poolClient.connected() && poolClient.available()) {
    char current = static_cast<char>(poolClient.read());

    if (current == '\n') {
      pool.lineBuffer.trim();

      if (!pool.lineBuffer.isEmpty()) {
        handleStratumLine(pool.lineBuffer);
      }

      pool.lineBuffer = "";
      continue;
    }

    if (current != '\r') {
      pool.lineBuffer += current;
    }

    if (pool.lineBuffer.length() > 12000) {
      pool.lineBuffer = "";
      pool.lastError = "STRATUM LINE TOO LONG";
    }
  }
}

void connectPool() {
  if (config.bitcoinAddress.isEmpty()) {
    pool.lastError = "ADD BITCOIN ADDRESS";
    return;
  }

  if (!looksLikeBitcoinAddress(config.bitcoinAddress)) {
    pool.lastError = "CHECK BITCOIN ADDRESS";
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    pool.lastError = "WI-FI OFFLINE";
    return;
  }

  if (millis() - pool.lastConnectAttempt < POOL_RECONNECT_MS) {
    return;
  }

  pool.lastConnectAttempt = millis();
  pool.lastError = "CONNECTING";

  if (!poolClient.connect(POOL_HOST, POOL_PORT, 8000)) {
    pool.reconnects++;
    pool.lastError = "POOL CONNECT FAILED";
    return;
  }

  poolClient.setNoDelay(true);
  pool.tcpConnected = true;
  pool.subscribed = false;
  pool.authorized = false;
  pool.workReady = false;
  pool.connectedSinceMillis = millis();
  pool.lastMessageMillis = millis();
  pool.lineBuffer.reserve(2048);
  pool.lastError = "SUBSCRIBING";

  sendSubscribe();
}

void maintainPool() {
  if (!poolClient.connected()) {
    pool.tcpConnected = false;
    pool.subscribed = false;
    pool.authorized = false;
    pool.workReady = false;
    connectPool();
    return;
  }

  pool.tcpConnected = true;
  readStratum();

  if (millis() - pool.lastMessageMillis > POOL_SILENCE_MS) {
    disconnectPool("POOL TIMEOUT");
  }
}

// ============================================================================
// Mining engine
// ============================================================================

void initializeLocalWork() {
  for (uint8_t i = 0; i < sizeof(localHeader); i++) {
    localHeader[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  }

  memcpy(workHeader, localHeader, sizeof(workHeader));
  currentNonce = esp_random();
  prepareHeaderMidstate();
  miningPoolJob = false;
}

void runMiningBatch() {
  if (pool.authorized && currentJob.valid && !pool.workReady) {
    buildPoolWork();
  }

  if (!(pool.authorized && pool.workReady) && miningPoolJob) {
    initializeLocalWork();
  }

  uint8_t firstHash[32];
  uint8_t secondHash[32];
  mbedtls_sha256_context firstContext;
  mbedtls_sha256_context secondContext;

  mbedtls_sha256_init(&firstContext);
  mbedtls_sha256_init(&secondContext);

  for (uint16_t batch = 0; batch < HASH_BATCH_SIZE; batch++) {
    workHeader[76] = currentNonce & 0xFF;
    workHeader[77] = (currentNonce >> 8) & 0xFF;
    workHeader[78] = (currentNonce >> 16) & 0xFF;
    workHeader[79] = (currentNonce >> 24) & 0xFF;

    mbedtls_sha256_clone(&firstContext, &headerMidstate);
    mbedtls_sha256_update_ret(&firstContext, workHeader + 64, 16);
    mbedtls_sha256_finish_ret(&firstContext, firstHash);

    mbedtls_sha256_starts_ret(&secondContext, 0);
    mbedtls_sha256_update_ret(&secondContext, firstHash, 32);
    mbedtls_sha256_finish_ret(&secondContext, secondHash);

    uint8_t leadingBits = countLeadingZeroBits(secondHash);

    if (leadingBits > sessionBestBits) {
      sessionBestBits = leadingBits;
    }

    if (miningPoolJob && digestMeetsTarget(secondHash, shareTarget)) {
      submitShare(currentNonce);
    }

    currentNonce++;
    sessionHashes++;
    hashesInWindow++;

    if (currentNonce == 0) {
      if (pool.authorized && currentJob.valid) {
        pool.workReady = false;
        buildPoolWork();
      } else {
        initializeLocalWork();
      }
    }
  }

  mbedtls_sha256_free(&firstContext);
  mbedtls_sha256_free(&secondContext);
}

void sampleHashrate() {
  currentHashrateKHs =
    hashesInWindow /
    (VALUE_REFRESH_MS / 1000.0f) /
    1000.0f;

  hashesInWindow = 0;

  if (smoothedHashrateKHs < 0.01f) {
    smoothedHashrateKHs = currentHashrateKHs;
  } else {
    smoothedHashrateKHs =
      smoothedHashrateKHs * 0.74f +
      currentHashrateKHs * 0.26f;
  }
}

// ============================================================================
// NTP and HTTPS
// ============================================================================

void configureClock() {
  configTzTime(
    NJ_TZ,
    "pool.ntp.org",
    "time.cloudflare.com",
    "time.google.com"
  );
}

bool secureGet(const String& url, String& response, int* responseCode = nullptr) {
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

  int status = http.GET();

  if (responseCode != nullptr) {
    *responseCode = status;
  }

  if (status <= 0) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();

  return status == HTTP_CODE_OK;
}

// ============================================================================
// Coinbase market data
// ============================================================================

bool fetchCoinbaseTicker() {
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/ticker",
        response
      )) {
    market.error = "COINBASE TICKER FAILED";
    return false;
  }

  JsonDocument document;

  if (deserializeJson(document, response)) {
    market.error = "TICKER JSON ERROR";
    return false;
  }

  market.price = String(document["price"] | "0").toFloat();
  market.bid = String(document["bid"] | "0").toFloat();
  market.ask = String(document["ask"] | "0").toFloat();
  market.tickerValid = market.price > 0;

  return market.tickerValid;
}

bool fetchCoinbaseStats() {
  String response;

  if (!secureGet(
        "https://api.exchange.coinbase.com/products/BTC-USD/stats",
        response
      )) {
    market.error = "COINBASE STATS FAILED";
    return false;
  }

  JsonDocument document;

  if (deserializeJson(document, response)) {
    market.error = "STATS JSON ERROR";
    return false;
  }

  market.open24h = String(document["open"] | "0").toFloat();
  market.high24h = String(document["high"] | "0").toFloat();
  market.low24h = String(document["low"] | "0").toFloat();
  market.volume24h = String(document["volume"] | "0").toFloat();

  if (market.open24h > 0 && market.price > 0) {
    market.change24h =
      ((market.price - market.open24h) / market.open24h) * 100.0f;
    market.statsValid = true;
  }

  return market.statsValid;
}

bool fetchCoinbaseCandles() {
  time_t now = time(nullptr);

  if (now < 100000) {
    market.error = "CLOCK NOT READY";
    return false;
  }

  time_t start = now - 24L * 60L * 60L;

  String url =
    "https://api.exchange.coinbase.com/products/BTC-USD/candles"
    "?granularity=3600"
    "&start=" + isoUtc(start) +
    "&end=" + isoUtc(now);

  String response;

  if (!secureGet(url, response)) {
    market.error = "COINBASE CANDLES FAILED";
    return false;
  }

  JsonDocument document;

  if (deserializeJson(document, response) || !document.is<JsonArray>()) {
    market.error = "CANDLE JSON ERROR";
    return false;
  }

  JsonArray rows = document.as<JsonArray>();
  uint8_t available = min(
    static_cast<int>(rows.size()),
    static_cast<int>(CANDLE_COUNT)
  );

  market.candleCount = 0;

  // Coinbase sends newest buckets first; reverse them for left-to-right time.
  for (int destination = 0; destination < available; destination++) {
    int source = available - 1 - destination;
    JsonArray row = rows[source].as<JsonArray>();

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
  marketChartNeedsDraw = true;

  return market.candlesValid;
}

void fetchMarketBundle() {
  bool tickerOk = fetchCoinbaseTicker();
  bool statsOk = fetchCoinbaseStats();
  bool candlesOk = fetchCoinbaseCandles();

  if (tickerOk || statsOk || candlesOk) {
    market.error = "";
  } else {
    diagnostics.marketFailures++;
  }

  lastTickerRefresh = millis();
  lastMarketRefresh = millis();
}

// ============================================================================
// Bitcoin network data
// ============================================================================

void fetchNetworkData() {
  bool success = false;
  String response;

  if (secureGet(
        "https://mempool.space/api/blocks/tip/height",
        response
      )) {
    response.trim();
    network.blockHeight = static_cast<uint32_t>(response.toInt());
    network.heightValid = network.blockHeight > 0;
    success |= network.heightValid;
  }

  response = "";

  if (secureGet(
        "https://mempool.space/api/v1/fees/recommended",
        response
      )) {
    JsonDocument document;

    if (!deserializeJson(document, response)) {
      network.fastestFee = document["fastestFee"] | 0;
      network.halfHourFee = document["halfHourFee"] | 0;
      network.hourFee = document["hourFee"] | 0;
      network.feesValid = true;
      success = true;
    }
  }

  network.error = success ? "" : "MEMPOOL API FAILED";

  if (!success) {
    diagnostics.networkFailures++;
  }

  lastNetworkRefresh = millis();
}

// ============================================================================
// Paramus OpenWeather data
// ============================================================================

void fetchWeatherData() {
  if (config.openWeatherKey.isEmpty()) {
    weather.valid = false;
    weather.error = "ADD OPENWEATHER API KEY";
    lastWeatherRefresh = millis();
    return;
  }

  String url =
    "https://api.openweathermap.org/data/2.5/weather"
    "?lat=" + String(PARAMUS_LAT, 6) +
    "&lon=" + String(PARAMUS_LON, 6) +
    "&units=imperial"
    "&appid=" + config.openWeatherKey;

  String response;
  int statusCode = 0;

  if (!secureGet(url, response, &statusCode)) {
    weather.valid = false;
    weather.error = statusCode == 401 ? "INVALID WEATHER API KEY" : "WEATHER REQUEST FAILED";
    diagnostics.weatherFailures++;
    lastWeatherRefresh = millis();
    return;
  }

  JsonDocument document;

  if (deserializeJson(document, response)) {
    weather.valid = false;
    weather.error = "WEATHER JSON ERROR";
    diagnostics.weatherFailures++;
    lastWeatherRefresh = millis();
    return;
  }

  weather.temperatureF = document["main"]["temp"] | 0.0f;
  weather.feelsLikeF = document["main"]["feels_like"] | 0.0f;
  weather.minimumF = document["main"]["temp_min"] | 0.0f;
  weather.maximumF = document["main"]["temp_max"] | 0.0f;
  weather.humidity = document["main"]["humidity"] | 0;
  weather.pressureHpa = document["main"]["pressure"] | 0;
  weather.windMph = document["wind"]["speed"] | 0.0f;
  weather.windDegrees = document["wind"]["deg"] | 0;
  weather.clouds = document["clouds"]["all"] | 0;
  weather.sunrise = document["sys"]["sunrise"] | 0;
  weather.sunset = document["sys"]["sunset"] | 0;
  weather.condition = String(document["weather"][0]["main"] | "--");
  weather.description = titleCase(String(document["weather"][0]["description"] | "--"));
  weather.updatedEpoch = document["dt"] | static_cast<uint32_t>(time(nullptr));
  weather.error = "";
  weather.valid = true;

  lastWeatherRefresh = millis();
}

// ============================================================================
// Page layouts
// ============================================================================

void drawPoolLayout() {
  tft.fillScreen(C_BG);
  drawHeader("OCEAN POOL", config.deviceName, pool.tcpConnected);

  panel(8, 40, 304, 62, C_PANEL);
  leftText("REAL STRATUM HASHRATE", 18, 48, C_MUTED, C_PANEL, 1);

  metricCard(8, 110, 96, 49, "ACCEPTED");
  metricCard(112, 110, 96, 49, "REJECTED");
  metricCard(216, 110, 96, 49, "JOBS");

  panel(8, 167, 304, 49, C_PANEL);
  leftText("STATUS", 18, 176, C_MUTED, C_PANEL, 1);
  leftText("DIFF", 213, 176, C_MUTED, C_PANEL, 1);

  drawPageDots();
}

void drawMiningLayout() {
  tft.fillScreen(C_BG);
  drawHeader("MINING STATS", miningPoolJob ? "POOL JOB" : "LOCAL FALLBACK", true);

  metricCard(8, 42, 148, 58, "SESSION HASHES");
  metricCard(164, 42, 148, 58, "LIFETIME HASHES");
  metricCard(8, 108, 96, 55, "BEST BITS");
  metricCard(112, 108, 96, 55, "NONCE");
  metricCard(216, 108, 96, 55, "UPTIME");

  panel(8, 171, 304, 45, C_PANEL);
  leftText("JOB", 18, 179, C_MUTED, C_PANEL, 1);
  leftText("LAST SHARE", 200, 179, C_MUTED, C_PANEL, 1);

  drawPageDots();
}

void drawMarketLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BTC / USD", "COINBASE 24H", market.tickerValid);

  panel(8, 40, 304, 48, C_PANEL);
  leftText("LAST PRICE", 17, 47, C_MUTED, C_PANEL, 1);

  panel(8, 96, 304, 120, C_PANEL);

  drawPageDots();
}

void drawNetworkLayout() {
  tft.fillScreen(C_BG);
  drawHeader("BITCOIN NETWORK", "MEMPOOL.SPACE", network.heightValid || network.feesValid);

  metricCard(8, 42, 304, 48, "BLOCK HEIGHT");
  metricCard(8, 99, 96, 102, "FASTEST");
  metricCard(112, 99, 96, 102, "30 MIN");
  metricCard(216, 99, 96, 102, "1 HOUR");

  drawPageDots();
}

void drawClockLayout() {
  tft.fillScreen(C_BG);
  drawHeader("NEW JERSEY TIME", "AMERICA / NEW YORK", time(nullptr) > 100000);

  panel(8, 44, 304, 89, C_PANEL);
  panel(8, 141, 304, 75, C_PANEL);

  centerText("PARAMUS, NEW JERSEY", 160, 155, C_MUTED, C_PANEL, 1);
  centerText("AUTOMATIC EST / EDT", 160, 201, C_CYAN, C_PANEL, 1);

  drawPageDots();
}

void drawWeatherLayout() {
  tft.fillScreen(C_BG);
  drawHeader("PARAMUS WEATHER", "OPENWEATHER", weather.valid);

  panel(8, 42, 140, 105, C_PANEL);
  panel(156, 42, 156, 105, C_PANEL);
  metricCard(8, 155, 96, 61, "HUMIDITY");
  metricCard(112, 155, 96, 61, "WIND");
  metricCard(216, 155, 96, 61, "SUNSET");

  drawPageDots();
}

void drawDeviceLayout() {
  tft.fillScreen(C_BG);
  drawHeader("DEVICE HEALTH", "ESP32 TELEMETRY", WiFi.status() == WL_CONNECTED);

  metricCard(8, 42, 148, 58, "FREE HEAP");
  metricCard(164, 42, 148, 58, "MIN HEAP");
  metricCard(8, 108, 148, 58, "WI-FI SIGNAL");
  metricCard(164, 108, 148, 58, "CPU");

  panel(8, 174, 304, 42, C_PANEL);
  leftText("RESET", 18, 183, C_MUTED, C_PANEL, 1);
  leftText("RECONNECTS", 205, 183, C_MUTED, C_PANEL, 1);

  drawPageDots();
}

void drawSettingsLayout() {
  tft.fillScreen(C_BG);
  drawHeader("SETTINGS", String("V") + FW_VERSION, true);

  panel(8, 42, 304, 42, C_PANEL);
  leftText("POOL", 17, 49, C_MUTED, C_PANEL, 1);
  leftText(String(POOL_HOST) + ":" + String(POOL_PORT), 17, 65, C_TEXT, C_PANEL, 2);

  panel(8, 92, 304, 42, C_PANEL);
  leftText("PAYOUT ADDRESS", 17, 99, C_MUTED, C_PANEL, 1);
  leftText(shortAddress(config.bitcoinAddress), 17, 115, C_ORANGE, C_PANEL, 2);

  panel(8, 142, 304, 42, C_PANEL);
  leftText("WEATHER API", 17, 149, C_MUTED, C_PANEL, 1);
  leftText(config.openWeatherKey.isEmpty() ? "NOT CONFIGURED" : "CONFIGURED", 17, 165,
           config.openWeatherKey.isEmpty() ? C_RED : C_GREEN, C_PANEL, 2);

  panel(8, 192, 304, 24, C_PANEL);
  centerText("HOLD BOOT 3 SEC TO REOPEN SETUP", 160, 204, C_CYAN, C_PANEL, 1);

  drawPageDots();
}

void drawCurrentLayout() {
  switch (currentPage) {
    case Page::POOL: drawPoolLayout(); break;
    case Page::MINING: drawMiningLayout(); break;
    case Page::MARKET: drawMarketLayout(); break;
    case Page::NETWORK: drawNetworkLayout(); break;
    case Page::CLOCK: drawClockLayout(); break;
    case Page::WEATHER: drawWeatherLayout(); break;
    case Page::DEVICE: drawDeviceLayout(); break;
    case Page::SETTINGS: drawSettingsLayout(); break;
  }

  pageNeedsDraw = false;
}

// ============================================================================
// Number-only updates
// ============================================================================

void updatePoolValues() {
  leftValueBox(
    miningPoolJob ? "REAL STRATUM HASHRATE" : "LOCAL FALLBACK HASHRATE",
    18,
    48,
    178,
    12,
    miningPoolJob ? C_MUTED : C_YELLOW,
    C_PANEL,
    1
  );

  leftValueBox(String(smoothedHashrateKHs, 2), 18, 63, 182, 31, C_ORANGE, C_PANEL, 4);
  leftValueBox("kH/s", 207, 70, 68, 22, C_TEXT, C_PANEL, 2);

  valueBox(String(lifetime.acceptedBeforeBoot + acceptedShares), 17, 132, 78, 20, C_GREEN, C_PANEL, 2);
  valueBox(String(lifetime.rejectedBeforeBoot + rejectedShares), 121, 132, 78, 20, C_RED, C_PANEL, 2);
  valueBox(String(pool.jobsReceived), 225, 132, 78, 20, C_CYAN, C_PANEL, 2);

  String status = pool.lastError;
  if (pool.authorized && pool.workReady) status = "AUTHORIZED / MINING";
  else if (pool.authorized) status = "AUTHORIZED / WAITING JOB";
  else if (pool.subscribed) status = "SUBSCRIBED / AUTHORIZING";
  else if (pool.tcpConnected) status = "CONNECTED / SUBSCRIBING";

  leftValueBox(status, 18, 192, 184, 17,
               pool.authorized ? C_GREEN : C_YELLOW, C_PANEL, 1);
  valueBox(String(pool.difficulty, 4), 244, 191, 58, 18, C_PURPLE, C_PANEL, 1);
}

void updateMiningValues() {
  valueBox(compactNumber(sessionHashes), 18, 66, 128, 24, C_TEXT, C_PANEL, 2);
  valueBox(compactNumber(lifetime.hashesBeforeBoot + sessionHashes), 174, 66, 128, 24,
           C_CYAN, C_PANEL, 2);

  valueBox(String(max(lifetime.bestBitsBeforeBoot, sessionBestBits)), 18, 134, 76, 24,
           C_YELLOW, C_PANEL, 2);

  char nonceBuffer[12];
  snprintf(nonceBuffer, sizeof(nonceBuffer), "%08lX", static_cast<unsigned long>(currentNonce));
  valueBox(nonceBuffer, 121, 134, 78, 24, C_TEXT, C_PANEL, 1);

  valueBox(uptimeText((millis() - bootMillis) / 1000ULL), 225, 134, 78, 24,
           C_GREEN, C_PANEL, 1);

  String jobId = currentJob.valid ? currentJob.jobId : "LOCAL";
  if (jobId.length() > 18) jobId = jobId.substring(0, 18);
  leftValueBox(jobId, 18, 196, 170, 15, C_CYAN, C_PANEL, 1);

  String lastShare = pool.lastShareMillis == 0
    ? "NONE"
    : uptimeText((millis() - pool.lastShareMillis) / 1000ULL) + " AGO";
  valueBox(lastShare, 204, 195, 98, 16, C_ORANGE, C_PANEL, 1);
}

void updateMarketValues() {
  leftValueBox(
    market.tickerValid ? "$" + String(market.price, 2) : "UNAVAILABLE",
    17,
    61,
    190,
    22,
    market.tickerValid ? C_ORANGE : C_RED,
    C_PANEL,
    market.tickerValid ? 4 : 2
  );

  String change = market.statsValid
    ? String(market.change24h >= 0 ? "+" : "") + String(market.change24h, 2) + "%"
    : "--";

  valueBox(change, 225, 57, 76, 28,
           market.change24h >= 0 ? C_GREEN : C_RED, C_PANEL, 2);
}

void drawMarketChart() {
  const int x = 15;
  const int y = 103;
  const int width = 290;
  const int height = 107;

  tft.fillRect(x, y, width, height, C_PANEL);

  if (!market.candlesValid || market.candleCount < 2) {
    centerText(market.error, 160, 156, C_MUTED, C_PANEL, 1);
    return;
  }

  float minimum = market.candles[0].low;
  float maximum = market.candles[0].high;
  float maximumVolume = market.candles[0].volume;

  for (uint8_t i = 1; i < market.candleCount; i++) {
    minimum = min(minimum, market.candles[i].low);
    maximum = max(maximum, market.candles[i].high);
    maximumVolume = max(maximumVolume, market.candles[i].volume);
  }

  float range = max(1.0f, maximum - minimum);
  const int chartTop = y + 4;
  const int chartBottom = y + 80;
  const int volumeTop = y + 84;
  const int volumeBottom = y + height - 3;

  auto priceY = [&](float price) -> int {
    return chartBottom - static_cast<int>(((price - minimum) / range) * (chartBottom - chartTop));
  };

  for (int line = 1; line < 4; line++) {
    int gridY = chartTop + line * (chartBottom - chartTop) / 4;
    tft.drawFastHLine(x, gridY, width, C_GRID);
  }

  int spacing = max(7, width / market.candleCount);
  int bodyWidth = max(3, spacing / 2);

  for (uint8_t i = 0; i < market.candleCount; i++) {
    const Candle& candle = market.candles[i];
    int centerX = x + i * spacing + spacing / 2;
    int highY = priceY(candle.high);
    int lowY = priceY(candle.low);
    int openY = priceY(candle.open);
    int closeY = priceY(candle.close);
    uint16_t color = candle.close >= candle.open ? C_GREEN : C_RED;

    tft.drawFastVLine(centerX, highY, max(1, lowY - highY), color);
    tft.fillRect(centerX - bodyWidth / 2, min(openY, closeY), bodyWidth,
                 max(2, abs(closeY - openY)), color);

    int volumeHeight = maximumVolume > 0
      ? static_cast<int>((candle.volume / maximumVolume) * (volumeBottom - volumeTop))
      : 0;
    tft.fillRect(centerX - 1, volumeBottom - volumeHeight, 3, volumeHeight, C_PURPLE);
  }

  for (uint8_t i = 1; i < market.candleCount; i++) {
    int x1 = x + (i - 1) * spacing + spacing / 2;
    int x2 = x + i * spacing + spacing / 2;
    tft.drawLine(x1, priceY(market.candles[i - 1].close),
                 x2, priceY(market.candles[i].close), C_CYAN);
  }

  if (market.tickerValid) {
    int currentY = priceY(market.price);
    tft.drawFastHLine(x, currentY, width, C_ORANGE_DARK);
  }

  leftText("24H HOURLY", x + 3, y + 2, C_MUTED, C_PANEL, 1);
  leftText("$" + String(maximum, 0), x + 216, y + 2, C_MUTED, C_PANEL, 1);
  leftText("$" + String(minimum, 0), x + 216, y + 72, C_MUTED, C_PANEL, 1);

  marketChartNeedsDraw = false;
}

void updateNetworkValues() {
  valueBox(network.heightValid ? String(network.blockHeight) : "--",
           20, 66, 280, 18, C_PURPLE, C_PANEL, 2);

  valueBox(network.feesValid ? String(network.fastestFee) : "--",
           18, 130, 76, 42, C_ORANGE, C_PANEL, 4);
  valueBox("sat/vB", 18, 178, 76, 14, C_MUTED, C_PANEL, 1);

  valueBox(network.feesValid ? String(network.halfHourFee) : "--",
           122, 130, 76, 42, C_YELLOW, C_PANEL, 4);
  valueBox("sat/vB", 122, 178, 76, 14, C_MUTED, C_PANEL, 1);

  valueBox(network.feesValid ? String(network.hourFee) : "--",
           226, 130, 76, 42, C_CYAN, C_PANEL, 4);
  valueBox("sat/vB", 226, 178, 76, 14, C_MUTED, C_PANEL, 1);
}

void updateClockValues() {
  struct tm info;

  if (!getLocalTime(&info, 10)) {
    valueBox("SYNCING...", 18, 69, 284, 46, C_YELLOW, C_PANEL, 4);
    valueBox("NTP TIME NOT READY", 18, 170, 284, 22, C_MUTED, C_PANEL, 2);
    return;
  }

  char timeBuffer[20];
  char dateBuffer[40];
  char weekdayBuffer[20];

  strftime(timeBuffer, sizeof(timeBuffer), "%I:%M:%S %p", &info);
  strftime(dateBuffer, sizeof(dateBuffer), "%B %d, %Y", &info);
  strftime(weekdayBuffer, sizeof(weekdayBuffer), "%A", &info);

  valueBox(timeBuffer, 18, 69, 284, 46, C_ORANGE, C_PANEL, 4);
  valueBox(weekdayBuffer, 18, 168, 284, 20, C_TEXT, C_PANEL, 2);
  valueBox(dateBuffer, 18, 188, 284, 18, C_CYAN, C_PANEL, 2);
}

void updateWeatherValues() {
  if (!weather.valid) {
    valueBox("--", 18, 74, 120, 42, C_RED, C_PANEL, 4);
    valueBox(weather.error, 166, 78, 136, 32, C_RED, C_PANEL, 1);
    valueBox("--", 18, 184, 76, 20, C_MUTED, C_PANEL, 2);
    valueBox("--", 122, 184, 76, 20, C_MUTED, C_PANEL, 2);
    valueBox("--", 226, 184, 76, 20, C_MUTED, C_PANEL, 2);
    return;
  }

  valueBox(String(weather.temperatureF, 1) + " F", 18, 65, 120, 46,
           C_ORANGE, C_PANEL, 4);
  valueBox("FEELS " + String(weather.feelsLikeF, 1) + " F", 18, 116, 120, 20,
           C_CYAN, C_PANEL, 1);

  valueBox(weather.condition, 166, 60, 136, 28, C_TEXT, C_PANEL, 2);
  valueBox(weather.description, 166, 90, 136, 26, C_MUTED, C_PANEL, 1);
  valueBox("H " + String(weather.maximumF, 0) + " / L " + String(weather.minimumF, 0),
           166, 118, 136, 18, C_YELLOW, C_PANEL, 1);

  valueBox(String(weather.humidity) + "%", 18, 184, 76, 20, C_CYAN, C_PANEL, 2);
  valueBox(String(weather.windMph, 1) + " mph", 122, 184, 76, 20, C_GREEN, C_PANEL, 1);
  valueBox(timeFromEpoch(weather.sunset), 226, 184, 76, 20, C_ORANGE, C_PANEL, 1);
}

void updateDeviceValues() {
  diagnostics.minimumHeap = min(diagnostics.minimumHeap, ESP.getFreeHeap());

  valueBox(compactNumber(ESP.getFreeHeap()), 18, 66, 128, 24, C_CYAN, C_PANEL, 2);
  valueBox(compactNumber(diagnostics.minimumHeap), 174, 66, 128, 24, C_YELLOW, C_PANEL, 2);

  valueBox(WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "OFFLINE",
           18, 132, 128, 24, WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED, C_PANEL, 2);

  valueBox(String(ESP.getCpuFreqMHz()) + " MHz", 174, 132, 128, 24,
           C_ORANGE, C_PANEL, 2);

  leftValueBox(resetReasonText(diagnostics.resetReason), 18, 199, 170, 13,
               C_TEXT, C_PANEL, 1);
  valueBox(String(diagnostics.wifiReconnects + pool.reconnects), 249, 198, 53, 15,
           C_PURPLE, C_PANEL, 1);
}

void updateCurrentValues() {
  switch (currentPage) {
    case Page::POOL: updatePoolValues(); break;
    case Page::MINING: updateMiningValues(); break;
    case Page::MARKET: updateMarketValues(); break;
    case Page::NETWORK: updateNetworkValues(); break;
    case Page::CLOCK: updateClockValues(); break;
    case Page::WEATHER: updateWeatherValues(); break;
    case Page::DEVICE: updateDeviceValues(); break;
    case Page::SETTINGS: break;
  }
}

// ============================================================================
// Wi-Fi setup portal
// ============================================================================

void savePortalCallback() {
  portalSaveRequested = true;
}

void configureWifi() {
  snprintf(portalDeviceName, sizeof(portalDeviceName), "%s", config.deviceName.c_str());
  snprintf(portalBitcoinAddress, sizeof(portalBitcoinAddress), "%s", config.bitcoinAddress.c_str());
  snprintf(portalWeatherKey, sizeof(portalWeatherKey), "%s", config.openWeatherKey.c_str());
  snprintf(portalAutoRotate, sizeof(portalAutoRotate), "%d", config.autoRotate ? 1 : 0);
  snprintf(portalPageSeconds, sizeof(portalPageSeconds), "%u", config.pageSeconds);

  tft.fillScreen(C_BG);
  drawHeader("WI-FI SETUP", "POOL + WEATHER CONFIG", false);

  panel(12, 48, 296, 48, C_PANEL);
  centerText("CONNECT TO", 160, 59, C_MUTED, C_PANEL, 1);
  centerText(AP_NAME, 160, 78, C_ORANGE, C_PANEL, 2);

  panel(12, 104, 296, 51, C_PANEL);
  centerText(String("PASSWORD: ") + AP_PASSWORD, 160, 121, C_TEXT, C_PANEL, 2);
  centerText("OPEN 192.168.4.1 IF NEEDED", 160, 143, C_CYAN, C_PANEL, 1);

  panel(12, 163, 296, 44, C_PANEL);
  centerText("ENTER PUBLIC BTC ADDRESS + WEATHER KEY", 160, 178, C_YELLOW, C_PANEL, 1);
  centerText("NEVER ENTER PRIVATE KEYS OR SEED WORDS", 160, 195, C_RED, C_PANEL, 1);

  WiFiManager manager;

  WiFiManagerParameter pName(
    "deviceName",
    "Device name",
    portalDeviceName,
    24
  );

  WiFiManagerParameter pBitcoinAddress(
    "bitcoinAddress",
    "PUBLIC Bitcoin receiving address",
    portalBitcoinAddress,
    90
  );

  WiFiManagerParameter pWeatherKey(
    "openWeatherKey",
    "OpenWeather API key",
    portalWeatherKey,
    64
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

  manager.addParameter(&pName);
  manager.addParameter(&pBitcoinAddress);
  manager.addParameter(&pWeatherKey);
  manager.addParameter(&pAutoRotate);
  manager.addParameter(&pPageSeconds);

  manager.setSaveConfigCallback(savePortalCallback);
  manager.setTitle("CYD Bitcoin Lab V9 Setup");
  manager.setClass("invert");
  manager.setConnectTimeout(30);
  manager.setConfigPortalTimeout(300);

  bool connected = manager.autoConnect(AP_NAME, AP_PASSWORD);

  if (!connected) {
    tft.fillScreen(C_BG);
    centerText("SETUP TIMED OUT", 160, 103, C_RED, C_BG, 4);
    centerText("RESTARTING", 160, 140, C_MUTED, C_BG, 2);
    delay(2500);
    ESP.restart();
  }

  if (portalSaveRequested) {
    config.deviceName = pName.getValue();
    config.bitcoinAddress = pBitcoinAddress.getValue();
    config.openWeatherKey = pWeatherKey.getValue();
    config.autoRotate = atoi(pAutoRotate.getValue()) != 0;
    config.pageSeconds = constrain(atoi(pPageSeconds.getValue()), 5, 60);

    config.deviceName.trim();
    config.bitcoinAddress.trim();
    config.openWeatherKey.trim();

    if (config.deviceName.isEmpty()) {
      config.deviceName = "ORANGE NODE";
    }

    saveConfiguration();
  }

  tft.fillScreen(C_BG);
  centerText("WI-FI CONNECTED", 160, 95, C_GREEN, C_BG, 4);
  centerText(WiFi.SSID(), 160, 134, C_TEXT, C_BG, 2);
  centerText(WiFi.localIP().toString(), 160, 160, C_CYAN, C_BG, 2);
  delay(1200);
}

void reopenSetup() {
  saveStats();
  disconnectPool("OPENING SETUP");

  tft.fillScreen(C_BG);
  centerText("OPENING SETUP", 160, 103, C_ORANGE, C_BG, 4);

  WiFiManager manager;
  manager.resetSettings();

  delay(1500);
  ESP.restart();
}

// ============================================================================
// Browser live-view JSON over USB serial
// ============================================================================

void emitSerialJson() {
  JsonDocument document;

  document["type"] = "status";
  document["version"] = FW_VERSION;
  document["device"] = config.deviceName;
  document["pool"] = "OCEAN";
  document["pool_connected"] = pool.tcpConnected;
  document["subscribed"] = pool.subscribed;
  document["authorized"] = pool.authorized;
  document["mining_pool_job"] = miningPoolJob;
  document["pool_status"] = pool.lastError;
  document["difficulty"] = pool.difficulty;
  document["hashrate_khs"] = smoothedHashrateKHs;
  document["session_hashes"] = sessionHashes;
  document["lifetime_hashes"] = lifetime.hashesBeforeBoot + sessionHashes;
  document["accepted"] = lifetime.acceptedBeforeBoot + acceptedShares;
  document["rejected"] = lifetime.rejectedBeforeBoot + rejectedShares;
  document["jobs"] = pool.jobsReceived;
  document["best_bits"] = max(lifetime.bestBitsBeforeBoot, sessionBestBits);
  document["nonce"] = currentNonce;
  document["uptime_s"] = (millis() - bootMillis) / 1000UL;
  document["block_height"] = network.heightValid ? network.blockHeight : 0;
  document["btc_price"] = market.tickerValid ? market.price : 0;
  document["btc_change_24h"] = market.statsValid ? market.change24h : 0;
  document["weather_valid"] = weather.valid;
  document["weather_temp_f"] = weather.valid ? weather.temperatureF : 0;
  document["weather_condition"] = weather.valid ? weather.description : weather.error;
  document["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  document["free_heap"] = ESP.getFreeHeap();

  serializeJson(document, Serial);
  Serial.println();
}

// ============================================================================
// Navigation and scheduled work
// ============================================================================

void changePage(Page value) {
  currentPage = value;
  pageNeedsDraw = true;
  lastPageChange = millis();
}

void nextPage() {
  uint8_t next = (static_cast<uint8_t>(currentPage) + 1) % PAGE_COUNT;
  changePage(static_cast<Page>(next));
}

void handleButton() {
  bool down = digitalRead(PIN_BOOT) == LOW;

  if (down && !buttonWasDown) {
    buttonWasDown = true;
    buttonDownMillis = millis();
  }

  if (!down && buttonWasDown) {
    uint32_t duration = millis() - buttonDownMillis;
    buttonWasDown = false;

    if (duration >= LONG_PRESS_MS) {
      reopenSetup();
    } else {
      nextPage();
    }
  }
}

void maintainPageRotation() {
  if (!config.autoRotate) {
    return;
  }

  uint32_t interval = static_cast<uint32_t>(config.pageSeconds) * 1000UL;

  if (millis() - lastPageChange >= interval) {
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
  diagnostics.wifiReconnects++;
  WiFi.reconnect();
}

enum class StartupDataStep : uint8_t {
  WAIT = 0,
  NETWORK = 1,
  TICKER = 2,
  MARKET = 3,
  WEATHER = 4,
  DONE = 5
};

StartupDataStep startupDataStep = StartupDataStep::WAIT;
uint32_t nextStartupDataMillis = 0;

void maintainData() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (startupDataStep != StartupDataStep::DONE) {
    if (millis() < nextStartupDataMillis) {
      return;
    }

    switch (startupDataStep) {
      case StartupDataStep::WAIT:
        startupDataStep = StartupDataStep::NETWORK;
        break;

      case StartupDataStep::NETWORK:
        fetchNetworkData();
        startupDataStep = StartupDataStep::TICKER;
        break;

      case StartupDataStep::TICKER:
        fetchCoinbaseTicker();
        lastTickerRefresh = millis();
        startupDataStep = StartupDataStep::MARKET;
        break;

      case StartupDataStep::MARKET:
        fetchCoinbaseStats();
        fetchCoinbaseCandles();
        lastMarketRefresh = millis();
        startupDataStep = StartupDataStep::WEATHER;
        break;

      case StartupDataStep::WEATHER:
        fetchWeatherData();
        startupDataStep = StartupDataStep::DONE;
        break;

      case StartupDataStep::DONE:
        break;
    }

    nextStartupDataMillis = millis() + 1500;
    return;
  }

  if (millis() - lastTickerRefresh >= TICKER_REFRESH_MS) {
    fetchCoinbaseTicker();
    lastTickerRefresh = millis();
  }

  if (millis() - lastMarketRefresh >= MARKET_REFRESH_MS) {
    fetchCoinbaseStats();
    fetchCoinbaseCandles();
    lastMarketRefresh = millis();
  }

  if (millis() - lastNetworkRefresh >= NETWORK_REFRESH_MS) {
    fetchNetworkData();
  }

  if (millis() - lastWeatherRefresh >= WEATHER_REFRESH_MS) {
    fetchWeatherData();
  }
}

void maintainPersistence() {
  if (millis() - lastSaveRefresh >= SAVE_REFRESH_MS) {
    saveStats();
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
  diagnostics.minimumHeap = ESP.getFreeHeap();

  loadPreferences();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  configureWifi();
  configureClock();

  mbedtls_sha256_init(&headerMidstate);
  updateShareTarget(1.0);
  initializeLocalWork();

  currentPage = Page::POOL;
  lastPageChange = millis();
  lastSaveRefresh = millis();
  nextStartupDataMillis = millis() + 1500;

  drawCurrentLayout();
  updateCurrentValues();

  pool.lineBuffer.reserve(4096);
  connectPool();
}

void loop() {
  maintainPool();
  runMiningBatch();

  handleButton();
  maintainPageRotation();
  maintainWifi();
  maintainData();
  maintainPersistence();

  if (pageNeedsDraw) {
    drawCurrentLayout();
    updateCurrentValues();

    if (currentPage == Page::MARKET) {
      drawMarketChart();
    }
  }

  if (currentPage == Page::MARKET && marketChartNeedsDraw) {
    drawMarketChart();
  }

  if (millis() - lastValueRefresh >= VALUE_REFRESH_MS) {
    lastValueRefresh = millis();
    sampleHashrate();
    updateCurrentValues();
  }

  if (millis() - lastSerialJson >= SERIAL_JSON_MS) {
    lastSerialJson = millis();
    emitSerialJson();
  }

  delay(1);
}
