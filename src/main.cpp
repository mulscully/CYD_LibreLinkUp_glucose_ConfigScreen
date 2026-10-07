/*
 * CYD (ESP32-2432S028R) Direct LibreLinkUp Glucose Display — v2
 * -----------------------------------------------------------------------------
 * Same as v1, but with no credentials baked into the code at all. On first
 * boot (or whenever you hold the BOOT button while powering on), the device
 * starts its own WiFi access point with a web-based setup page where you
 * enter your home WiFi, LibreLinkUp login, and glucose thresholds. Everything
 * is saved to flash (survives power loss) via ESP32's built-in Preferences
 * storage, so someone downloading this from GitHub can set it up without
 * touching a single line of code.
 *
 * HOW TO SET IT UP (first boot, or after holding BOOT at power-on):
 *   1. The CYD's own screen will show a WiFi network name to join, e.g.
 *      "CYD-Glucose-Setup", and an address to visit.
 *   2. On your phone/computer, join that WiFi network.
 *   3. A setup page should open automatically (or go to 192.168.4.1).
 *   4. Pick your home WiFi, enter your LibreLinkUp email/password and your
 *      low/high glucose thresholds, and submit.
 *   5. The device reboots, connects to your WiFi, and starts showing glucose.
 *
 * TO RECONFIGURE LATER: hold the board's BOOT button down while plugging it
 * in / pressing reset, and the setup page comes back up (your existing
 * settings are pre-filled, so you only need to change what you want).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "mbedtls/md.h"

// How often to refresh (ms).
const unsigned long REFRESH_INTERVAL_MS = 60000;

const int STATUS_BAR_WIDTH = 20;

// BOOT button on most ESP32 dev boards (including the CYD) is wired to GPIO0.
const int BOOT_BUTTON_PIN = 0;

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// ---------------- Runtime config (loaded from flash, or set via the portal) ----------------
String libreEmail;
String librePassword;
int lowThreshold = 90;
int highThreshold = 220;

// ---------------- LibreLinkUp API state ----------------
String baseUrl = "https://api.libreview.io";
String authToken = "";
String accountIdHash = "";
String patientId = "";
String selfUserId = "";
bool loggedIn = false;

// Forward declarations
bool fetchGlucoseViaGraph(const String& id, int& outValue, int& outTrend, String& outTimestamp);
void drawUpdatedLine(bool stale, int minutesAgo);

// ============================================================
// CONFIG PORTAL
// ============================================================

// Shown on the CYD's own screen the moment the config portal starts, so
// someone without a computer handy can still see what WiFi network to join.
void configModeCallback(WiFiManager* myWiFiManager) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Setup Mode", tft.width() / 2, 35, 4);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("1. Join WiFi:", tft.width() / 2, 85, 2);
  tft.drawString(myWiFiManager->getConfigPortalSSID(), tft.width() / 2, 112, 4);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("2. Open in browser:", tft.width() / 2, 160, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("192.168.4.1", tft.width() / 2, 187, 4);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Waiting for setup...", tft.width() / 2, tft.height() - 20, 2);
}

// Loads previously saved settings from flash into our runtime variables, and
// returns them so WiFiManagerParameter fields can be pre-filled with them.
void loadSavedConfig() {
  prefs.begin("glucose-cfg", /*readOnly=*/true);
  libreEmail = prefs.getString("email", "");
  librePassword = prefs.getString("password", "");
  lowThreshold = prefs.getInt("low", 90);
  highThreshold = prefs.getInt("high", 220);
  prefs.end();
}

void saveConfig() {
  prefs.begin("glucose-cfg", /*readOnly=*/false);
  prefs.putString("email", libreEmail);
  prefs.putString("password", librePassword);
  prefs.putInt("low", lowThreshold);
  prefs.putInt("high", highThreshold);
  prefs.end();
}

// Runs the WiFiManager flow: tries saved WiFi first (unless forced), and
// falls back to / is forced into the config portal as needed. Blocks until
// either a successful connection or (on forced portal) a timeout.
void runWiFiManagerSetup(bool forcePortal) {
  loadSavedConfig();

  // WiFiManagerParameter needs char buffers, not Arduino Strings, and the
  // default values must already be set before we construct these, so the
  // portal pre-fills with whatever was last saved.
  static char emailBuf[64];
  static char passwordBuf[64];
  static char lowBuf[6];
  static char highBuf[6];
  libreEmail.toCharArray(emailBuf, sizeof(emailBuf));
  librePassword.toCharArray(passwordBuf, sizeof(passwordBuf));
  snprintf(lowBuf, sizeof(lowBuf), "%d", lowThreshold);
  snprintf(highBuf, sizeof(highBuf), "%d", highThreshold);

  WiFiManagerParameter custom_email("email", "LibreLinkUp Email", emailBuf, 63);
  // The trailing "custom" HTML snippet makes this render as a password field
  // (masked input) instead of plain text.
  WiFiManagerParameter custom_password("password", "LibreLinkUp Password", passwordBuf, 63,
                                        " type='password'");
  WiFiManagerParameter custom_low("low", "Low Threshold (mg/dL)", lowBuf, 5);
  WiFiManagerParameter custom_high("high", "High Threshold (mg/dL)", highBuf, 5);

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.addParameter(&custom_email);
  wm.addParameter(&custom_password);
  wm.addParameter(&custom_low);
  wm.addParameter(&custom_high);

  // Don't block forever in the portal — if nobody configures it within 5
  // minutes, reboot and try again (prevents it getting stuck in AP mode
  // indefinitely if someone walks away).
  wm.setConfigPortalTimeout(300);

  bool connected;
  if (forcePortal) {
    // Keeps existing WiFi/custom settings as pre-filled defaults, but always
    // shows the portal rather than silently reconnecting.
    connected = wm.startConfigPortal("CYD-Glucose-Setup");
  } else {
    // Tries saved WiFi first; only shows the portal if that fails.
    connected = wm.autoConnect("CYD-Glucose-Setup");
  }

  if (!connected) {
    Serial.println("WiFi setup timed out or failed — restarting to try again.");
    ESP.restart();
  }

  // Pull whatever values are now in the fields (either freshly submitted, or
  // still the pre-filled defaults if the portal never ran) and save them.
  libreEmail = String(custom_email.getValue());
  librePassword = String(custom_password.getValue());
  lowThreshold = atoi(custom_low.getValue());
  highThreshold = atoi(custom_high.getValue());
  if (lowThreshold <= 0) lowThreshold = 90;
  if (highThreshold <= 0) highThreshold = 220;

  saveConfig();

  Serial.println("WiFi connected: " + WiFi.localIP().toString());
}

// ============================================================
// LIBRELINKUP API (same logic as v1)
// ============================================================

String sha256hex(const String& input) {
  byte shaResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hash = "";
  for (int i = 0; i < 32; i++) {
    char buf[3];
    sprintf(buf, "%02x", shaResult[i]);
    hash += buf;
  }
  return hash;
}

void addLLUHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("product", "llu.android");
  http.addHeader("version", "4.16.0");
  if (authToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + authToken);
  }
  if (accountIdHash.length() > 0) {
    http.addHeader("Account-Id", accountIdHash);
  }
}

bool login() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/auth/login");
  addLLUHeaders(http);

  StaticJsonDocument<256> body;
  body["email"] = libreEmail;
  body["password"] = librePassword;
  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("Login failed, HTTP code: %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Login JSON parse failed");
    return false;
  }

  bool redirect = doc["data"]["redirect"] | false;
  if (redirect) {
    const char* region = doc["data"]["region"];
    if (region) {
      baseUrl = "https://api-" + String(region) + ".libreview.io";
      Serial.println("Redirected to region: " + baseUrl);
      return login();
    }
  }

  const char* token = doc["data"]["authTicket"]["token"];
  if (!token) {
    Serial.println("No auth token in response");
    return false;
  }
  authToken = String(token);

  const char* userId = doc["data"]["user"]["id"];
  if (userId) {
    selfUserId = String(userId);
    accountIdHash = sha256hex(selfUserId);
  } else {
    Serial.println("WARNING: no user id in login response, account-id header will be missing");
  }

  loggedIn = true;
  Serial.println("Login successful.");
  return true;
}

bool fetchGlucose(int& outValue, int& outTrend, String& outTimestamp) {
  if (!loggedIn) {
    if (!login()) return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/connections");
  addLLUHeaders(http);

  int code = http.GET();
  if (code == 401) {
    Serial.println("Token expired, re-logging in...");
    loggedIn = false;
    http.end();
    if (!login()) return false;
    return fetchGlucose(outValue, outTrend, outTimestamp);
  }
  if (code != 200) {
    Serial.printf("Connections fetch failed, HTTP code: %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Connections JSON parse failed");
    return false;
  }

  JsonArray connections = doc["data"].as<JsonArray>();
  if (connections.size() == 0) {
    Serial.println("No connections found — trying self-monitoring (patient role) fallback...");
    return fetchGlucoseViaGraph(selfUserId, outValue, outTrend, outTimestamp);
  }

  JsonObject chosen = connections[0];
  patientId = chosen["patientId"].as<String>();
  JsonObject gm = chosen["glucoseMeasurement"];
  if (gm.isNull()) {
    Serial.println("No glucoseMeasurement in connection data");
    return false;
  }

  outValue = gm["ValueInMgPerDl"] | gm["Value"].as<int>();
  outTrend = gm["TrendArrow"] | 3;
  outTimestamp = gm["Timestamp"].as<String>();

  return true;
}

bool fetchGlucoseViaGraph(const String& id, int& outValue, int& outTrend, String& outTimestamp) {
  if (id.length() == 0) {
    Serial.println("No self user id available for graph fallback");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/connections/" + id + "/graph");
  addLLUHeaders(http);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Graph fallback fetch failed, HTTP code: %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Graph fallback JSON parse failed");
    return false;
  }

  JsonObject gm = doc["data"]["connection"]["glucoseMeasurement"];
  if (gm.isNull()) {
    Serial.println("No glucoseMeasurement in graph fallback response");
    return false;
  }

  outValue = gm["ValueInMgPerDl"] | gm["Value"].as<int>();
  outTrend = gm["TrendArrow"] | 3;
  outTimestamp = gm["Timestamp"].as<String>();

  return true;
}

// ============================================================
// DISPLAY
// ============================================================

uint16_t statusBarColor(int value) {
  if (value < lowThreshold) return TFT_RED;
  if (value > highThreshold) return TFT_YELLOW;
  return TFT_GREEN;
}

void drawTrendArrow(int trend, int cx, int cy) {
  uint16_t col = TFT_WHITE;
  int size = 14;

  switch (trend) {
    case 1:
      tft.fillTriangle(cx - size, cy - size, cx + size, cy - size, cx, cy + size, col);
      break;
    case 2:
      tft.drawLine(cx - size, cy - size, cx + size, cy + size, col);
      tft.drawLine(cx + size, cy + size, cx + size - 7, cy + size, col);
      tft.drawLine(cx + size, cy + size, cx + size, cy + size - 7, col);
      break;
    case 4:
      tft.drawLine(cx - size, cy + size, cx + size, cy - size, col);
      tft.drawLine(cx + size, cy - size, cx + size - 7, cy - size, col);
      tft.drawLine(cx + size, cy - size, cx + size, cy - size + 7, col);
      break;
    case 5:
      tft.fillTriangle(cx - size, cy + size, cx + size, cy + size, cx, cy - size, col);
      break;
    case 3:
    default:
      tft.drawLine(cx - size, cy, cx + size, cy, col);
      tft.fillTriangle(cx + size, cy, cx + size - 7, cy - 5, cx + size - 7, cy + 5, col);
      break;
  }
}

String updatedLineText(bool stale, int minutesAgo) {
  if (stale) return "STALE DATA";
  if (minutesAgo <= 0) return "Updated just now";
  if (minutesAgo == 1) return "Updated 1 min ago";
  return "Updated " + String(minutesAgo) + " min ago";
}

void drawUpdatedLine(bool stale, int minutesAgo) {
  int contentCenterX = STATUS_BAR_WIDTH + (tft.width() - STATUS_BAR_WIDTH) / 2;
  int y = tft.height() - 20;
  tft.fillRect(STATUS_BAR_WIDTH, y - 12, tft.width() - STATUS_BAR_WIDTH, 24, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(updatedLineText(stale, minutesAgo), contentCenterX, y, 2);
}

void drawGlucoseScreen(int value, int trend, bool stale, int minutesAgo) {
  tft.fillScreen(TFT_BLACK);

  uint16_t barColor = stale ? TFT_DARKGREY : statusBarColor(value);
  tft.fillRect(0, 0, STATUS_BAR_WIDTH, tft.height(), barColor);

  int contentCenterX = STATUS_BAR_WIDTH + (tft.width() - STATUS_BAR_WIDTH) / 2;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("GLUCOSE", contentCenterX, 30, 4);

  int numberY = tft.height() / 2 - 10;
  tft.drawString(String(value), contentCenterX - 25, numberY, 8);

  drawTrendArrow(trend, contentCenterX + 100, numberY);

  tft.drawString("mg/dL", contentCenterX + 40, numberY + 60, 4);

  drawUpdatedLine(stale, minutesAgo);
}

// ============================================================
// MAIN
// ============================================================

unsigned long lastFetch = 0;
unsigned long lastSuccessMillis = 0;
bool haveReading = false;
int lastValue = 0;
int lastTrend = 3;

bool lastDrawnStale = false;
int lastDrawnValue = -1;
int lastDrawnTrend = -1;
int lastDrawnMinutesAgo = -1;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Booting ===");

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // IMPORTANT: we deliberately do NOT check the BOOT button here, before WiFi
  // setup. GPIO0 (BOOT) is a hardware "strapping pin" — if it's held LOW at
  // the exact instant of power-on/reset, the ESP32's own boot ROM enters its
  // flashing/download mode instead of running our firmware at all. Checking
  // it only works safely *after* normal boot has already completed (see the
  // hold-to-reconfigure check in loop() instead).
  runWiFiManagerSetup(false);

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Logging in to LibreLinkUp...", tft.width() / 2, tft.height() / 2, 4);

  login();
  lastFetch = 0;
}

// Checks whether BOOT has been held continuously for a few seconds while the
// device is already running normally (safe — well past the hardware
// strapping window that only matters at the instant of reset/power-on).
// Returns true once the hold threshold is reached.
unsigned long bootHeldSince = 0;
const unsigned long BOOT_HOLD_MS = 3000;

bool checkForReconfigureHold() {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (bootHeldSince == 0) {
      bootHeldSince = millis();
    } else if (millis() - bootHeldSince >= BOOT_HOLD_MS) {
      bootHeldSince = 0; // reset so it doesn't immediately re-trigger
      return true;
    }
  } else {
    bootHeldSince = 0; // released early, cancel the hold
  }
  return false;
}

void loop() {
  if (checkForReconfigureHold()) {
    Serial.println("BOOT held for 3s during normal operation — entering config portal.");
    runWiFiManagerSetup(true);
    // After the portal finishes (or times out), fall through and keep running
    // normally — login()/fetch will pick up any newly saved credentials.
    loggedIn = false; // force a fresh login attempt with whatever was just saved
    lastFetch = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // WiFiManager already handles the initial connection; if it drops later,
    // just let the ESP32's own WiFi stack attempt reconnection automatically
    // (it does this by default), and skip fetching until it's back.
    delay(1000);
    return;
  }

  unsigned long now = millis();
  bool stale = false;

  if (now - lastFetch >= REFRESH_INTERVAL_MS || lastFetch == 0) {
    int value, trend;
    String timestamp;
    bool ok = fetchGlucose(value, trend, timestamp);

    if (ok) {
      lastValue = value;
      lastTrend = trend;
      haveReading = true;
      lastSuccessMillis = now;
      Serial.printf("Glucose: %d mg/dL, trend: %d, time: %s\n", value, trend, timestamp.c_str());
    } else if (!haveReading) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("No data yet...", tft.width() / 2, tft.height() / 2, 4);
      lastFetch = now;
      delay(1000);
      return;
    } else {
      stale = true;
    }

    lastFetch = now;
  }

  if (haveReading) {
    int minutesAgo = (int)((now - lastSuccessMillis) / 60000UL);

    bool needsFullRedraw =
      (lastValue != lastDrawnValue) ||
      (lastTrend != lastDrawnTrend) ||
      (stale != lastDrawnStale);

    if (needsFullRedraw) {
      drawGlucoseScreen(lastValue, lastTrend, stale, minutesAgo);
      lastDrawnValue = lastValue;
      lastDrawnTrend = lastTrend;
      lastDrawnStale = stale;
      lastDrawnMinutesAgo = minutesAgo;
    } else if (minutesAgo != lastDrawnMinutesAgo) {
      drawUpdatedLine(stale, minutesAgo);
      lastDrawnMinutesAgo = minutesAgo;
    }
  }

  delay(1000);
}
