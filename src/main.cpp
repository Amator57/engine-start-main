#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"
#include "webpage.h"

enum State : uint8_t { ST_IDLE = 0, ST_RAMPUP, ST_FULL, ST_RAMPDOWN, ST_FAULT, ST_SHUTDOWN };
volatile uint8_t smState = ST_IDLE;
String faultMsg = "";

struct Settings {
  uint8_t startPct = DEF_START_PCT;
  uint8_t rampTime = DEF_RAMP_TIME;
  bool    softStop = DEF_SOFT_STOP;
  uint8_t maxCurrent = DEF_MAX_CURRENT;
} cfg;

volatile uint16_t levelPercent = 0;
volatile uint32_t firingDelayUs = 0;
volatile uint32_t halfCycleUs = 10000;
volatile uint32_t lastZcMicros = 0;
volatile bool zcSeen = false;
uint32_t delayLut[101];

hw_timer_t* triacTimer = nullptr;

uint32_t rampStartMs = 0;
uint32_t fullSinceMs = 0;
bool relayOn = false;
volatile int8_t relayPendingAction = 0;
uint32_t relayPendingMs = 0;
uint32_t shutdownSinceMs = 0;
bool runHadZc = false;
bool wifiCheckDone = false;
uint32_t wifiBeginMs = 0;

float currentAmps = -1.0f;
uint8_t ocHits = 0;

uint32_t pendingRebootMs = 0;

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
bool apMode = false;
String apSsidName = AP_SSID_PREFIX;

void relayWrite(bool on);

void IRAM_ATTR triacGateIsr() {
  if (levelPercent > 0) digitalWrite(PIN_TRIAC, HIGH);
}

void IRAM_ATTR zcIsr() {
  uint32_t now = micros();
  uint32_t period = now - lastZcMicros;
  lastZcMicros = now;
  zcSeen = true;
  if (period > 4000 && period < 20000) halfCycleUs = (halfCycleUs * 3 + period) >> 2;
  if (relayPendingAction != 0) {
    relayWrite(relayPendingAction > 0);
    relayPendingAction = 0;
  }
  digitalWrite(PIN_TRIAC, LOW);
  if (levelPercent > 0 && firingDelayUs > 0) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    timerWrite(triacTimer, 0);
    timerAlarm(triacTimer, firingDelayUs, false, 0);
#else
    timerWrite(triacTimer, 0);
    timerAlarmWrite(triacTimer, firingDelayUs, false);
    timerAlarmEnable(triacTimer);
#endif
  }
}

void buildLut() {
  for (int p = 1; p <= 100; p++) {
    float f = p / 100.0f;
    float f2 = f * f;
    float lo = 0.0f, hi = (float)M_PI;
    for (int i = 0; i < 40; i++) {
      float mid = 0.5f * (lo + hi);
      float ratio = ((float)M_PI - mid + 0.5f * sinf(2.0f * mid)) / (float)M_PI;
      if (ratio > f2) lo = mid; else hi = mid;
    }
    float alpha = 0.5f * (lo + hi);
    delayLut[p] = (uint32_t)lroundf(alpha * (10000.0f / (float)M_PI));
  }
  delayLut[100] = 100;
}

void relayWrite(bool on) {
  relayOn = on;
#if RELAY_ACTIVE_HIGH
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
#else
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
#endif
}

void setRelay(bool on) {
  if (on == relayOn) return;
  relayPendingAction = on ? 1 : -1;
  relayPendingMs = millis();
}

void setLevel(float pct) {
  int p = (int)lroundf(constrain(pct, 0.0f, 100.0f));
  if (p <= 0) {
    levelPercent = 0;
    firingDelayUs = 0;
    digitalWrite(PIN_TRIAC, LOW);
    return;
  }
  if (p > 100) p = 100;
  uint32_t hc = halfCycleUs;
  uint32_t d = (uint32_t)lroundf(delayLut[p] * (hc / 10000.0f));
  if (d < 100) d = 100;
  uint32_t maxD = (hc > 1500) ? (hc - 1500) : 1000;
  if (d > maxD) d = maxD;
  firingDelayUs = d;
  levelPercent = (uint16_t)p;
}

void allOff() {
  levelPercent = 0;
  firingDelayUs = 0;
  digitalWrite(PIN_TRIAC, LOW);
  setRelay(false);
}

bool mainsPresent() {
  return zcSeen && ((uint32_t)(micros() - lastZcMicros) < 100000);
}

void beginShutdown() {
  if (smState == ST_SHUTDOWN) return;
  setRelay(false);
  runHadZc = false;
  shutdownSinceMs = millis();
  smState = ST_SHUTDOWN;
}

void immediateStop() {
  beginShutdown();
}

void cmdStart() {
  if (smState != ST_IDLE && smState != ST_FAULT) return;
  faultMsg = "";
  ocHits = 0;
  runHadZc = mainsPresent();
  setRelay(false);
  setLevel(cfg.startPct);
  rampStartMs = millis();
  smState = ST_RAMPUP;
}

void cmdStop() {
  if (cfg.softStop && (smState == ST_FULL || smState == ST_RAMPUP)) {
    setLevel(100.0f);
    uint32_t t0 = millis();
    while (millis() - t0 < 60) delay(1);
    setRelay(false);
    rampStartMs = millis();
    smState = ST_RAMPDOWN;
    return;
  }
  immediateStop();
}

void loadSettings() {
  cfg.startPct  = constrain(prefs.getUChar("startPct", DEF_START_PCT), 10, 80);
  cfg.rampTime  = constrain(prefs.getUChar("rampTime", DEF_RAMP_TIME), 1, 60);
  cfg.softStop  = prefs.getBool("softStop", DEF_SOFT_STOP);
  cfg.maxCurrent = constrain(prefs.getUChar("maxCur", DEF_MAX_CURRENT), 0, 40);
}

void saveSettings() {
  prefs.putUChar("startPct", cfg.startPct);
  prefs.putUChar("rampTime", cfg.rampTime);
  prefs.putBool("softStop", cfg.softStop);
  prefs.putUChar("maxCur", cfg.maxCurrent);
}

void startAp() {
  WiFi.mode(WIFI_AP);
  char buf[40];
  snprintf(buf, sizeof(buf), "%s-%04X", AP_SSID_PREFIX, (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
  apSsidName = String(buf);
  WiFi.softAP(apSsidName.c_str(), AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  apMode = true;
  wifiCheckDone = true;
  if (MDNS.begin("softstart")) MDNS.addService("http", "tcp", 80);
  Serial.printf("[WiFi] AP mode: %s pass %s IP %s\n", apSsidName.c_str(), AP_PASS, WiFi.softAPIP().toString().c_str());
}

void initWifi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifiBeginMs = millis();
    wifiCheckDone = false;
    Serial.printf("[WiFi] connecting to %s\n", ssid.c_str());
  } else {
    startAp();
  }
}

void wifiPoll() {
  if (wifiCheckDone) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiCheckDone = true;
    if (MDNS.begin("softstart")) MDNS.addService("http", "tcp", 80);
    Serial.printf("[WiFi] connected, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (millis() - wifiBeginMs > 15000) {
    startAp();
  }
}

void handleStatus() {
  int dispLevel = (smState == ST_FULL) ? 100 : (int)levelPercent;
  float freq = 500000.0f / (float)halfCycleUs;
#if !USE_CURRENT_SENSOR
  currentAmps = -1.0f;
#endif
  String s = "{\"state\":" + String(smState);
  s += ",\"level\":" + String(dispLevel);
  s += ",\"current\":" + String(currentAmps, 1);
  s += ",\"freq\":" + String(freq, 1);
  s += ",\"mains\":" + String(mainsPresent() ? "true" : "false");
  s += ",\"up\":" + String(millis() / 1000UL);
  s += ",\"heap\":" + String(ESP.getFreeHeap());
  s += ",\"rssi\":" + String(apMode ? 0 : WiFi.RSSI());
  s += ",\"ap\":" + String(apMode ? "true" : "false");
  s += ",\"ssid\":\"" + (apMode ? apSsidName : WiFi.SSID()) + "\"";
  s += ",\"fault\":\"" + faultMsg + "\"}";
  server.send(200, "application/json", s);
}

void handleSettingsGet() {
  String s = "{\"startPct\":" + String(cfg.startPct);
  s += ",\"rampTime\":" + String(cfg.rampTime);
  s += ",\"softStop\":" + String(cfg.softStop ? "true" : "false");
  s += ",\"maxCurrent\":" + String(cfg.maxCurrent) + "}";
  server.send(200, "application/json", s);
}

void handleSettingsPost() {
  cfg.startPct   = constrain(server.arg("startPct").toInt(), 10, 80);
  cfg.rampTime   = constrain(server.arg("rampTime").toInt(), 1, 60);
  cfg.maxCurrent = constrain(server.arg("maxCurrent").toInt(), 0, 40);
  cfg.softStop   = server.arg("softStop") == "1";
  saveSettings();
  server.send(200, "text/plain", "OK");
}

void handleWifiPost() {
  String ssid = server.arg("ssid");
  ssid.trim();
  String pass = server.arg("pass");
  if (ssid.length() < 1 || ssid.length() > 32) {
    server.send(400, "text/plain", "Bad SSID");
    return;
  }
  if (pass.length() != 0 && pass.length() < 8) {
    server.send(400, "text/plain", "Password must be 8+ chars");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  pendingRebootMs = millis() + 1000;
  server.send(200, "text/plain", "OK");
}

void initWeb() {
  server.on("/", HTTP_GET, [] {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/start", HTTP_POST, [] {
    cmdStart();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/stop", HTTP_POST, [] {
    cmdStop();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.onNotFound([] {
    if (apMode) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.begin();
  Serial.println("[Web] server started");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nSoftStart 2kW v%s (ESP32-C3)\n", FW_VERSION);

  digitalWrite(PIN_TRIAC, LOW);
  pinMode(PIN_TRIAC, OUTPUT);
#if RELAY_ACTIVE_HIGH
  digitalWrite(PIN_RELAY, LOW);
#else
  digitalWrite(PIN_RELAY, HIGH);
#endif
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_ZC, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if USE_CURRENT_SENSOR
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
#endif

  buildLut();

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  triacTimer = timerBegin(1000000);
  timerAttachInterrupt(triacTimer, &triacGateIsr);
#else
  triacTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(triacTimer, &triacGateIsr, true);
#endif

  attachInterrupt(digitalPinToInterrupt(PIN_ZC), zcIsr, RISING);

  prefs.begin("softstart", false);
  loadSettings();
  Serial.printf("[CFG] startPct=%u%% rampTime=%us softStop=%d maxCur=%uA\n",
                cfg.startPct, cfg.rampTime, (int)cfg.softStop, cfg.maxCurrent);

  initWifi();
  initWeb();
}

void loop() {
  uint32_t now = millis();

  wifiPoll();
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();

  if (relayPendingAction != 0 && now - relayPendingMs > RELAY_ZC_TIMEOUT_MS) {
    relayWrite(relayPendingAction > 0);
    relayPendingAction = 0;
  }

  if (smState != ST_IDLE && smState != ST_SHUTDOWN && mainsPresent()) runHadZc = true;

  if (runHadZc && smState != ST_IDLE && smState != ST_SHUTDOWN &&
      (uint32_t)(micros() - lastZcMicros) > 300000) {
    beginShutdown();
  }

#if AUTO_START_ON_MAINS
  if (smState == ST_IDLE && mainsPresent()) cmdStart();
#endif

  switch (smState) {
    case ST_RAMPUP: {
      uint32_t el = now - rampStartMs;
      uint32_t T = (uint32_t)cfg.rampTime * 1000UL;
      float frac = T ? ((el > T ? T : el) / (float)T) : 1.0f;
      setLevel(cfg.startPct + (100.0f - cfg.startPct) * frac);
      if (el >= T) {
        setLevel(100.0f);
        fullSinceMs = now;
        smState = ST_FULL;
      }
      break;
    }
    case ST_FULL:
      if (!relayOn && now - fullSinceMs >= RELAY_ON_DELAY_MS) {
        setRelay(true);
      }
      break;
    case ST_SHUTDOWN:
      if (!relayOn && relayPendingAction == 0 &&
          now - shutdownSinceMs >= (RELAY_ZC_TIMEOUT_MS + RELAY_OFF_GUARD_MS)) {
        levelPercent = 0;
        firingDelayUs = 0;
        digitalWrite(PIN_TRIAC, LOW);
        smState = ST_IDLE;
      }
      break;
    case ST_RAMPDOWN: {
      uint32_t el = now - rampStartMs;
      uint32_t T = (uint32_t)cfg.rampTime * 1000UL;
      float frac = T ? ((el > T ? T : el) / (float)T) : 1.0f;
      setLevel(100.0f - (100.0f - cfg.startPct) * frac);
      if (el >= T) immediateStop();
      break;
    }
    default:
      break;
  }

#if USE_CURRENT_SENSOR
  static uint32_t lastCurMs = 0;
  if (now - lastCurMs >= 300) {
    lastCurMs = now;
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(PIN_CURRENT);
    mv >>= 2;
    currentAmps = ((float)mv * CURRENT_DIVIDER - CURRENT_MV_ZERO) / CURRENT_MV_PER_A;
    if (currentAmps < 0) currentAmps = 0;
    if (cfg.maxCurrent > 0 && smState != ST_IDLE && smState != ST_FAULT &&
        currentAmps > (float)cfg.maxCurrent) {
      if (++ocHits >= 3) {
        allOff();
        smState = ST_FAULT;
        faultMsg = "Перевантаження: " + String(currentAmps, 1) + " А";
      }
    } else {
      ocHits = 0;
    }
  }
#endif

  static bool btnLast = HIGH;
  static uint32_t btnDownMs = 0;
  bool btn = digitalRead(PIN_BUTTON);
  if (btnLast == HIGH && btn == LOW) btnDownMs = now;
  if (btnLast == LOW && btn == HIGH && now - btnDownMs > 30 && now - btnDownMs < 1500) {
    if (smState == ST_IDLE || smState == ST_FAULT) cmdStart();
    else cmdStop();
  }
  btnLast = btn;

  if (pendingRebootMs && (int32_t)(now - pendingRebootMs) >= 0) {
    delay(200);
    ESP.restart();
  }
}
