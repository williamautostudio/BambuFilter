#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <cstring>
#include <ctype.h>
#include <esp_system.h>
#include <cstdarg>

#include <cstdio>

// ========= Globals =========
WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);
WiFiManager wifiManager;

bool mqttWasConnected = false;
unsigned long lastMqttAttemptMs = 0;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
wl_status_t lastWifiStatus = WL_IDLE_STATUS;
bool mqttStateDirty = false;
int pendingDutyActiveHigh = 0;
char mqttClientId[32] = "";
int pendingPercentAfterStart = 0;
unsigned long pendingPercentApplyMs = 0;
constexpr uint32_t SOFT_START_SETTLE_MS = 800;

// ========= FWD declarations =========
void loadConfig();
void saveConfig();
void applyConfigToParameters();
bool updateConfigFromParameters();
bool parseBoolParam(const char* value);
int  percentToDuty(int pct);
int  invertDuty(int duty);
void ensureMqtt();
void setupPwm();
void handleRoot();
void handleFanApi();
void handleStatusApi();
void handleReconfig();
void notFound();
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void applyPowerOnPolicy();
void writeDutyActiveLow(int dutyActiveHigh);
void publishStateFromDuty(int dutyActiveHigh);
void publishMqttStatus(const char* status);
void handleFanSpeed(int percent);

// ===== Serial logging helpers =====
template <typename T>
void logPrint(const T& value) { Serial.print(value); }

template <typename T>
void logPrintln(const T& value) { Serial.println(value); }

void logPrintf(const char* fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  Serial.print(buffer);
}

// ========= Config & Parameters =========
Preferences preferences;
struct Config {
  bool mqtt_enabled;                 // NEW: master toggle (default false)
  char mqtt_host[40];
  int  mqtt_port;
  char mqtt_user[40];
  char mqtt_pass[40];
  char mqtt_command_topic[100];
  char mqtt_state_topic[100];
  char mqtt_status_topic[100];
  int  fan_default_speed_pct;
  bool fan_default_on;
} currentConfig;

constexpr int MQTT_HOST_PARAM_LEN   = 40;
constexpr int MQTT_PORT_PARAM_LEN   = 6;
constexpr int MQTT_USER_PARAM_LEN   = 40;
constexpr int MQTT_PASS_PARAM_LEN   = 40;
constexpr int MQTT_TOPIC_PARAM_LEN  = 100;
constexpr int FAN_DEFAULT_SPEED_PARAM_LEN = 4;
constexpr int FAN_DEFAULT_ON_PARAM_LEN    = 6;

// NEW: robust checkbox implementation using a hidden field + UI checkbox synced via JS
// Hidden field actually submitted to WiFiManager (value '1' or '0')
WiFiManagerParameter custom_mqtt_enable_hidden("use_mqtt", "", "0", 2, "type='hidden'");
// UI block with checkbox and a small script to mirror to hidden field
WiFiManagerParameter custom_mqtt_enable_ui(
  "<div style='margin:8px 0;display:flex;align-items:center;gap:8px;'><input type='checkbox' id='use_mqtt_cb'><span>Enable MQTT</span></div>"
  "<script>(function(){var cb=document.getElementById('use_mqtt_cb');var hid=document.getElementById('use_mqtt');if(!cb||!hid)return;cb.checked=(hid.value==='1');cb.addEventListener('change',function(){hid.value=cb.checked?'1':'0';});})();</script>"
);

// Non‑MQTT parameters first (so MQTT block can be placed at the very bottom of the portal)
WiFiManagerParameter custom_fan_def_spd("fspd", "Fan Default Speed (15-100)", "", FAN_DEFAULT_SPEED_PARAM_LEN);
WiFiManagerParameter custom_fan_def_on ("fdon", "Fan Default ON (true/false)", "", FAN_DEFAULT_ON_PARAM_LEN);

// MQTT parameters (grouped together, added last in setup so they appear at the bottom)
WiFiManagerParameter custom_mqtt_header("<hr><h3>MQTT Settings</h3>");
WiFiManagerParameter custom_mqtt_host("mqtt_host", "MQTT Server", "", MQTT_HOST_PARAM_LEN);
WiFiManagerParameter custom_mqtt_port("mqtt_port", "MQTT Port", "", MQTT_PORT_PARAM_LEN);
WiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT User", "", MQTT_USER_PARAM_LEN);
WiFiManagerParameter custom_mqtt_pass("mqtt_pass", "MQTT Pass", "", MQTT_PASS_PARAM_LEN, "type='password'");
WiFiManagerParameter custom_mqtt_cmd_topic   ("cmdtopic",    "MQTT Command Topic (max 100)", "", MQTT_TOPIC_PARAM_LEN);
WiFiManagerParameter custom_mqtt_state_topic ("statetopic",  "MQTT State Topic (max 100)",   "", MQTT_TOPIC_PARAM_LEN);
WiFiManagerParameter custom_mqtt_status_topic("statustopic", "MQTT Status Topic (max 100)",  "", MQTT_TOPIC_PARAM_LEN);

// ========= PWM / Fan runtime =========
static const int  FAN_PWM_PIN    = 10;
static const int  LEDC_CHANNEL   = 0;
static const int  LEDC_TIMER     = 0;
static const uint32_t PWM_FREQ_HZ = 25000;
static const uint8_t  PWM_RES_BITS = 10;

static const int PCT_MIN_START = 25;
static const int PCT_MIN_RUN   = 15;

int currentDuty = 0;
int currentPercent = 0;
int lastUserPercent = 0;

// ========= Config I/O =========
void loadConfig() {
  preferences.begin("fan-control", false);
  currentConfig.mqtt_enabled = preferences.getBool("mqtt_enabled", false);  // default false

  preferences.getString("mqtt_host", currentConfig.mqtt_host, sizeof(currentConfig.mqtt_host));
  currentConfig.mqtt_port = preferences.getInt("mqtt_port", 1883);
  preferences.getString("mqtt_user", currentConfig.mqtt_user, sizeof(currentConfig.mqtt_user));
  preferences.getString("mqtt_pass", currentConfig.mqtt_pass, sizeof(currentConfig.mqtt_pass));

  logPrintf("[%.3f ms] loadConfig: loaded mqtt_user='%s' (len: %d), mqtt_pass='%s' (len: %d)\n",
            millis() / 1000.0f,
            currentConfig.mqtt_user, strlen(currentConfig.mqtt_user),
            currentConfig.mqtt_pass, strlen(currentConfig.mqtt_pass));

  preferences.getString("cmd_topic",    currentConfig.mqtt_command_topic, sizeof(currentConfig.mqtt_command_topic));
  preferences.getString("state_topic",  currentConfig.mqtt_state_topic,   sizeof(currentConfig.mqtt_state_topic));
  preferences.getString("status_topic", currentConfig.mqtt_status_topic,  sizeof(currentConfig.mqtt_status_topic));
  currentConfig.fan_default_speed_pct = preferences.getInt("fan_def_spd", 50);
  currentConfig.fan_default_on        = preferences.getBool("fan_def_on", true);
  preferences.end();

  if (strlen(currentConfig.mqtt_host) == 0) {
    strncpy(currentConfig.mqtt_host, "192.168.2.231", sizeof(currentConfig.mqtt_host));
    currentConfig.mqtt_host[sizeof(currentConfig.mqtt_host) - 1] = '\0';
  }
  if (strlen(currentConfig.mqtt_command_topic) == 0) {
    strncpy(currentConfig.mqtt_command_topic, "bambu/p1s/fan/cmd", sizeof(currentConfig.mqtt_command_topic));
    currentConfig.mqtt_command_topic[sizeof(currentConfig.mqtt_command_topic) - 1] = '\0';
  }
  if (strlen(currentConfig.mqtt_state_topic) == 0) {
    strncpy(currentConfig.mqtt_state_topic, "bambu/p1s/fan/state", sizeof(currentConfig.mqtt_state_topic));
    currentConfig.mqtt_state_topic[sizeof(currentConfig.mqtt_state_topic) - 1] = '\0';
  }
  if (strlen(currentConfig.mqtt_status_topic) == 0) {
    strncpy(currentConfig.mqtt_status_topic, "bambu/p1s/fan/status", sizeof(currentConfig.mqtt_status_topic));
    currentConfig.mqtt_status_topic[sizeof(currentConfig.mqtt_status_topic) - 1] = '\0';
  }
  if (strlen(currentConfig.mqtt_user) == 0) currentConfig.mqtt_user[0] = '\0';
  if (strlen(currentConfig.mqtt_pass) == 0) currentConfig.mqtt_pass[0] = '\0';

  currentConfig.fan_default_speed_pct = constrain(currentConfig.fan_default_speed_pct, 0, 100);
  if (currentConfig.fan_default_speed_pct > 0 && currentConfig.fan_default_speed_pct < PCT_MIN_RUN) {
    currentConfig.fan_default_speed_pct = PCT_MIN_RUN;
  }

  lastUserPercent = currentConfig.fan_default_speed_pct;
  applyConfigToParameters();
}

void saveConfig() {
  float t = millis() / 1000.0f;
  preferences.begin("fan-control", false);

  // Snapshot old values
  bool   old_mqtt_enabled = preferences.getBool("mqtt_enabled", false);
  String old_mqtt_host    = preferences.getString("mqtt_host", "");
  int    old_mqtt_port    = preferences.getInt("mqtt_port", 1883);
  String old_mqtt_user    = preferences.getString("mqtt_user", "");
  String old_mqtt_pass    = preferences.getString("mqtt_pass", "");
  String old_cmd_topic    = preferences.getString("cmd_topic", "");
  String old_state_topic  = preferences.getString("state_topic", "");
  String old_status_topic = preferences.getString("status_topic", "");
  int    old_def_spd      = preferences.getInt("fan_def_spd", 50);
  bool   old_def_on       = preferences.getBool("fan_def_on", true);

  // Write new values
  preferences.putBool  ("mqtt_enabled", currentConfig.mqtt_enabled);
  preferences.putString("mqtt_host",    currentConfig.mqtt_host);
  preferences.putInt   ("mqtt_port",    currentConfig.mqtt_port);
  preferences.putString("mqtt_user",    currentConfig.mqtt_user);
  preferences.putString("mqtt_pass",    currentConfig.mqtt_pass);
  preferences.putString("cmd_topic",    currentConfig.mqtt_command_topic);
  preferences.putString("state_topic",  currentConfig.mqtt_state_topic);
  preferences.putString("status_topic", currentConfig.mqtt_status_topic);
  preferences.putInt   ("fan_def_spd", currentConfig.fan_default_speed_pct);
  preferences.putBool  ("fan_def_on",  currentConfig.fan_default_on);

  // Log only what changed (mask secrets)
  if (old_mqtt_enabled != currentConfig.mqtt_enabled)
    logPrintf("[%.3f s] NVS updated: mqtt_enabled: %s -> %s\n", t,
              old_mqtt_enabled ? "true" : "false",
              currentConfig.mqtt_enabled ? "true" : "false");

  if (old_mqtt_host != currentConfig.mqtt_host)
    logPrintf("[%.3f s] NVS updated: mqtt_host: '%s' -> '%s'\n", t,
              old_mqtt_host.c_str(), currentConfig.mqtt_host);

  if (old_mqtt_port != currentConfig.mqtt_port)
    logPrintf("[%.3f s] NVS updated: mqtt_port: %d -> %d\n", t,
              old_mqtt_port, currentConfig.mqtt_port);

  if (old_mqtt_user != currentConfig.mqtt_user)
    logPrintf("[%.3f s] NVS updated: mqtt_user: '%s' -> '%s'\n", t,
              old_mqtt_user.c_str(), currentConfig.mqtt_user);

  if ((int)old_mqtt_pass.length() != (int)strlen(currentConfig.mqtt_pass))
    logPrintf("[%.3f s] NVS updated: mqtt_pass length: %d -> %d\n", t,
              (int)old_mqtt_pass.length(), (int)strlen(currentConfig.mqtt_pass));

  if (old_cmd_topic != currentConfig.mqtt_command_topic)
    logPrintf("[%.3f s] NVS updated: cmd_topic: '%s' -> '%s'\n", t,
              old_cmd_topic.c_str(), currentConfig.mqtt_command_topic);

  if (old_state_topic != currentConfig.mqtt_state_topic)
    logPrintf("[%.3f s] NVS updated: state_topic: '%s' -> '%s'\n", t,
              old_state_topic.c_str(), currentConfig.mqtt_state_topic);

  if (old_status_topic != currentConfig.mqtt_status_topic)
    logPrintf("[%.3f s] NVS updated: status_topic: '%s' -> '%s'\n", t,
              old_status_topic.c_str(), currentConfig.mqtt_status_topic);

  if (old_def_spd != currentConfig.fan_default_speed_pct)
    logPrintf("[%.3f s] NVS updated: fan_def_spd: %d -> %d\n", t,
              old_def_spd, currentConfig.fan_default_speed_pct);

  if (old_def_on != currentConfig.fan_default_on)
    logPrintf("[%.3f s] NVS updated: fan_def_on: %s -> %s\n", t,
              old_def_on ? "true" : "false",
              currentConfig.fan_default_on ? "true" : "false");

  preferences.end();
}

void applyConfigToParameters() {
  // Reflect mqtt_enabled into the hidden field (UI checkbox is synced by JS)
  custom_mqtt_enable_hidden.setValue(currentConfig.mqtt_enabled ? "1" : "0", 2);

  // Non‑MQTT first
  int safePct = constrain(currentConfig.fan_default_speed_pct, 0, 100);
  if (safePct > 0 && safePct < PCT_MIN_RUN) safePct = PCT_MIN_RUN;
  char speedBuffer[FAN_DEFAULT_SPEED_PARAM_LEN];
  snprintf(speedBuffer, sizeof(speedBuffer), "%d", safePct);
  custom_fan_def_spd.setValue(speedBuffer, FAN_DEFAULT_SPEED_PARAM_LEN);
  custom_fan_def_on.setValue(currentConfig.fan_default_on ? "true" : "false", FAN_DEFAULT_ON_PARAM_LEN);

  // MQTT block
  custom_mqtt_host.setValue(currentConfig.mqtt_host, MQTT_HOST_PARAM_LEN);
  custom_mqtt_user.setValue(currentConfig.mqtt_user, MQTT_USER_PARAM_LEN);
  custom_mqtt_pass.setValue(currentConfig.mqtt_pass, MQTT_PASS_PARAM_LEN);
  custom_mqtt_cmd_topic.setValue  (currentConfig.mqtt_command_topic, MQTT_TOPIC_PARAM_LEN);
  custom_mqtt_state_topic.setValue(currentConfig.mqtt_state_topic,   MQTT_TOPIC_PARAM_LEN);
  custom_mqtt_status_topic.setValue(currentConfig.mqtt_status_topic, MQTT_TOPIC_PARAM_LEN);
  char portBuffer[MQTT_PORT_PARAM_LEN];
  snprintf(portBuffer, sizeof(portBuffer), "%d", currentConfig.mqtt_port);
  custom_mqtt_port.setValue(portBuffer, MQTT_PORT_PARAM_LEN);
}

bool parseBoolParam(const char* value) {
  if (!value) return false;
  while (*value && isspace(static_cast<unsigned char>(*value))) value++;
  if (*value == '\0') return false;
  char c = static_cast<char>(tolower(static_cast<unsigned char>(*value)));
  if (c == '1' || c == 't' || c == 'y') return true;
  if (c == '0' || c == 'f' || c == 'n') return false;
  if (c == 'o' && value[1] && tolower(static_cast<unsigned char>(value[1])) == 'n') return true; // 'on'
  return false;
}

bool updateConfigFromParameters() {
  logPrintf("[%.3f ms] Entering updateConfigFromParameters()\n", millis() / 1000.0f);
  Config newConfig = currentConfig;

  auto safeCopy = [](char* dest, size_t size, const char* src) {
    if (!src) src = "";
    strncpy(dest, src, size);
    dest[size - 1] = '\0';
  };

  // Checkbox first
  newConfig.mqtt_enabled = parseBoolParam(custom_mqtt_enable_hidden.getValue());

  // MQTT fields
  safeCopy(newConfig.mqtt_host, sizeof(newConfig.mqtt_host), custom_mqtt_host.getValue());
  safeCopy(newConfig.mqtt_user, sizeof(newConfig.mqtt_user), custom_mqtt_user.getValue());
  safeCopy(newConfig.mqtt_pass, sizeof(newConfig.mqtt_pass), custom_mqtt_pass.getValue());
  safeCopy(newConfig.mqtt_command_topic, sizeof(newConfig.mqtt_command_topic), custom_mqtt_cmd_topic.getValue());
  safeCopy(newConfig.mqtt_state_topic,   sizeof(newConfig.mqtt_state_topic),   custom_mqtt_state_topic.getValue());
  safeCopy(newConfig.mqtt_status_topic,  sizeof(newConfig.mqtt_status_topic),  custom_mqtt_status_topic.getValue());

  const char* portValue = custom_mqtt_port.getValue();
  if (portValue && strlen(portValue) > 0) {
    int port = atoi(portValue);
    if (port > 0 && port <= 65535) newConfig.mqtt_port = port;
  } else {
    newConfig.mqtt_port = 1883;
  }

  // Non‑MQTT: default speed and ON flag
  const char* speedValue = custom_fan_def_spd.getValue();
  if (speedValue && strlen(speedValue) > 0) {
    newConfig.fan_default_speed_pct = constrain(atoi(speedValue), 0, 100);
  }
  if (newConfig.fan_default_speed_pct > 0 && newConfig.fan_default_speed_pct < PCT_MIN_RUN) {
    newConfig.fan_default_speed_pct = PCT_MIN_RUN;
  }
  newConfig.fan_default_on = parseBoolParam(custom_fan_def_on.getValue());

  bool changed =
    newConfig.mqtt_enabled != currentConfig.mqtt_enabled ||
    strcmp(newConfig.mqtt_host, currentConfig.mqtt_host) != 0 ||
    strcmp(newConfig.mqtt_user, currentConfig.mqtt_user) != 0 ||
    strcmp(newConfig.mqtt_pass, currentConfig.mqtt_pass) != 0 ||
    strcmp(newConfig.mqtt_command_topic, currentConfig.mqtt_command_topic) != 0 ||
    strcmp(newConfig.mqtt_state_topic,   currentConfig.mqtt_state_topic)   != 0 ||
    strcmp(newConfig.mqtt_status_topic,  currentConfig.mqtt_status_topic)  != 0 ||
    newConfig.mqtt_port != currentConfig.mqtt_port ||
    newConfig.fan_default_speed_pct != currentConfig.fan_default_speed_pct ||
    newConfig.fan_default_on != currentConfig.fan_default_on;

  if (changed) {
    currentConfig = newConfig;
    lastUserPercent = newConfig.fan_default_speed_pct > 0 ? newConfig.fan_default_speed_pct : 0;
  }

  applyConfigToParameters();
  return changed;
}

// ========= PWM helpers =========
int percentToDuty(int pct) {
  pct = constrain(pct, 0, 100);
  if (pct > 0 && pct < PCT_MIN_RUN) pct = PCT_MIN_RUN;
  int duty = (int)round((pct / 100.0f) * ((1 << PWM_RES_BITS) - 1));
  return constrain(duty, 0, (1 << PWM_RES_BITS) - 1);
}

int invertDuty(int duty) {
  const int DUTY_MAX = (1 << PWM_RES_BITS) - 1;
  return DUTY_MAX - constrain(duty, 0, DUTY_MAX);
}

void writeDutyActiveLow(int dutyActiveHigh) {
  int dutyActiveLow = invertDuty(dutyActiveHigh);
  ledcWrite(LEDC_CHANNEL, dutyActiveLow);
  currentDuty = dutyActiveHigh;
}

// ========= MQTT‑aware publishers =========
void publishStateFromDuty(int dutyActiveHigh) {
  if (!currentConfig.mqtt_enabled) return; // MQTT disabled => no publish

  if (!mqtt.connected()) {
    pendingDutyActiveHigh = dutyActiveHigh;
    mqttStateDirty = true;
    ensureMqtt();
    if (!mqtt.connected()) {
      return;
    }
  }

  const int DUTY_MAX = (1 << PWM_RES_BITS) - 1;
  float percent = 100.0f * dutyActiveHigh / DUTY_MAX;
  int setpoint = constrain(lastUserPercent, 0, 100);
  char payload[160];
  snprintf(payload, sizeof(payload), "{\"duty\":%d,\"percent\":%.1f,\"setpoint\":%d}", dutyActiveHigh, percent, setpoint);

  if (!mqtt.publish(currentConfig.mqtt_state_topic, payload, true)) {
    pendingDutyActiveHigh = dutyActiveHigh;
    mqttStateDirty = true;
    return;
  }
  mqttStateDirty = false;
  mqtt.loop();
}

void publishMqttStatus(const char* status) {
  if (!currentConfig.mqtt_enabled) return;
  if (!mqtt.connected() || status == nullptr) return;
  mqtt.publish(currentConfig.mqtt_status_topic, status, true);
  mqtt.loop();
}

// ========= Fan control =========
bool tryParseInt(const char* s, int& out) {
  char* endp = nullptr;
  long v = strtol(s, &endp, 10);
  if (endp == s || *endp != '\0') return false;
  out = (int)v;
  return true;
}

bool tryParseJsonPercent(const String& s, int& outPercent) {
  int idx = s.indexOf("speed");
  if (idx < 0) idx = s.indexOf("percent");
  if (idx < 0) return false;
  int colon = s.indexOf(':', idx);
  if (colon < 0) return false;
  int j = colon + 1;
  while (j < (int)s.length() && isspace((unsigned char)s[j])) j++;
  int start = j;
  while (j < (int)s.length() && (isdigit((unsigned char)s[j]) || s[j] == '.')) j++;
  String numStr = s.substring(start, j);
  outPercent = (int)round(numStr.toFloat());
  return true;
}

bool parseSpeedCommand(const char* payload, int& outPercent) {
  String s = String(payload);
  s.trim();

  if (s.startsWith("RAW:") || s.startsWith("raw:")) {
    int v;
    if (tryParseInt(s.substring(4).c_str(), v)) {
      const int DUTY_MAX = (1 << PWM_RES_BITS) - 1;
      outPercent = (int)round(100.0f * v / DUTY_MAX);
      return true;
    }
    return false;
  }

  if (s.startsWith("{") && s.endsWith("}")) {
    int pct;
    if (tryParseJsonPercent(s, pct)) {
      outPercent = constrain(pct, 0, 100);
      return true;
    }
    return false;
  }

  int val;
  if (tryParseInt(s.c_str(), val)) {
    if (val <= 100) {
      outPercent = constrain(val, 0, 100);
    } else {
      const int DUTY_MAX = (1 << PWM_RES_BITS) - 1;
      outPercent = (int)round(100.0f * constrain(val, 0, DUTY_MAX) / DUTY_MAX);
    }
    return true;
  }

  return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!currentConfig.mqtt_enabled) return;
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  int percent;
  if (parseSpeedCommand(msg.c_str(), percent)) {
    handleFanSpeed(percent);
  }
}

void ensureMqtt() {
  if (!currentConfig.mqtt_enabled) return; // respect the toggle

  if (mqtt.connected()) {
    if (!mqttWasConnected) {
      mqttWasConnected = true;
      logPrintf("[%lu ms] MQTT connected & subscribed.\n", millis());
    }
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttemptMs = now;

  wl_status_t wifiStatus = WiFi.status();
  logPrintf("[%lu ms] ensureMqtt: wifi=%d, mqtt.connected=%d\n", now, wifiStatus, mqtt.connected());

  if (wifiStatus != WL_CONNECTED) {
    logPrintf("[%lu ms] WiFi not connected, skipping MQTT reconnect\n", now);
    mqttWasConnected = false;
    return;
  }

  if (mqttWasConnected) {
    logPrintf("[%lu ms] MQTT disconnected, retrying...\n", now);
    mqttWasConnected = false;
  }

  if (mqttClientId[0] == '\0') {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(mqttClientId, sizeof(mqttClientId), "xiao-%02X%02X%02X%02X%02X%02X",
             (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
             (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
  }

  mqtt.setServer(currentConfig.mqtt_host, currentConfig.mqtt_port);
  mqtt.setCallback(mqttCallback);

  logPrintf("[%lu ms] Attempting MQTT connect. Host: %s, Port: %d, User: '%s' (len: %d), Pass: '%s' (len: %d)\n",
            now, currentConfig.mqtt_host, currentConfig.mqtt_port,
            currentConfig.mqtt_user, strlen(currentConfig.mqtt_user),
            currentConfig.mqtt_pass, strlen(currentConfig.mqtt_pass));

  bool connect_success = mqtt.connect(
    mqttClientId,
    currentConfig.mqtt_user,
    currentConfig.mqtt_pass,
    currentConfig.mqtt_status_topic, 1, true, "offline");

  if (!connect_success) {
    logPrintf("[%lu ms] MQTT connection failed, rc=%d\n", now, mqtt.state());
    return;
  }

  mqttWasConnected = true;
  publishMqttStatus("online");
  mqtt.subscribe(currentConfig.mqtt_command_topic, 1);
  if (mqttStateDirty) {
    publishStateFromDuty(pendingDutyActiveHigh);
  } else {
    publishStateFromDuty(currentDuty);
  }
  logPrintf("[%lu ms] MQTT connected & subscribed.\n", millis());
}

void handleFanSpeed(int percent) {
  int requested = constrain(percent, 0, 100);
  int effective = requested;
  if (effective > 0 && effective < PCT_MIN_RUN) effective = PCT_MIN_RUN;

  if (currentConfig.mqtt_enabled && !mqtt.connected()) {
    ensureMqtt();
  }

  bool softStart = false;
  if (currentDuty == 0 && effective > 0 && effective < PCT_MIN_START) {
    softStart = true;
    pendingPercentAfterStart = max(requested, PCT_MIN_RUN);
    pendingPercentApplyMs = millis() + SOFT_START_SETTLE_MS;
    effective = PCT_MIN_START;
  } else {
    pendingPercentApplyMs = 0;
  }

  writeDutyActiveLow(percentToDuty(effective));

  const int DUTY_MAX = (1 << PWM_RES_BITS) - 1;
  currentPercent = (effective == 0) ? 0 : (int)round((100.0f * currentDuty) / DUTY_MAX);
  if (requested > 0) {
    int stored = max(requested, PCT_MIN_RUN);
    lastUserPercent = stored;
  }

  if (requested == 0) {
    pendingPercentAfterStart = 0;
  }

  if (softStart) {
    publishStateFromDuty(percentToDuty(pendingPercentAfterStart));
  } else {
    publishStateFromDuty(currentDuty);
  }
}

// ========= HTTP / UI =========
String getFanStateJson() {
  String json = "{";
  bool isOn = currentPercent > 0;
  json += "\"status\":\"" + String(isOn ? "on" : "off") + "\",";
  json += "\"speed\":" + String(currentPercent);
  json += ",\"setpoint\":" + String(constrain(lastUserPercent, 0, 100));
  json += ",\"default_on\":" + String(currentConfig.fan_default_on ? "true" : "false");
  json += "}";
  return json;
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Bambu Fan Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 20px; }
    .container { max-width: 400px; margin: auto; padding: 20px; border: 1px solid #ccc; border-radius: 8px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
    button { padding: 10px 20px; margin: 10px; font-size: 16px; cursor: pointer; border: none; border-radius: 5px; transition: background-color 0.2s ease; }
    #fanStatus { font-size: 20px; margin: 15px 0; }
    #speedSlider { width: 80%; margin: 15px 0; }
    .btn-on { background-color: #4CAF50; color: white; }
    .btn-off { background-color: #f44336; color: white; }
    .btn-on.active { background-color: #2e7d32; }
    .btn-on.inactive { background-color: #a5d6a7; }
    .btn-off.active { background-color: #c62828; }
    .btn-off.inactive { background-color: #ef9a9a; }
    .btn-reconfig { background-color: #008CBA; color: white; }
    button:disabled { cursor: default; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Bambu Fan Control</h1>
    <div id="fanStatus">Fan Status: -- Speed: --%</div>

    <!-- New: power-on default toggle -->
    <div style="margin:10px 0;">
      <label><input type="checkbox" id="defaultOnToggle"> Default ON at power-up</label>
    </div>

    <button id="btnOn" class="btn-on inactive" onclick="setFanState(true)">Turn On</button>
    <button id="btnOff" class="btn-off inactive" onclick="setFanState(false)">Turn Off</button>

    <p>Fan Speed:</p>
    <input type="range" min="0" max="100" value="0" class="slider" id="speedSlider">
    <p><span id="speedValue">0</span>%</p>

    <button class="btn-reconfig" onclick="reconfigure()">Reconfigure WiFi/MQTT</button>
  </div>

  <script>
    var fanStateElement = document.getElementById('fanStatus');
    var speedSlider = document.getElementById('speedSlider');
    var speedValueElement = document.getElementById('speedValue');
    var btnOn = document.getElementById('btnOn');
    var btnOff = document.getElementById('btnOff');
    var sliderDebounce = null;
    var lastSetpoint = 0;
    var defaultOnToggle = document.getElementById('defaultOnToggle');

    function clampPercent(value) {
      var n = parseInt(value, 10);
      if (isNaN(n) || !isFinite(n)) { return 0; }
      if (n < 0) return 0; if (n > 100) return 100; return n;
    }

    function applyButtonState(isOn) {
      if (isOn) {
        btnOn.disabled = true;  btnOn.classList.add('inactive'); btnOn.classList.remove('active');
        btnOff.disabled = false; btnOff.classList.add('active');  btnOff.classList.remove('inactive');
      } else {
        btnOn.disabled = false; btnOn.classList.add('active');  btnOn.classList.remove('inactive');
        btnOff.disabled = true;  btnOff.classList.add('inactive'); btnOff.classList.remove('active');
      }
    }

    function statusText(isOn, speed, setpoint) {
      var text = "Fan Status: " + (isOn ? "On" : "Off") + " Speed: " + speed + "%";
      if (!isOn && setpoint !== speed) { text += " (Set: " + setpoint + "%)"; }
      return text;
    }

    function applyUiState(response) {
      var isOn = response.status === "on";
      var speed = clampPercent(response.speed);
      var setpoint = response.setpoint !== undefined ? clampPercent(response.setpoint) : speed;
      if (isOn) { lastSetpoint = speed; } else { lastSetpoint = setpoint; }
      fanStateElement.textContent = statusText(isOn, speed, setpoint);
      if (isOn) { speedSlider.value = speed; speedValueElement.textContent = speed; }
      else { speedSlider.value = setpoint; speedValueElement.textContent = setpoint; }
      applyButtonState(isOn);
      // reflect default_on flag
      if (defaultOnToggle) { defaultOnToggle.checked = !!response.default_on; }
    }

    function fetchStatus() {
      var xhr = new XMLHttpRequest();
      xhr.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
          try { var response = JSON.parse(this.responseText); applyUiState(response); }
          catch (e) { console.error('Invalid status payload', e); }
        }
      };
      xhr.open('GET', '/status', true); xhr.send();
    }

    function setFanState(isOn) {
      if ((isOn && btnOn.disabled) || (!isOn && btnOff.disabled)) return;
      var state = isOn ? 'on' : 'off';
      applyButtonState(isOn);
      var value = clampPercent(lastSetpoint);
      speedSlider.value = value; speedValueElement.textContent = value;
      var xhr = new XMLHttpRequest();
      xhr.onreadystatechange = function() { if (this.readyState === 4) { fetchStatus(); } };
      xhr.open('GET', '/fan?state=' + state, true); xhr.send();
    }

    function sendFanSpeed(speed) {
      var value = clampPercent(speed); lastSetpoint = value;
      var xhr = new XMLHttpRequest();
      xhr.onreadystatechange = function() { if (this.readyState === 4) { fetchStatus(); } };
      xhr.open('GET', '/fan?speed=' + value, true); xhr.send();
    }

    speedSlider.addEventListener('input', function() {
      var value = clampPercent(this.value); speedValueElement.textContent = value;
      if (sliderDebounce) { clearTimeout(sliderDebounce); }
      sliderDebounce = setTimeout(function() { sendFanSpeed(value); }, 80);
    });
    speedSlider.addEventListener('change', function() { var value = clampPercent(this.value); sendFanSpeed(value); });

    function reconfigure() {
      if (confirm('Reconfigure WiFi/MQTT? The ESP32 will restart into configuration mode.')) {
        var xhr = new XMLHttpRequest(); xhr.open('GET', '/reconfig', true); xhr.send();
      }
    }

    // New: toggle handler to persist default power-on behavior
    if (defaultOnToggle) {
      defaultOnToggle.addEventListener('change', function(){
        var xhr = new XMLHttpRequest();
        xhr.onreadystatechange = function(){ if (this.readyState===4) { fetchStatus(); } };
        xhr.open('GET', '/fan?default_on=' + (defaultOnToggle.checked ? 'true':'false'), true);
        xhr.send();
      });
    }

    setInterval(fetchStatus, 1500);
    fetchStatus();
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleFanApi() {
  // New: allow toggling power-on default via UI
  if (server.hasArg("default_on")) {
    bool v = parseBoolParam(server.arg("default_on").c_str());
    currentConfig.fan_default_on = v;
    saveConfig();
  }

  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "on") {
      int target = lastUserPercent > 0 ? lastUserPercent : currentConfig.fan_default_speed_pct;
      handleFanSpeed(target);
    } else if (state == "off") {
      handleFanSpeed(0);
    }
  } else if (server.hasArg("speed")) {
    int requested = constrain(server.arg("speed").toInt(), 0, 100);
    int stored = requested > 0 ? max(requested, PCT_MIN_RUN) : 0;
    if (stored > 0) {
      lastUserPercent = stored;
      currentConfig.fan_default_speed_pct = lastUserPercent; // persist last setpoint
      saveConfig();
    }

    if (currentDuty == 0 && currentPercent == 0) {
      pendingPercentAfterStart = 0;
      pendingPercentApplyMs = 0;
      publishStateFromDuty(currentDuty); // just report setpoint if stopped
    } else {
      handleFanSpeed(requested);
    }
  }
  server.send(200, "application/json", getFanStateJson());
}

void handleStatusApi() {
  server.send(200, "application/json", getFanStateJson());
}

void handleReconfig() {
  server.send(200, "text/plain", "ESP32 restarting to enter config mode...");
  server.stop();
  if (mqtt.connected()) {
    publishMqttStatus("offline");
    mqtt.disconnect();
    mqttWasConnected = false;
  }
  lastMqttAttemptMs = millis();
  delay(50);

  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  delay(50);
  WiFi.mode(WIFI_AP_STA);

  applyConfigToParameters();

  bool portalResult = wifiManager.startConfigPortal("BambuFanAP", "password");
  logPrintf("[%.3f ms] handleReconfig(): portalResult = %d\n", millis() / 1000.0f, portalResult);
  if (!portalResult) {
    logPrintln("Config portal closed without station connection.");
  }

  delay(200);
  ESP.restart();
}

void notFound() { server.send(404, "text/plain", "Not found"); }

void configModeCallback(WiFiManager *myWiFiManager) {
  logPrintln("Entered config mode");
  logPrint("AP SSID: "); logPrintln(myWiFiManager->getConfigPortalSSID());
  logPrint("AP IP address: "); logPrintln(WiFi.softAPIP());
}

void saveConfigCallback() {
  logPrintf("[%.3f ms] Entering saveConfigCallback()\n", millis() / 1000.0f);
  bool configUpdated = updateConfigFromParameters();
  saveConfig();
  logPrintf("[%.3f ms] saveConfigCallback(): updateConfigFromParameters=%d, saved to NVS\n",
            millis() / 1000.0f, configUpdated);
}

void applyPowerOnPolicy() {
  lastUserPercent = constrain(currentConfig.fan_default_speed_pct, 0, 100);
  if (lastUserPercent > 0 && lastUserPercent < PCT_MIN_RUN) lastUserPercent = PCT_MIN_RUN;
  if (currentConfig.fan_default_on) {
    handleFanSpeed(lastUserPercent);
  } else {
    handleFanSpeed(0);
  }
}

void setupPwm() {
#if defined(ARDUINO_ESP32C3_DEV)
  ledcAttachPin(FAN_PWM_PIN, LEDC_CHANNEL);
  ledcSetup(LEDC_CHANNEL, PWM_FREQ_HZ, PWM_RES_BITS);
#else
  ledcAttach(FAN_PWM_PIN, PWM_FREQ_HZ, PWM_RES_BITS);
#endif
  writeDutyActiveLow(0);
}

void setup() {
  Serial.begin(115200);
  delay(5000);
  loadConfig();
  applyConfigToParameters();

  lastUserPercent = constrain(currentConfig.fan_default_speed_pct, 0, 100);
  if (lastUserPercent > 0 && lastUserPercent < PCT_MIN_RUN) lastUserPercent = PCT_MIN_RUN;

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  mqtt.setBufferSize(256);
  mqtt.setKeepAlive(45);
  mqtt.setSocketTimeout(5);
  setupPwm();

  // Start fan policy immediately (no network dependency)
  applyPowerOnPolicy();

  wifiManager.setDebugOutput(true);
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setSaveParamsCallback([](){ saveConfigCallback(); });

  // --- Parameter order: non‑MQTT first ---
  wifiManager.addParameter(&custom_fan_def_spd);
  wifiManager.addParameter(&custom_fan_def_on);

  // --- MQTT block at the very bottom of the portal ---
  wifiManager.addParameter(&custom_mqtt_header);
  // add hidden field then the UI checkbox+script
  wifiManager.addParameter(&custom_mqtt_enable_hidden);
  wifiManager.addParameter(&custom_mqtt_enable_ui);
  wifiManager.addParameter(&custom_mqtt_host);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.addParameter(&custom_mqtt_cmd_topic);
  wifiManager.addParameter(&custom_mqtt_state_topic);
  wifiManager.addParameter(&custom_mqtt_status_topic);

  wifiManager.setShowPassword(true);

  if (!wifiManager.autoConnect("BambuFanAP", "password")) {
    logPrintln("Failed to connect and timed out.");
    delay(3000);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    logPrint("WiFi connected, IP: ");
    logPrintln(WiFi.localIP());

    if (currentConfig.mqtt_enabled) {
      ensureMqtt();
    }

    ArduinoOTA.setHostname("esp32c3-fan");
    ArduinoOTA.begin();

    server.on("/",        HTTP_GET, handleRoot);
    server.on("/fan",     HTTP_GET, handleFanApi);
    server.on("/status",  HTTP_GET, handleStatusApi);
    server.on("/reconfig",HTTP_GET, handleReconfig);
    server.onNotFound(notFound);
    server.begin();
    logPrintln("HTTP server started");

    // After portal/connection, force back to STA-only to avoid AP lingering
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }

  if (currentConfig.fan_default_on) {
    handleFanSpeed(currentConfig.fan_default_speed_pct);
  } else {
    handleFanSpeed(0);
  }
}

void loop() {
  wl_status_t currentStatus = WiFi.status();
  if (currentStatus != lastWifiStatus) {
    lastWifiStatus = currentStatus;
    logPrintf("[%lu ms] WiFi status changed: %d\n", millis(), currentStatus);
    if (currentStatus != WL_CONNECTED) {
      mqttWasConnected = false;
    }
  }

  if (currentStatus == WL_CONNECTED) {
    if (currentConfig.mqtt_enabled) {
      if (!mqtt.connected()) ensureMqtt();
      mqtt.loop();
    } else {
      if (mqtt.connected()) {
        // Respect new toggle: disconnect if previously connected
        mqtt.disconnect();
        mqttWasConnected = false;
      }
    }
    server.handleClient();

    if (pendingPercentAfterStart > 0 && millis() >= pendingPercentApplyMs && currentPercent > pendingPercentAfterStart) {
      int target = pendingPercentAfterStart;
      pendingPercentAfterStart = 0;
      pendingPercentApplyMs = 0;
      handleFanSpeed(target);
    }
  } else {
    WiFi.reconnect();
  }
  ArduinoOTA.handle();
  delay(2);
}
