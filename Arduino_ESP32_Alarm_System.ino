#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <UniversalTelegramBot.h>
#include <RCSwitch.h>
#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md5.h"

#include "secrets.h"

// -----------------------------
// Hardware / RF configuration
// -----------------------------
static constexpr uint8_t RF_RX_PIN = 27;
static constexpr uint8_t RF_TX_PIN = 14;
static constexpr uint8_t RF_BITS = 24;
static constexpr uint8_t BARK_MIC_PIN = 34;  // ADC input-only pin on ESP32

// Panel arm/disarm RF keys (from a learned panel remote)
static constexpr uint32_t PANEL_ARM_CODE = 1592772;
static constexpr uint32_t PANEL_DISARM_CODE = 1592770;
static constexpr const char* PANEL_ARM_REMOTE_KEY = "panel_arm";
static constexpr const char* PANEL_DISARM_REMOTE_KEY = "panel_disarm";

// -----------------------------
// Bot / persistence limits
// -----------------------------
static constexpr uint32_t BOT_POLL_INTERVAL_MS = 2200;
static constexpr uint32_t RF_DUPLICATE_WINDOW_MS = 1200;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 4000;
static constexpr uint8_t WIFI_MAX_CONNECT_ATTEMPTS = 3;
static constexpr uint32_t WIFI_ATTEMPT_TIMEOUT_MS = 12000;
static constexpr uint16_t TELEGRAM_CLIENT_TIMEOUT_MS = 900;
static constexpr uint16_t PUSH_HTTP_TIMEOUT_MS = 600;
static constexpr uint16_t PUSH_HTTP_CONNECT_TIMEOUT_MS = 400;
static constexpr uint16_t WEB_LOGS_MAX_BATCH = 40;
static constexpr size_t WEB_OVERVIEW_DOC_CAPACITY = 24576;
static constexpr uint16_t ADC_MAX_VALUE = 4095;
static constexpr uint16_t BARK_SAMPLE_INTERVAL_MS = 10;
static constexpr uint16_t BARK_WINDOW_MS = 250;
static constexpr size_t CONFIG_EXPORT_CHUNK_SIZE = 3000;
static constexpr size_t CONFIG_IMPORT_MAX_LEN = 60000;
static constexpr uint32_t CONFIG_IMPORT_TIMEOUT_MS = 10UL * 60UL * 1000UL;
static constexpr size_t MAX_CONFIG_IMPORT_SESSIONS = 5;

static constexpr uint8_t BARK_MODE_AO = 0;
static constexpr uint8_t BARK_MODE_DO = 1;
static constexpr uint8_t BARK_MODE_BOTH = 2;

static constexpr size_t MAX_AUTH_CHATS = 20;
static constexpr size_t MAX_RUNTIME_LISTENERS = 20;
static constexpr size_t MAX_SENSORS = 120;
static constexpr uint8_t MAX_GROUPS = 100;  // 0..99
static constexpr size_t MAX_REMOTES = 40;

static constexpr size_t SENSOR_NAME_LEN = 32;
static constexpr size_t GROUP_NAME_LEN = 28;
static constexpr size_t PUSH_HOST_LEN = 96;
static constexpr size_t REMOTE_KEY_LEN = 24;
static constexpr size_t WIFI_SSID_LEN = 33;
static constexpr size_t WIFI_PASS_LEN = 65;
static constexpr size_t SMS_ROUTER_HOST_LEN = 64;
static constexpr size_t SMS_ROUTER_PASSWORD_LEN = 64;
static constexpr size_t SMS_PHONE_LEN = 24;
static constexpr size_t SMS_ROUTER_ID_LEN = 20;
static constexpr size_t SMS_FIRMWARE_ID_LEN = 24;
static constexpr size_t MAX_SMS_RECIPIENTS = 5;

static constexpr const char* SMS_ROUTER_TL_MR100 = "tl-mr100";
static constexpr const char* SMS_FIRMWARE_MR100_GDPR_V1 = "mr100-gdpr-v1";
static constexpr uint16_t SMS_HTTP_TIMEOUT_MS = 2500;
static constexpr uint16_t SMS_HTTP_CONNECT_TIMEOUT_MS = 1600;
static constexpr uint16_t SMS_POLL_INTERVAL_MS = 600;
static constexpr uint16_t SMS_POLL_TIMEOUT_MS = 15000;
static constexpr uint32_t SMS_MIN_SEND_INTERVAL_MS = 7000;
static constexpr uint16_t PERF_SLOW_OP_LOG_MS = 700;
static constexpr uint16_t PERF_HTTP_SLOW_LOG_MS = 1000;
static constexpr uint16_t PERF_LOOP_GAP_LOG_MS = 1800;
static constexpr uint16_t PERF_LOG_MIN_INTERVAL_MS = 1200;
static constexpr uint16_t COOPERATIVE_DELAY_SLICE_MS = 20;

static constexpr const char* STATE_FILE = "/alarm_state.json";
static constexpr const char* WIFI_CONFIG_FILE = "/wifi_config.json";
static constexpr const char* PUSH_EVENT_ROUTE = "/api/alarm/events";
static constexpr const char* WIFI_AP_SSID = "AlarmSetup";
static constexpr const char* WIFI_AP_PASSWORD = "alarmsetup";
static constexpr const char* WEB_SESSION_COOKIE = "ALARMSESSID";
static constexpr const char* WEB_CHAT_PREFIX = "web:";
static constexpr uint32_t WEB_SESSION_TTL_MS = 24UL * 60UL * 60UL * 1000UL;
static constexpr size_t MAX_WEB_SESSIONS = 8;
static constexpr size_t WEB_EVENT_CAPACITY = 120;
static constexpr size_t WEB_EVENT_TEXT_LEN = 200;
static constexpr int16_t WIFI_SCAN_STATE_RUNNING = -1;
static constexpr int16_t WIFI_SCAN_STATE_FAILED = -2;
static constexpr uint32_t WIFI_SCAN_REFRESH_MS = 15000;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 10000;
static constexpr uint32_t WIFI_SCAN_RETRY_MS = 3000;

struct SensorEntry {
  bool used = false;
  uint32_t code = 0;
  uint8_t bits = RF_BITS;
  uint8_t group = 0;
  bool notifyEnabled = false;  // independent sensor-level alert mode
  char name[SENSOR_NAME_LEN] = {0};
};

struct PushServerConfig {
  bool enabled = false;
  bool https = false;
  char host[PUSH_HOST_LEN] = {0};
  uint16_t port = 8080;
};

struct RemoteEntry {
  bool used = false;
  char key[REMOTE_KEY_LEN] = {0};
  uint32_t code = 0;
  uint8_t bits = RF_BITS;
};

struct WifiCredentials {
  bool valid = false;
  char ssid[WIFI_SSID_LEN] = {0};
  char password[WIFI_PASS_LEN] = {0};
};

struct BarkConfig {
  bool enabled = false;
  uint32_t code = 7654321;
  uint16_t threshold = 3000;
  uint32_t cooldownMs = 7000;
  bool emitRf = false;  // optional: transmit bark code via RF TX
  uint8_t bits = RF_BITS;
  uint8_t inputMode = BARK_MODE_DO;   // 3-pin MRS018A default
  uint8_t doActiveLevel = 0;          // typical active LOW for this module
};

struct SmsConfig {
  bool enabled = false;
  bool savedModeEnabled = true;
  bool groupModeEnabled = true;
  bool sensorModeEnabled = true;
  char routerId[SMS_ROUTER_ID_LEN] = "tl-mr100";
  char firmwareId[SMS_FIRMWARE_ID_LEN] = "mr100-gdpr-v1";
  char routerHost[SMS_ROUTER_HOST_LEN] = "192.168.0.1";
  char routerPassword[SMS_ROUTER_PASSWORD_LEN] = {0};
  char recipientPhones[MAX_SMS_RECIPIENTS][SMS_PHONE_LEN] = {{0}};
  char routerUser[16] = "admin";
};

struct ConfigImportSession {
  bool active = false;
  String chatId = "";
  String payload = "";
  unsigned long updatedMs = 0;
};

struct WebSession {
  bool used = false;
  char token[33] = {0};  // 16 random bytes in hex
  unsigned long updatedMs = 0;
};

struct WebEventEntry {
  uint32_t seq = 0;
  unsigned long tsMs = 0;
  char text[WEB_EVENT_TEXT_LEN] = {0};
};

RCSwitch rf;
WiFiClientSecure tgClient;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, tgClient);
WebServer provisionServer(80);

unsigned long botLastPollMs = 0;
unsigned long wifiRetryMs = 0;
unsigned long pushBackoffUntilMs = 0;
uint8_t pushConsecutiveFailures = 0;

uint32_t lastRfCode = 0;
unsigned long lastRfCodeMs = 0;

// Persistent state
String authorizedChats[MAX_AUTH_CHATS];
size_t authorizedCount = 0;

SensorEntry sensors[MAX_SENSORS];
char groupNames[MAX_GROUPS][GROUP_NAME_LEN];
bool groupArmed[MAX_GROUPS];
bool listenSavedArmed = false;
PushServerConfig pushServer;
RemoteEntry remotes[MAX_REMOTES];

// Runtime-only state (must not survive outage)
String runtimeListenAllChats[MAX_RUNTIME_LISTENERS];
size_t runtimeListenAllCount = 0;

WifiCredentials wifiCreds;
bool wifiProvisioningMode = false;
bool offlineApMode = false;
String wifiProvisionApSsid = "";
String wifiScanOptionsCache = "";
unsigned long wifiScanCacheMs = 0;
unsigned long wifiScanStartedMs = 0;
unsigned long wifiScanNextRefreshMs = 0;

BarkConfig barkConfig;
unsigned long barkLastSampleMs = 0;
unsigned long barkWindowStartMs = 0;
unsigned long barkLastTriggerMs = 0;
uint16_t barkWindowPeak = 0;
bool barkWindowDoTriggered = false;
uint16_t barkWindowSampleCount = 0;
uint16_t barkWindowDoActiveSamples = 0;
uint16_t barkLastAoLevel = 0;
int barkLastDoLevel = -1;
uint16_t barkLastWindowPeak = 0;
uint16_t barkLastTriggerNoiseLevel = 0;
bool barkLastTriggerDoActive = false;
uint8_t barkLastDoActivePct = 0;
SmsConfig smsConfig;
unsigned long smsLastSendMs = 0;
uint32_t smsLastSendCode = 0;
String smsLastResult = "never";

ConfigImportSession configImportSessions[MAX_CONFIG_IMPORT_SESSIONS];

WebSession webSessions[MAX_WEB_SESSIONS];
WebEventEntry webEvents[WEB_EVENT_CAPACITY];
size_t webEventWritePos = 0;
size_t webEventCount = 0;
uint32_t webEventSeq = 0;

bool webCommandCaptureActive = false;
String webCommandCaptureChatId = "";
String webCommandCaptureReply = "";
bool webServerStarted = false;
bool telegramPollActive = false;
const char* perfBusyOp = "idle";
unsigned long perfBusySinceMs = 0;
unsigned long perfLastSlowLogMs = 0;
unsigned long perfLastLoopGapLogMs = 0;
unsigned long loopLastTickMs = 0;

class BusyScope {
 public:
  explicit BusyScope(const char* op) : prevOp(perfBusyOp), prevSinceMs(perfBusySinceMs) {
    perfBusyOp = op;
    perfBusySinceMs = millis();
  }
  ~BusyScope() {
    perfBusyOp = prevOp;
    perfBusySinceMs = prevSinceMs;
  }

 private:
  const char* prevOp;
  unsigned long prevSinceMs;
};

class FlagScope {
 public:
  explicit FlagScope(bool& flagRef) : flag(flagRef) {
    flag = true;
  }
  ~FlagScope() {
    flag = false;
  }

 private:
  bool& flag;
};

void processWebServer();
void pollTelegram();

void logSlowOperation(const String& name, unsigned long tookMs, uint16_t thresholdMs = PERF_SLOW_OP_LOG_MS) {
  if (tookMs < thresholdMs) return;
  unsigned long now = millis();
  if (now - perfLastSlowLogMs < PERF_LOG_MIN_INTERVAL_MS) return;
  perfLastSlowLogMs = now;
  unsigned long busyFor = (perfBusySinceMs == 0) ? 0 : (now - perfBusySinceMs);
  Serial.printf("[PERF] %s took %lums (busy=%s %lums)\n", name.c_str(), tookMs, perfBusyOp, busyFor);
}

void monitorLoopGap() {
  unsigned long now = millis();
  if (loopLastTickMs != 0) {
    unsigned long gap = now - loopLastTickMs;
    if (gap >= PERF_LOOP_GAP_LOG_MS && (now - perfLastLoopGapLogMs) >= PERF_LOOP_GAP_LOG_MS) {
      perfLastLoopGapLogMs = now;
      unsigned long busyFor = (perfBusySinceMs == 0) ? 0 : (now - perfBusySinceMs);
      Serial.printf("[PERF] loop gap=%lums (busy=%s %lums)\n", gap, perfBusyOp, busyFor);
    }
  }
  loopLastTickMs = now;
}

void runCooperativeHousekeeping() {
  processWebServer();
  if (!telegramPollActive) pollTelegram();
  yield();
}

void cooperativeDelayMs(uint32_t totalMs) {
  unsigned long startMs = millis();
  while (millis() - startMs < totalMs) {
    runCooperativeHousekeeping();
    delay(COOPERATIVE_DELAY_SLICE_MS);
  }
}

String smsLogPhone(const String& phone) {
  if (phone.length() <= 4) return phone;
  String out = phone;
  for (size_t i = 0; i < out.length(); i++) {
    bool keep = (i < 2) || (i >= out.length() - 2);
    char c = out.charAt(i);
    if (!keep && c >= '0' && c <= '9') out.setCharAt(i, '*');
  }
  return out;
}

void copyToBuf(char* dest, size_t destSize, const String& src) {
  if (dest == nullptr || destSize == 0) return;
  strlcpy(dest, src.c_str(), destSize);
}

String normalizeSpaces(String s) {
  s.replace('\r', ' ');
  s.replace('\n', ' ');
  s.trim();
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  return s;
}

String lowerCopy(String s) {
  s.toLowerCase();
  return s;
}

String barkModeText(uint8_t mode) {
  if (mode == BARK_MODE_AO) return "ao";
  if (mode == BARK_MODE_DO) return "do";
  return "both";
}

bool isBarkSource(const String& source) {
  return source == "bark" || source == "bark_test";
}

bool parseUInt32(const String& text, uint32_t& out);
void addWebEvent(const String& text);

bool isSmsRouterSupported(const String& routerId) {
  return routerId == SMS_ROUTER_TL_MR100;
}

bool isSmsFirmwareSupported(const String& firmwareId) {
  return firmwareId == SMS_FIRMWARE_MR100_GDPR_V1;
}

String smsRouterLabel(const String& routerId) {
  if (routerId == SMS_ROUTER_TL_MR100) return "TP-Link TL-MR100";
  return "unknown";
}

String smsFirmwareLabel(const String& firmwareId) {
  if (firmwareId == SMS_FIRMWARE_MR100_GDPR_V1) return "GDPR encrypted web API v1";
  return "unknown";
}

bool isPhoneNumberValid(const String& rawPhone) {
  String phone = rawPhone;
  phone.trim();
  if (phone.length() == 0 || phone.length() > 20) return false;
  size_t start = 0;
  if (phone.charAt(0) == '+') {
    if (phone.length() == 1) return false;
    start = 1;
  }
  for (size_t i = start; i < phone.length(); i++) {
    char c = phone.charAt(i);
    if (!(c >= '0' && c <= '9')) return false;
  }
  return true;
}

size_t smsRecipientCount() {
  size_t count = 0;
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] != '\0') count++;
  }
  return count;
}

int findSmsRecipientIndex(String phone) {
  phone.trim();
  if (phone.length() == 0) return -1;
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    if (phone == String(smsConfig.recipientPhones[i])) return static_cast<int>(i);
  }
  return -1;
}

String smsRecipientsInline() {
  String out = "";
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    if (out.length() > 0) out += ", ";
    out += String(smsConfig.recipientPhones[i]);
  }
  if (out.length() == 0) out = "(none)";
  return out;
}

String smsRecipientsListText() {
  String out = "";
  size_t num = 0;
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    num++;
    out += String(num) + ". " + String(smsConfig.recipientPhones[i]) + "\n";
  }
  if (num == 0) out = "(none)\n";
  return out;
}

void clearSmsRecipients() {
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    smsConfig.recipientPhones[i][0] = '\0';
  }
}

bool addSmsRecipient(String phone, String& reason) {
  reason = "";
  phone.trim();
  if (!isPhoneNumberValid(phone) || phone.length() >= SMS_PHONE_LEN) {
    reason = "invalid phone format";
    return false;
  }
  if (findSmsRecipientIndex(phone) >= 0) {
    reason = "already exists";
    return true;
  }
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] != '\0') continue;
    copyToBuf(smsConfig.recipientPhones[i], SMS_PHONE_LEN, phone);
    return true;
  }
  reason = "maximum recipients reached (5)";
  return false;
}

bool removeSmsRecipient(String phone) {
  int idx = findSmsRecipientIndex(phone);
  if (idx < 0) return false;
  for (size_t i = idx; i + 1 < MAX_SMS_RECIPIENTS; i++) {
    copyToBuf(smsConfig.recipientPhones[i], SMS_PHONE_LEN, String(smsConfig.recipientPhones[i + 1]));
  }
  smsConfig.recipientPhones[MAX_SMS_RECIPIENTS - 1][0] = '\0';
  return true;
}

bool parseSmsRecipientsCsv(const String& csv, String phones[], size_t& outCount, String& reason) {
  outCount = 0;
  reason = "";
  String token = "";

  auto pushToken = [&](String p) -> bool {
    p.trim();
    if (p.length() == 0) return true;
    if (!isPhoneNumberValid(p) || p.length() >= SMS_PHONE_LEN) {
      reason = "invalid phone: " + p;
      return false;
    }
    for (size_t i = 0; i < outCount; i++) {
      if (phones[i] == p) return true;
    }
    if (outCount >= MAX_SMS_RECIPIENTS) {
      reason = "max 5 recipients allowed";
      return false;
    }
    phones[outCount++] = p;
    return true;
  };

  for (size_t i = 0; i <= csv.length(); i++) {
    char c = (i < csv.length()) ? csv.charAt(i) : ',';
    bool delim = (c == ',' || c == ';' || c == '\n' || c == '\r');
    if (delim) {
      if (!pushToken(token)) return false;
      token = "";
    } else {
      token += c;
    }
  }

  if (outCount == 0) {
    reason = "at least one recipient is required";
    return false;
  }
  return true;
}

String normalizeSetCookieHeader(const String& setCookie) {
  String cookie = setCookie;
  cookie.trim();
  if (cookie.length() == 0) return "";
  int semi = cookie.indexOf(';');
  if (semi >= 0) cookie = cookie.substring(0, semi);
  cookie.trim();
  return cookie;
}

bool isSmsConfigured(String& reason) {
  if (!isSmsRouterSupported(String(smsConfig.routerId))) {
    reason = "unsupported router type";
    return false;
  }
  if (!isSmsFirmwareSupported(String(smsConfig.firmwareId))) {
    reason = "unsupported firmware type";
    return false;
  }
  String host = String(smsConfig.routerHost);
  host.trim();
  if (host.length() == 0) {
    reason = "router IP/host is empty";
    return false;
  }
  String routerPassword = String(smsConfig.routerPassword);
  if (routerPassword.length() == 0) {
    reason = "router password is empty";
    return false;
  }
  if (smsRecipientCount() == 0) {
    reason = "at least one destination phone is required";
    return false;
  }
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    String phone = String(smsConfig.recipientPhones[i]);
    if (!isPhoneNumberValid(phone)) {
      reason = "invalid destination phone in list";
      return false;
    }
  }
  reason = "";
  return true;
}

String genRandomDigit16() {
  String out = "";
  out.reserve(16);
  while (out.length() < 16) {
    uint32_t x = esp_random();
    for (uint8_t i = 0; i < 8 && out.length() < 16; i++) {
      out += static_cast<char>('0' + (x % 10));
      x /= 10;
    }
  }
  return out;
}

static int md5StartsCompat(mbedtls_md5_context* ctx) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  return mbedtls_md5_starts(ctx);
#else
  return mbedtls_md5_starts_ret(ctx);
#endif
}

static int md5UpdateCompat(mbedtls_md5_context* ctx, const unsigned char* input, size_t len) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  return mbedtls_md5_update(ctx, input, len);
#else
  return mbedtls_md5_update_ret(ctx, input, len);
#endif
}

static int md5FinishCompat(mbedtls_md5_context* ctx, unsigned char output[16]) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  return mbedtls_md5_finish(ctx, output);
#else
  return mbedtls_md5_finish_ret(ctx, output);
#endif
}

String md5Hex(const String& input) {
  unsigned char digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  int rc = md5StartsCompat(&ctx);
  rc |= md5UpdateCompat(&ctx, reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
  rc |= md5FinishCompat(&ctx, digest);
  mbedtls_md5_free(&ctx);
  if (rc != 0) return "";

  char hex[33];
  for (uint8_t i = 0; i < 16; i++) {
    snprintf(hex + (i * 2), 3, "%02x", digest[i]);
  }
  hex[32] = '\0';
  return String(hex);
}

String urlEncodeComponent(const String& src) {
  String out = "";
  out.reserve(src.length() * 3);
  const char* hex = "0123456789ABCDEF";

  for (size_t i = 0; i < src.length(); i++) {
    unsigned char c = static_cast<unsigned char>(src.charAt(i));
    bool unreserved =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool base64EncodeBuffer(const uint8_t* in, size_t inLen, String& out, String& err) {
  out = "";
  err = "";
  size_t olen = 0;
  int rc = mbedtls_base64_encode(nullptr, 0, &olen, in, inLen);
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    err = "base64 size failed";
    return false;
  }

  uint8_t* enc = static_cast<uint8_t*>(malloc(olen + 1));
  if (enc == nullptr) {
    err = "out of memory";
    return false;
  }

  rc = mbedtls_base64_encode(enc, olen, &olen, in, inLen);
  if (rc != 0) {
    free(enc);
    err = "base64 encode failed";
    return false;
  }
  enc[olen] = '\0';
  out = String(reinterpret_cast<const char*>(enc));
  free(enc);
  return true;
}

bool base64DecodeString(const String& in, uint8_t*& outBuf, size_t& outLen, String& err) {
  outBuf = nullptr;
  outLen = 0;
  err = "";
  if (in.length() == 0) {
    err = "empty base64 input";
    return false;
  }

  size_t cap = (in.length() * 3) / 4 + 8;
  uint8_t* tmp = static_cast<uint8_t*>(malloc(cap));
  if (tmp == nullptr) {
    err = "out of memory";
    return false;
  }

  int rc = mbedtls_base64_decode(
    tmp,
    cap,
    &outLen,
    reinterpret_cast<const uint8_t*>(in.c_str()),
    in.length()
  );
  if (rc != 0) {
    free(tmp);
    err = "base64 decode failed";
    return false;
  }

  outBuf = tmp;
  return true;
}

bool aesCbcEncryptToBase64(
  const String& plaintext,
  const String& key,
  const String& iv,
  String& outB64,
  String& err
) {
  outB64 = "";
  err = "";
  if (key.length() != 16 || iv.length() != 16) {
    err = "AES key/iv must be exactly 16 chars";
    return false;
  }

  size_t plainLen = plaintext.length();
  uint8_t padLen = static_cast<uint8_t>(16 - (plainLen % 16));
  if (padLen == 0) padLen = 16;
  size_t totalLen = plainLen + padLen;

  uint8_t* input = static_cast<uint8_t*>(malloc(totalLen));
  uint8_t* output = static_cast<uint8_t*>(malloc(totalLen));
  if (input == nullptr || output == nullptr) {
    if (input) free(input);
    if (output) free(output);
    err = "out of memory";
    return false;
  }

  memcpy(input, plaintext.c_str(), plainLen);
  for (size_t i = plainLen; i < totalLen; i++) input[i] = padLen;

  uint8_t ivBuf[16];
  memcpy(ivBuf, iv.c_str(), 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_enc(&aes, reinterpret_cast<const uint8_t*>(key.c_str()), 128);
  if (rc != 0) {
    mbedtls_aes_free(&aes);
    free(input);
    free(output);
    err = "AES setkey enc failed";
    return false;
  }

  rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, totalLen, ivBuf, input, output);
  mbedtls_aes_free(&aes);
  free(input);
  if (rc != 0) {
    free(output);
    err = "AES CBC encrypt failed";
    return false;
  }

  bool ok = base64EncodeBuffer(output, totalLen, outB64, err);
  free(output);
  return ok;
}

bool aesCbcDecryptFromBase64(
  const String& cipherB64,
  const String& key,
  const String& iv,
  String& plaintext,
  String& err
) {
  plaintext = "";
  err = "";
  if (key.length() != 16 || iv.length() != 16) {
    err = "AES key/iv must be exactly 16 chars";
    return false;
  }

  uint8_t* decoded = nullptr;
  size_t decodedLen = 0;
  if (!base64DecodeString(cipherB64, decoded, decodedLen, err)) return false;
  if (decodedLen == 0 || (decodedLen % 16) != 0) {
    free(decoded);
    err = "invalid encrypted data size";
    return false;
  }

  uint8_t* output = static_cast<uint8_t*>(malloc(decodedLen));
  if (output == nullptr) {
    free(decoded);
    err = "out of memory";
    return false;
  }

  uint8_t ivBuf[16];
  memcpy(ivBuf, iv.c_str(), 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_dec(&aes, reinterpret_cast<const uint8_t*>(key.c_str()), 128);
  if (rc != 0) {
    mbedtls_aes_free(&aes);
    free(decoded);
    free(output);
    err = "AES setkey dec failed";
    return false;
  }
  rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, decodedLen, ivBuf, decoded, output);
  mbedtls_aes_free(&aes);
  free(decoded);
  if (rc != 0) {
    free(output);
    err = "AES CBC decrypt failed";
    return false;
  }

  uint8_t padLen = output[decodedLen - 1];
  size_t plainLen = decodedLen;
  if (padLen > 0 && padLen <= 16 && padLen <= decodedLen) {
    bool pkcs7 = true;
    for (size_t i = decodedLen - padLen; i < decodedLen; i++) {
      if (output[i] != padLen) {
        pkcs7 = false;
        break;
      }
    }
    if (pkcs7) plainLen = decodedLen - padLen;
  }

  plaintext.reserve(plainLen + 1);
  for (size_t i = 0; i < plainLen; i++) plaintext += static_cast<char>(output[i]);
  free(output);
  return true;
}

bool rsaEncryptNoPaddingChunks(
  const String& modulusHex,
  const String& exponentHex,
  const String& plainText,
  String& outHex,
  String& err
) {
  outHex = "";
  err = "";
  if (plainText.length() == 0) {
    err = "RSA input is empty";
    return false;
  }

  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi m;
  mbedtls_mpi c;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&m);
  mbedtls_mpi_init(&c);

  int rc = mbedtls_mpi_read_string(&n, 16, modulusHex.c_str());
  if (rc == 0) rc = mbedtls_mpi_read_string(&e, 16, exponentHex.c_str());
  if (rc != 0) {
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&c);
    err = "RSA key parse failed";
    return false;
  }

  const size_t blockSize = 64;
  outHex.reserve(((plainText.length() + blockSize - 1) / blockSize) * 128);

  for (size_t off = 0; off < plainText.length(); off += blockSize) {
    size_t chunkLen = plainText.length() - off;
    if (chunkLen > blockSize) chunkLen = blockSize;

    uint8_t block[blockSize];
    memset(block, 0, sizeof(block));
    memcpy(block, plainText.c_str() + off, chunkLen);

    rc = mbedtls_mpi_read_binary(&m, block, blockSize);
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&c, &m, &e, &n, nullptr);
    if (rc != 0) {
      mbedtls_mpi_free(&n);
      mbedtls_mpi_free(&e);
      mbedtls_mpi_free(&m);
      mbedtls_mpi_free(&c);
      err = "RSA exp_mod failed";
      return false;
    }

    char hexBuf[260];
    size_t hexLen = 0;
    rc = mbedtls_mpi_write_string(&c, 16, hexBuf, sizeof(hexBuf), &hexLen);
    if (rc != 0) {
      mbedtls_mpi_free(&n);
      mbedtls_mpi_free(&e);
      mbedtls_mpi_free(&m);
      mbedtls_mpi_free(&c);
      err = "RSA output serialization failed";
      return false;
    }

    String chunkHex = String(hexBuf);
    chunkHex.toLowerCase();
    while (chunkHex.length() < 128) chunkHex = "0" + chunkHex;
    if (chunkHex.length() > 128) {
      chunkHex = chunkHex.substring(chunkHex.length() - 128);
    }
    outHex += chunkHex;
  }

  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&m);
  mbedtls_mpi_free(&c);
  return true;
}

String extractJsVar(const String& jsText, const String& key) {
  String marker = "var " + key + "=";
  int pos = jsText.indexOf(marker);
  if (pos < 0) return "";

  int start = pos + marker.length();
  while (start < jsText.length() && (jsText.charAt(start) == ' ' || jsText.charAt(start) == '\t')) start++;
  if (start >= jsText.length()) return "";

  if (jsText.charAt(start) == '"') {
    int end = jsText.indexOf('"', start + 1);
    if (end < 0) return "";
    return jsText.substring(start + 1, end);
  }

  int end = jsText.indexOf(';', start);
  if (end < 0) end = jsText.length();
  String raw = jsText.substring(start, end);
  raw.trim();
  return raw;
}

String findRouterValue(const String& decoded, const String& key) {
  String marker = key + "=";
  int pos = decoded.indexOf(marker);
  if (pos < 0) return "";
  int start = pos + marker.length();
  int end = start;
  while (end < decoded.length()) {
    char c = decoded.charAt(end);
    if (c == '\r' || c == '\n') break;
    end++;
  }
  return decoded.substring(start, end);
}

int findRouterErrorCode(const String& decoded) {
  int pos = decoded.indexOf("[error]");
  if (pos < 0) return 0;
  int start = pos + 7;
  int end = start;
  while (end < decoded.length()) {
    char c = decoded.charAt(end);
    if (c < '0' || c > '9') break;
    end++;
  }
  if (end <= start) return 0;
  return decoded.substring(start, end).toInt();
}

String buildRouterActPayload(uint8_t actType, const String& oid, const String attrs[], size_t attrCount) {
  String payload = String(actType) + "\r\n";
  payload += "[" + oid + "#0,0,0,0,0,0#0,0,0,0,0,0]0," + String(attrCount) + "\r\n";
  for (size_t i = 0; i < attrCount; i++) payload += attrs[i] + "\r\n";
  return payload;
}

String escapeSmsTextForRouter(const String& text) {
  String out = "";
  out.reserve(text.length() + 4);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == '\n') out += static_cast<char>(18);
    else if (c == '\r') out += static_cast<char>(17);
    else out += c;
  }
  return out;
}

bool routerHttpRequest(
  const String& method,
  const String& url,
  const String& body,
  const String& contentType,
  const String& cookie,
  const String& token,
  String& responseBody,
  String& setCookie,
  String& err
) {
  responseBody = "";
  setCookie = "";
  err = "";
  BusyScope busy("sms_http");
  unsigned long reqStartMs = millis();

  WiFiClient client;
  HTTPClient http;
  const char* headerKeys[] = {"Set-Cookie"};
  http.collectHeaders(headerKeys, 1);

  if (!http.begin(client, url)) {
    err = "http.begin failed";
    return false;
  }
  http.setConnectTimeout(SMS_HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(SMS_HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", "Mozilla/5.0");

  int slash = url.indexOf('/', 8);
  String referer = (slash > 0) ? url.substring(0, slash + 1) : (url + "/");
  http.addHeader("Referer", referer);
  if (cookie.length() > 0) http.addHeader("Cookie", cookie);
  if (token.length() > 0) http.addHeader("TokenID", token);
  if (contentType.length() > 0) http.addHeader("Content-Type", contentType);

  int code = -1;
  if (method == "POST") code = http.POST(body);
  else code = http.GET();
  responseBody = http.getString();
  setCookie = normalizeSetCookieHeader(http.header("Set-Cookie"));
  String errStr = http.errorToString(code);
  http.end();
  unsigned long tookMs = millis() - reqStartMs;
  if (tookMs >= PERF_HTTP_SLOW_LOG_MS) {
    int slash = url.indexOf('/', 8);
    String path = (slash > 0) ? url.substring(slash) : url;
    logSlowOperation("http " + method + " " + path + " code=" + String(code), tookMs, PERF_HTTP_SLOW_LOG_MS);
  }

  if (code < 200 || code >= 300) {
    err = "HTTP " + String(code) + " " + errStr;
    if (responseBody.length() > 0) err += " body=" + responseBody;
    return false;
  }
  return true;
}

bool routerExecuteAct(
  const String& baseUrl,
  const String& modulusHex,
  const String& exponentHex,
  uint32_t seq,
  const String& pwdHash,
  const String& aesKey,
  const String& aesIv,
  const String& cookie,
  const String& token,
  uint8_t actType,
  const String& oid,
  const String attrs[],
  size_t attrCount,
  String& decodedResponse,
  String& err
) {
  decodedResponse = "";
  err = "";

  String payload = buildRouterActPayload(actType, oid, attrs, attrCount);
  String encryptedData;
  if (!aesCbcEncryptToBase64(payload, aesKey, aesIv, encryptedData, err)) return false;

  String signSource = "h=" + pwdHash + "&s=" + String(seq + encryptedData.length());
  String signHex;
  if (!rsaEncryptNoPaddingChunks(modulusHex, exponentHex, signSource, signHex, err)) return false;

  String reqBody = "sign=" + signHex + "\r\ndata=" + encryptedData + "\r\n";
  String rawResp;
  String ignoredCookie;
  if (!routerHttpRequest(
        "POST",
        baseUrl + "/cgi_gdpr?",
        reqBody,
        "text/plain",
        cookie,
        token,
        rawResp,
        ignoredCookie,
        err
      )) {
    return false;
  }

  if (!aesCbcDecryptFromBase64(rawResp, aesKey, aesIv, decodedResponse, err)) return false;
  int errCode = findRouterErrorCode(decodedResponse);
  if (errCode != 0) {
    err = "router returned error code " + String(errCode);
    return false;
  }
  return true;
}

bool sendSmsViaTlMr100(
  const String& host,
  const String& username,
  const String& password,
  const String& phone,
  const String& message,
  String& status,
  String& err
) {
  BusyScope busy("sms_send_one");
  unsigned long smsStartMs = millis();
  status = "";
  err = "";
  if (WiFi.status() != WL_CONNECTED) {
    err = "wifi disconnected";
    Serial.println("[SMS] skip send: WiFi disconnected.");
    return false;
  }
  if (!isPhoneNumberValid(phone)) {
    err = "invalid destination phone";
    Serial.println("[SMS] skip send: invalid destination phone.");
    return false;
  }

  String hostTrim = host;
  hostTrim.trim();
  String baseUrl = "http://" + hostTrim;

  String getParmResp;
  String cookie = "";
  if (!routerHttpRequest("POST", baseUrl + "/cgi/getParm", "", "", "", "", getParmResp, cookie, err)) return false;

  String modulusHex = extractJsVar(getParmResp, "nn");
  String exponentHex = extractJsVar(getParmResp, "ee");
  String seqText = extractJsVar(getParmResp, "seq");
  uint32_t seq = 0;
  if (modulusHex.length() == 0 || exponentHex.length() == 0 || !parseUInt32(seqText, seq)) {
    err = "failed parsing /cgi/getParm";
    return false;
  }

  String aesKey = genRandomDigit16();
  String aesIv = genRandomDigit16();
  String pwdHash = md5Hex(username + password);

  String loginData;
  if (!aesCbcEncryptToBase64(username + "\n" + password, aesKey, aesIv, loginData, err)) return false;

  String loginSignSource = "key=" + aesKey + "&iv=" + aesIv + "&h=" + pwdHash + "&s=" + String(seq + loginData.length());
  String loginSign;
  if (!rsaEncryptNoPaddingChunks(modulusHex, exponentHex, loginSignSource, loginSign, err)) return false;

  String loginUrl = baseUrl + "/cgi/login?data=" + urlEncodeComponent(loginData) + "&sign=" + loginSign + "&Action=1&LoginStatus=0";
  String loginResp;
  String loginCookie = "";
  if (!routerHttpRequest("POST", loginUrl, "", "", cookie, "", loginResp, loginCookie, err)) return false;
  if (loginResp.indexOf("$.ret=0") < 0) {
    err = "router login failed";
    return false;
  }
  if (loginCookie.length() > 0) cookie = loginCookie;

  String rootResp;
  String rootCookie = "";
  if (!routerHttpRequest("GET", baseUrl + "/", "", "", cookie, "", rootResp, rootCookie, err)) return false;
  if (rootCookie.length() > 0) cookie = rootCookie;
  String token = extractJsVar(rootResp, "token");
  if (token.length() == 0) token = "0";

  String escaped = escapeSmsTextForRouter(message);
  String setAttrs[3];
  setAttrs[0] = "index=1";
  setAttrs[1] = "to=" + phone;
  setAttrs[2] = "textContent=" + escaped;
  String decoded;
  if (!routerExecuteAct(
        baseUrl,
        modulusHex,
        exponentHex,
        seq,
        pwdHash,
        aesKey,
        aesIv,
        cookie,
        token,
        2,
        "LTE_SMS_SENDNEWMSG",
        setAttrs,
        3,
        decoded,
        err
      )) {
    return false;
  }

  unsigned long startMs = millis();
  while (millis() - startMs < SMS_POLL_TIMEOUT_MS) {
    cooperativeDelayMs(SMS_POLL_INTERVAL_MS);
    String getAttrs[1];
    getAttrs[0] = "sendResult";
    if (!routerExecuteAct(
          baseUrl,
          modulusHex,
          exponentHex,
          seq,
          pwdHash,
          aesKey,
          aesIv,
          cookie,
          token,
          1,
          "LTE_SMS_SENDNEWMSG",
          getAttrs,
          1,
          decoded,
          err
        )) {
      return false;
    }

    String sendResult = findRouterValue(decoded, "sendResult");
    if (sendResult == "1") {
      status = "sent";
      unsigned long tookMs = millis() - smsStartMs;
      if (tookMs >= 1500) {
        Serial.printf("[SMS] send slow -> %s in %lums\n", smsLogPhone(phone).c_str(), tookMs);
      }
      logSlowOperation("sms single send", tookMs, PERF_SLOW_OP_LOG_MS);
      return true;
    }
    if (sendResult == "2") {
      status = "modem_busy";
      err = "router modem busy";
      Serial.printf("[SMS] send fail -> %s modem busy (%lums)\n", smsLogPhone(phone).c_str(), millis() - smsStartMs);
      return false;
    }
    if (sendResult.length() > 0 && sendResult != "3") {
      status = "failed";
      err = "router sendResult=" + sendResult;
      Serial.printf(
        "[SMS] send fail -> %s sendResult=%s (%lums)\n",
        smsLogPhone(phone).c_str(),
        sendResult.c_str(),
        millis() - smsStartMs
      );
      return false;
    }
  }

  status = "timeout";
  err = "router SMS polling timeout";
  Serial.printf("[SMS] send timeout -> %s (%lums)\n", smsLogPhone(phone).c_str(), millis() - smsStartMs);
  return false;
}

bool sendSmsToConfiguredRecipients(const String& message, String& summary, String& errSummary) {
  BusyScope busy("sms_send_batch");
  unsigned long batchStartMs = millis();
  summary = "";
  errSummary = "";
  size_t total = smsRecipientCount();
  if (total == 0) {
    errSummary = "no recipients configured";
    return false;
  }
  Serial.printf("[SMS] batch start: recipients=%u\n", static_cast<unsigned>(total));

  size_t sent = 0;
  size_t failed = 0;
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    String phone = String(smsConfig.recipientPhones[i]);
    String status;
    String err;
    bool ok = sendSmsViaTlMr100(
      String(smsConfig.routerHost),
      String(smsConfig.routerUser),
      String(smsConfig.routerPassword),
      phone,
      message,
      status,
      err
    );
    if (ok) {
      sent++;
    } else {
      failed++;
      Serial.printf("[SMS] recipient failed -> %s (%s)\n", smsLogPhone(phone).c_str(), err.c_str());
      if (errSummary.length() == 0) errSummary = phone + ": " + err;
    }
    runCooperativeHousekeeping();
  }

  summary = String(sent) + "/" + String(total) + " sent";
  unsigned long tookMs = millis() - batchStartMs;
  if (failed == 0) {
    Serial.printf("[SMS] batch done: %s in %lums\n", summary.c_str(), tookMs);
  } else {
    Serial.printf("[SMS] batch done with failures: %s in %lums\n", summary.c_str(), tookMs);
  }
  logSlowOperation("sms batch send", tookMs, PERF_SLOW_OP_LOG_MS);
  return failed == 0;
}

void maybeSendSmsAlert(
  uint32_t code,
  bool throughSavedMode,
  bool throughGroupMode,
  bool throughSensorMode,
  const String& source,
  const String& baseText
) {
  if (!smsConfig.enabled) return;

  bool modeMatch =
    (throughSavedMode && smsConfig.savedModeEnabled) ||
    (throughGroupMode && smsConfig.groupModeEnabled) ||
    (throughSensorMode && smsConfig.sensorModeEnabled);
  if (!modeMatch) return;

  String cfgReason;
  if (!isSmsConfigured(cfgReason)) {
    smsLastResult = "error: " + cfgReason;
    return;
  }

  unsigned long now = millis();
  if (smsLastSendMs != 0 && (now - smsLastSendMs) < SMS_MIN_SEND_INTERVAL_MS && smsLastSendCode == code) {
    smsLastResult = "throttled";
    return;
  }

  String mode = "";
  if (throughSavedMode) mode += "saved+";
  if (throughGroupMode) mode += "group+";
  if (throughSensorMode) mode += "sensor+";
  if (mode.endsWith("+")) mode.remove(mode.length() - 1);

  String text = "[ALARM " + mode + "] " + baseText + " src=" + source;
  if (text.length() > 159) text = text.substring(0, 159);

  String summary;
  String err;
  bool ok = sendSmsToConfiguredRecipients(text, summary, err);
  smsLastSendMs = now;
  smsLastSendCode = code;
  if (ok) {
    smsLastResult = summary;
    addWebEvent("[SMS] " + summary + " mode=" + mode);
  } else {
    smsLastResult = "error: " + summary + " | " + err;
    addWebEvent("[SMS] failed: " + summary + " | " + err);
  }
}

bool readFileString(const char* path, String& out) {
  out = "";
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  while (f.available()) out += static_cast<char>(f.read());
  f.close();
  return true;
}

bool writeFileString(const char* path, const String& data) {
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  size_t written = f.print(data);
  f.close();
  return written == data.length();
}

String argsAfterFirstToken(const String& line) {
  int sp = line.indexOf(' ');
  if (sp < 0) return "";
  String out = line.substring(sp + 1);
  out.trim();
  return out;
}

bool parseUInt32(const String& text, uint32_t& out) {
  if (text.length() == 0) return false;
  char* endPtr = nullptr;
  unsigned long value = strtoul(text.c_str(), &endPtr, 10);
  if (endPtr == nullptr || *endPtr != '\0') return false;
  out = static_cast<uint32_t>(value);
  return true;
}

bool parseUInt16(const String& text, uint16_t& out) {
  if (text.length() == 0) return false;
  char* endPtr = nullptr;
  unsigned long value = strtoul(text.c_str(), &endPtr, 10);
  if (endPtr == nullptr || *endPtr != '\0' || value > 65535UL) return false;
  out = static_cast<uint16_t>(value);
  return true;
}

bool parseGroupId(const String& text, uint8_t& out) {
  uint32_t tmp = 0;
  if (!parseUInt32(text, tmp) || tmp > 99) return false;
  out = static_cast<uint8_t>(tmp);
  return true;
}

bool parseBits(const String& text, uint8_t& out) {
  uint32_t tmp = 0;
  if (!parseUInt32(text, tmp) || tmp == 0 || tmp > 32) return false;
  out = static_cast<uint8_t>(tmp);
  return true;
}

int parseHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool parseMacAddress(String text, uint8_t out[6]) {
  if (out == nullptr) return false;

  text.trim();
  if (text.length() == 0) return false;

  String clean = "";
  clean.reserve(12);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == ':' || c == '-' || c == '.' || c == ' ') continue;
    if (parseHexNibble(c) < 0) return false;
    clean += c;
  }

  if (clean.length() != 12) return false;

  for (uint8_t i = 0; i < 6; i++) {
    int hi = parseHexNibble(clean.charAt(i * 2));
    int lo = parseHexNibble(clean.charAt(i * 2 + 1));
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(
    buf,
    sizeof(buf),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(buf);
}

bool sendWakeOnLan(const uint8_t mac[6], const IPAddress& broadcastIp, uint16_t port, String& err) {
  err = "";
  if (WiFi.status() != WL_CONNECTED) {
    err = "WiFi is not connected.";
    return false;
  }
  if (port == 0) {
    err = "Port must be 1..65535.";
    return false;
  }

  WiFiUDP udp;
  if (!udp.begin(0)) {
    err = "UDP begin failed.";
    return false;
  }
  if (!udp.beginPacket(broadcastIp, port)) {
    udp.stop();
    err = "UDP beginPacket failed.";
    return false;
  }

  uint8_t ff[6];
  memset(ff, 0xFF, sizeof(ff));
  size_t wrote = udp.write(ff, sizeof(ff));
  if (wrote != sizeof(ff)) {
    udp.stop();
    err = "UDP write failed (header).";
    return false;
  }
  for (uint8_t i = 0; i < 16; i++) {
    wrote = udp.write(mac, 6);
    if (wrote != 6) {
      udp.stop();
      err = "UDP write failed (payload).";
      return false;
    }
  }
  if (!udp.endPacket()) {
    udp.stop();
    err = "UDP endPacket failed.";
    return false;
  }

  udp.stop();
  return true;
}

void addWebEvent(const String& text) {
  if (WEB_EVENT_CAPACITY == 0) return;

  WebEventEntry& slot = webEvents[webEventWritePos];
  slot.seq = ++webEventSeq;
  slot.tsMs = millis();
  copyToBuf(slot.text, WEB_EVENT_TEXT_LEN, text);

  webEventWritePos = (webEventWritePos + 1) % WEB_EVENT_CAPACITY;
  if (webEventCount < WEB_EVENT_CAPACITY) webEventCount++;
}

String parseCookieValue(const String& cookieHeader, const String& name) {
  String prefix = name + "=";
  int start = 0;
  while (start < cookieHeader.length()) {
    int end = cookieHeader.indexOf(';', start);
    if (end < 0) end = cookieHeader.length();

    String part = cookieHeader.substring(start, end);
    part.trim();
    if (part.startsWith(prefix)) {
      String value = part.substring(prefix.length());
      value.trim();
      return value;
    }
    start = end + 1;
  }
  return "";
}

void cleanupWebSessions() {
  unsigned long now = millis();
  for (size_t i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (!webSessions[i].used) continue;
    if (now - webSessions[i].updatedMs <= WEB_SESSION_TTL_MS) continue;
    webSessions[i] = WebSession();
  }
}

int findWebSessionIndex(const String& token) {
  if (token.length() == 0) return -1;
  for (size_t i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (!webSessions[i].used) continue;
    if (token == String(webSessions[i].token)) return static_cast<int>(i);
  }
  return -1;
}

bool isValidWebSession(const String& token) {
  cleanupWebSessions();
  int idx = findWebSessionIndex(token);
  if (idx < 0) return false;
  webSessions[idx].updatedMs = millis();
  return true;
}

String createWebSessionToken() {
  cleanupWebSessions();
  int slot = -1;
  for (size_t i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (!webSessions[i].used) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    // Reuse the oldest slot if all are busy.
    unsigned long oldest = millis();
    slot = 0;
    for (size_t i = 0; i < MAX_WEB_SESSIONS; i++) {
      if (webSessions[i].updatedMs <= oldest) {
        oldest = webSessions[i].updatedMs;
        slot = static_cast<int>(i);
      }
    }
  }

  String token = "";
  token.reserve(32);
  while (token.length() < 32) {
    uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
    if (b < 16) token += "0";
    token += String(b, HEX);
  }
  token.toLowerCase();

  webSessions[slot].used = true;
  copyToBuf(webSessions[slot].token, sizeof(webSessions[slot].token), token);
  webSessions[slot].updatedMs = millis();
  return token;
}

void removeWebSession(const String& token) {
  int idx = findWebSessionIndex(token);
  if (idx < 0) return;
  webSessions[idx] = WebSession();
}

String currentWebSessionToken() {
  if (!provisionServer.hasHeader("Cookie")) return "";
  String cookie = provisionServer.header("Cookie");
  return parseCookieValue(cookie, WEB_SESSION_COOKIE);
}

bool isWebChatAuthorized(const String& chatId) {
  if (!chatId.startsWith(WEB_CHAT_PREFIX)) return false;
  String token = chatId.substring(strlen(WEB_CHAT_PREFIX));
  return isValidWebSession(token);
}

String sanitizeRemoteKey(String key) {
  key.trim();
  key.toLowerCase();
  if (key.length() == 0 || key.length() >= REMOTE_KEY_LEN) return "";

  for (size_t i = 0; i < key.length(); i++) {
    char c = key.charAt(i);
    bool ok = (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return "";
  }
  return key;
}

String groupLabel(uint8_t groupId) {
  if (groupId >= MAX_GROUPS) return "invalid";
  if (groupNames[groupId][0] != '\0') return String(groupNames[groupId]);
  return String("Group ") + String(groupId);
}

void clearPersistentState() {
  authorizedCount = 0;
  for (size_t i = 0; i < MAX_AUTH_CHATS; i++) authorizedChats[i] = "";

  for (size_t i = 0; i < MAX_SENSORS; i++) sensors[i] = SensorEntry();
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    groupNames[g][0] = '\0';
    groupArmed[g] = false;
  }
  for (size_t i = 0; i < MAX_REMOTES; i++) remotes[i] = RemoteEntry();

  listenSavedArmed = false;
  pushServer = PushServerConfig();
  barkConfig = BarkConfig();
  smsConfig = SmsConfig();
}

void clearRuntimeState() {
  runtimeListenAllCount = 0;
  for (size_t i = 0; i < MAX_RUNTIME_LISTENERS; i++) runtimeListenAllChats[i] = "";

  for (size_t i = 0; i < MAX_WEB_SESSIONS; i++) webSessions[i] = WebSession();
  for (size_t i = 0; i < WEB_EVENT_CAPACITY; i++) webEvents[i] = WebEventEntry();
  webEventWritePos = 0;
  webEventCount = 0;
  webEventSeq = 0;
  webCommandCaptureActive = false;
  webCommandCaptureChatId = "";
  webCommandCaptureReply = "";
  smsLastSendMs = 0;
  smsLastSendCode = 0;
  smsLastResult = "never";
  barkLastSampleMs = 0;
  barkWindowStartMs = 0;
  barkLastTriggerMs = 0;
  barkWindowPeak = 0;
  barkWindowDoTriggered = false;
  barkWindowSampleCount = 0;
  barkWindowDoActiveSamples = 0;
  barkLastAoLevel = 0;
  barkLastDoLevel = -1;
  barkLastWindowPeak = 0;
  barkLastTriggerNoiseLevel = 0;
  barkLastTriggerDoActive = false;
  barkLastDoActivePct = 0;
}

bool isAuthorized(const String& chatId) {
  if (isWebChatAuthorized(chatId)) return true;
  for (size_t i = 0; i < authorizedCount; i++) {
    if (authorizedChats[i] == chatId) return true;
  }
  return false;
}

bool addAuthorizedChat(const String& chatId) {
  if (isAuthorized(chatId)) return true;
  if (authorizedCount >= MAX_AUTH_CHATS) return false;
  authorizedChats[authorizedCount++] = chatId;
  return true;
}

bool addRuntimeListenAll(const String& chatId) {
  for (size_t i = 0; i < runtimeListenAllCount; i++) {
    if (runtimeListenAllChats[i] == chatId) return true;
  }
  if (runtimeListenAllCount >= MAX_RUNTIME_LISTENERS) return false;
  runtimeListenAllChats[runtimeListenAllCount++] = chatId;
  return true;
}

bool removeRuntimeListenAll(const String& chatId) {
  for (size_t i = 0; i < runtimeListenAllCount; i++) {
    if (runtimeListenAllChats[i] != chatId) continue;
    for (size_t j = i + 1; j < runtimeListenAllCount; j++) {
      runtimeListenAllChats[j - 1] = runtimeListenAllChats[j];
    }
    runtimeListenAllChats[runtimeListenAllCount - 1] = "";
    runtimeListenAllCount--;
    return true;
  }
  return false;
}

bool isRuntimeListenAllEnabledForChat(const String& chatId) {
  for (size_t i = 0; i < runtimeListenAllCount; i++) {
    if (runtimeListenAllChats[i] == chatId) return true;
  }
  return false;
}

int findSensorByCode(uint32_t code) {
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].used && sensors[i].code == code) return static_cast<int>(i);
  }
  return -1;
}

int firstFreeSensorSlot() {
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used) return static_cast<int>(i);
  }
  return -1;
}

uint16_t countSensorsInGroup(uint8_t groupId) {
  uint16_t count = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].used && sensors[i].group == groupId) count++;
  }
  return count;
}

uint16_t countNotifySensorsInGroup(uint8_t groupId) {
  uint16_t count = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used || sensors[i].group != groupId) continue;
    if (sensors[i].notifyEnabled) count++;
  }
  return count;
}

uint16_t removeSensorsInGroup(uint8_t groupId) {
  uint16_t removed = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used || sensors[i].group != groupId) continue;
    sensors[i] = SensorEntry();
    removed++;
  }
  return removed;
}

int findRemoteByKey(const String& key) {
  String norm = sanitizeRemoteKey(key);
  if (norm.length() == 0) return -1;

  for (size_t i = 0; i < MAX_REMOTES; i++) {
    if (!remotes[i].used) continue;
    if (norm == String(remotes[i].key)) return static_cast<int>(i);
  }
  return -1;
}

int firstFreeRemoteSlot() {
  for (size_t i = 0; i < MAX_REMOTES; i++) {
    if (!remotes[i].used) return static_cast<int>(i);
  }
  return -1;
}

size_t countSavedRemotes() {
  size_t count = 0;
  for (size_t i = 0; i < MAX_REMOTES; i++) if (remotes[i].used) count++;
  return count;
}

size_t countSensorNotifyEnabled() {
  size_t count = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].used && sensors[i].notifyEnabled) count++;
  }
  return count;
}

String sensorDisplayName(const SensorEntry& s) {
  if (s.name[0] != '\0') return String(s.name);
  return String("Sensor ") + String(s.code);
}

void setWifiCreds(const String& ssid, const String& password) {
  String ssidClean = ssid;
  ssidClean.trim();
  copyToBuf(wifiCreds.ssid, WIFI_SSID_LEN, ssidClean);
  copyToBuf(wifiCreds.password, WIFI_PASS_LEN, password);
  wifiCreds.valid = ssidClean.length() > 0;
}

bool saveWiFiCredentialsFile(const String& ssid, const String& password) {
  DynamicJsonDocument doc(512);
  doc["ssid"] = ssid;
  doc["password"] = password;

  File f = SPIFFS.open(WIFI_CONFIG_FILE, "w");
  if (!f) return false;
  size_t written = serializeJson(doc, f);
  f.close();
  return written > 0;
}

bool loadWiFiCredentialsFile() {
  if (!SPIFFS.exists(WIFI_CONFIG_FILE)) return false;
  File f = SPIFFS.open(WIFI_CONFIG_FILE, "r");
  if (!f) return false;

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  ssid.trim();
  if (ssid.length() == 0) return false;

  setWifiCreds(ssid, password);
  return wifiCreds.valid;
}

void initWiFiCredentials() {
  if (loadWiFiCredentialsFile()) {
    Serial.print("WiFi credentials loaded from SPIFFS. SSID: ");
    Serial.println(wifiCreds.ssid);
    return;
  }

  setWifiCreds(String(WIFI_SSID), String(WIFI_PASSWORD));
  Serial.print("Using WiFi credentials from secrets.h. SSID: ");
  Serial.println(wifiCreds.ssid);
}

bool saveState() {
  DynamicJsonDocument doc(32768);

  JsonArray auth = doc.createNestedArray("authorized_chats");
  for (size_t i = 0; i < authorizedCount; i++) auth.add(authorizedChats[i]);

  doc["listen_saved_armed"] = listenSavedArmed;

  JsonArray groups = doc.createNestedArray("groups");
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    if (!groupArmed[g] && groupNames[g][0] == '\0') continue;
    JsonObject item = groups.createNestedObject();
    item["id"] = g;
    item["armed"] = groupArmed[g];
    if (groupNames[g][0] != '\0') item["name"] = groupNames[g];
  }

  JsonArray sensorsArray = doc.createNestedArray("sensors");
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used) continue;
    JsonObject item = sensorsArray.createNestedObject();
    item["code"] = sensors[i].code;
    item["bits"] = sensors[i].bits;
    item["group"] = sensors[i].group;
    item["notify_enabled"] = sensors[i].notifyEnabled;
    if (sensors[i].name[0] != '\0') item["name"] = sensors[i].name;
  }

  JsonObject server = doc.createNestedObject("push_server");
  server["enabled"] = pushServer.enabled;
  server["https"] = pushServer.https;
  server["host"] = pushServer.host;
  server["port"] = pushServer.port;

  JsonArray remotesArray = doc.createNestedArray("remotes");
  for (size_t i = 0; i < MAX_REMOTES; i++) {
    if (!remotes[i].used) continue;
    JsonObject item = remotesArray.createNestedObject();
    item["key"] = remotes[i].key;
    item["code"] = remotes[i].code;
    item["bits"] = remotes[i].bits;
  }

  JsonObject bark = doc.createNestedObject("bark");
  bark["enabled"] = barkConfig.enabled;
  bark["code"] = barkConfig.code;
  bark["threshold"] = barkConfig.threshold;
  bark["cooldown_ms"] = barkConfig.cooldownMs;
  bark["emit_rf"] = barkConfig.emitRf;
  bark["bits"] = barkConfig.bits;
  bark["input_mode"] = barkConfig.inputMode;
  bark["do_active_level"] = barkConfig.doActiveLevel;

  JsonObject sms = doc.createNestedObject("sms");
  sms["enabled"] = smsConfig.enabled;
  sms["saved_mode_enabled"] = smsConfig.savedModeEnabled;
  sms["group_mode_enabled"] = smsConfig.groupModeEnabled;
  sms["sensor_mode_enabled"] = smsConfig.sensorModeEnabled;
  sms["router_id"] = smsConfig.routerId;
  sms["firmware_id"] = smsConfig.firmwareId;
  sms["router_host"] = smsConfig.routerHost;
  sms["router_password"] = smsConfig.routerPassword;
  JsonArray smsPhones = sms.createNestedArray("recipient_phones");
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    smsPhones.add(smsConfig.recipientPhones[i]);
  }
  // Legacy compatibility with old single-number key.
  if (smsConfig.recipientPhones[0][0] != '\0') sms["recipient_phone"] = smsConfig.recipientPhones[0];
  sms["router_user"] = smsConfig.routerUser;

  File f = SPIFFS.open(STATE_FILE, "w");
  if (!f) return false;
  size_t written = serializeJson(doc, f);
  f.close();
  return written > 0;
}

bool loadState() {
  clearPersistentState();

  if (!SPIFFS.exists(STATE_FILE)) return true;
  File f = SPIFFS.open(STATE_FILE, "r");
  if (!f) return false;

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  JsonArray auth = doc["authorized_chats"].as<JsonArray>();
  if (!auth.isNull()) {
    for (JsonVariant v : auth) addAuthorizedChat(String((const char*)v));
  }

  listenSavedArmed = doc["listen_saved_armed"] | false;

  JsonArray groups = doc["groups"].as<JsonArray>();
  if (!groups.isNull()) {
    for (JsonVariant v : groups) {
      JsonObject item = v.as<JsonObject>();
      uint8_t id = item["id"] | 255;
      if (id >= MAX_GROUPS) continue;
      groupArmed[id] = item["armed"] | false;
      String nm = item["name"] | "";
      copyToBuf(groupNames[id], GROUP_NAME_LEN, nm);
    }
  }

  JsonArray sensorsArray = doc["sensors"].as<JsonArray>();
  if (!sensorsArray.isNull()) {
    for (JsonVariant v : sensorsArray) {
      JsonObject item = v.as<JsonObject>();
      int slot = firstFreeSensorSlot();
      if (slot < 0) break;

      SensorEntry s;
      s.used = true;
      s.code = item["code"] | 0;
      s.bits = item["bits"] | RF_BITS;
      s.group = item["group"] | 0;
      s.notifyEnabled = item["notify_enabled"] | false;
      if (s.group >= MAX_GROUPS) s.group = 0;
      String nm = item["name"] | "";
      copyToBuf(s.name, SENSOR_NAME_LEN, nm);

      sensors[slot] = s;
    }
  }

  JsonObject server = doc["push_server"].as<JsonObject>();
  if (!server.isNull()) {
    pushServer.enabled = server["enabled"] | false;
    pushServer.https = server["https"] | false;
    pushServer.port = server["port"] | 8080;
    String host = server["host"] | "";
    copyToBuf(pushServer.host, PUSH_HOST_LEN, host);
  }

  JsonArray remotesArray = doc["remotes"].as<JsonArray>();
  if (!remotesArray.isNull()) {
    for (JsonVariant v : remotesArray) {
      JsonObject item = v.as<JsonObject>();
      String key = sanitizeRemoteKey(item["key"] | "");
      if (key.length() == 0) continue;
      int slot = findRemoteByKey(key);
      if (slot < 0) slot = firstFreeRemoteSlot();
      if (slot < 0) break;

      remotes[slot].used = true;
      copyToBuf(remotes[slot].key, REMOTE_KEY_LEN, key);
      remotes[slot].code = item["code"] | 0;
      remotes[slot].bits = item["bits"] | RF_BITS;
      if (remotes[slot].bits == 0 || remotes[slot].bits > 32) remotes[slot].bits = RF_BITS;
    }
  }

  JsonObject bark = doc["bark"].as<JsonObject>();
  if (!bark.isNull()) {
    barkConfig.enabled = bark["enabled"] | false;
    barkConfig.code = bark["code"] | barkConfig.code;
    barkConfig.threshold = bark["threshold"] | barkConfig.threshold;
    barkConfig.cooldownMs = bark["cooldown_ms"] | barkConfig.cooldownMs;
    barkConfig.emitRf = bark["emit_rf"] | false;
    barkConfig.bits = bark["bits"] | RF_BITS;
    barkConfig.inputMode = bark["input_mode"] | barkConfig.inputMode;
    barkConfig.doActiveLevel = bark["do_active_level"] | barkConfig.doActiveLevel;
    if (barkConfig.bits == 0 || barkConfig.bits > 32) barkConfig.bits = RF_BITS;
    if (barkConfig.threshold > ADC_MAX_VALUE) barkConfig.threshold = ADC_MAX_VALUE;
    if (barkConfig.cooldownMs < 500) barkConfig.cooldownMs = 500;
    if (barkConfig.inputMode > BARK_MODE_BOTH) barkConfig.inputMode = BARK_MODE_DO;
    if (barkConfig.doActiveLevel > 1) barkConfig.doActiveLevel = 0;
  }

  JsonObject sms = doc["sms"].as<JsonObject>();
  if (!sms.isNull()) {
    smsConfig.enabled = sms["enabled"] | false;
    smsConfig.savedModeEnabled = sms["saved_mode_enabled"] | true;
    smsConfig.groupModeEnabled = sms["group_mode_enabled"] | true;
    smsConfig.sensorModeEnabled = sms["sensor_mode_enabled"] | true;

    String routerId = sms["router_id"] | SMS_ROUTER_TL_MR100;
    String firmwareId = sms["firmware_id"] | SMS_FIRMWARE_MR100_GDPR_V1;
    String routerHost = sms["router_host"] | "192.168.0.1";
    String routerPassword = sms["router_password"] | "";
    String routerUser = sms["router_user"] | "admin";

    routerId.trim();
    firmwareId.trim();
    routerHost.trim();
    routerUser.trim();
    if (routerUser.length() == 0) routerUser = "admin";

    if (!isSmsRouterSupported(routerId)) routerId = SMS_ROUTER_TL_MR100;
    if (!isSmsFirmwareSupported(firmwareId)) firmwareId = SMS_FIRMWARE_MR100_GDPR_V1;

    copyToBuf(smsConfig.routerId, SMS_ROUTER_ID_LEN, routerId);
    copyToBuf(smsConfig.firmwareId, SMS_FIRMWARE_ID_LEN, firmwareId);
    copyToBuf(smsConfig.routerHost, SMS_ROUTER_HOST_LEN, routerHost);
    copyToBuf(smsConfig.routerPassword, SMS_ROUTER_PASSWORD_LEN, routerPassword);
    copyToBuf(smsConfig.routerUser, sizeof(smsConfig.routerUser), routerUser);

    clearSmsRecipients();
    JsonArray recipients = sms["recipient_phones"].as<JsonArray>();
    if (!recipients.isNull()) {
      for (JsonVariant v : recipients) {
        String phone = v.as<String>();
        String addReason;
        addSmsRecipient(phone, addReason);
      }
    } else {
      // Backward compatibility with previous config format.
      String legacyPhone = sms["recipient_phone"] | "";
      String addReason;
      addSmsRecipient(legacyPhone, addReason);
    }
  }

  return true;
}

void sendToChat(const String& chatId, const String& msg) {
  if (chatId.startsWith(WEB_CHAT_PREFIX)) {
    if (webCommandCaptureActive && chatId == webCommandCaptureChatId) {
      if (webCommandCaptureReply.length() > 0) webCommandCaptureReply += "\n";
      webCommandCaptureReply += msg;
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastSkipLogMs = 0;
    unsigned long now = millis();
    if (now - lastSkipLogMs > 8000) {
      lastSkipLogMs = now;
      Serial.println("[TG] Skip send: WiFi disconnected.");
    }
    return;
  }
  BusyScope busy("telegram_send");
  unsigned long sendStartMs = millis();
  bot.sendMessage(chatId, msg, "");
  logSlowOperation("tg sendMessage", millis() - sendStartMs, 500);
}

void sendToAuthorizedChats(const String& msg) {
  for (size_t i = 0; i < authorizedCount; i++) sendToChat(authorizedChats[i], msg);
}

void sendToRuntimeListenAll(const String& msg) {
  for (size_t i = 0; i < runtimeListenAllCount; i++) sendToChat(runtimeListenAllChats[i], msg);
}

void sendChunkedMessage(const String& chatId, const String& header, const String& payload) {
  size_t totalParts = (payload.length() + CONFIG_EXPORT_CHUNK_SIZE - 1) / CONFIG_EXPORT_CHUNK_SIZE;
  if (totalParts == 0) totalParts = 1;

  sendToChat(
    chatId,
    header + "_BEGIN parts=" + String(totalParts) + " bytes=" + String(payload.length())
  );

  for (size_t i = 0; i < totalParts; i++) {
    size_t start = i * CONFIG_EXPORT_CHUNK_SIZE;
    size_t end = start + CONFIG_EXPORT_CHUNK_SIZE;
    if (end > payload.length()) end = payload.length();
    String chunk = payload.substring(start, end);
    sendToChat(chatId, header + "_PART " + String(i + 1) + "/" + String(totalParts) + "\n" + chunk);
  }

  sendToChat(chatId, header + "_END");
}

void cleanupConfigImportSessions() {
  unsigned long now = millis();
  for (size_t i = 0; i < MAX_CONFIG_IMPORT_SESSIONS; i++) {
    if (!configImportSessions[i].active) continue;
    if (now - configImportSessions[i].updatedMs <= CONFIG_IMPORT_TIMEOUT_MS) continue;
    configImportSessions[i] = ConfigImportSession();
  }
}

int findConfigImportSession(const String& chatId) {
  for (size_t i = 0; i < MAX_CONFIG_IMPORT_SESSIONS; i++) {
    if (!configImportSessions[i].active) continue;
    if (configImportSessions[i].chatId == chatId) return static_cast<int>(i);
  }
  return -1;
}

int startConfigImportSession(const String& chatId) {
  int idx = findConfigImportSession(chatId);
  if (idx >= 0) {
    configImportSessions[idx].payload = "";
    configImportSessions[idx].updatedMs = millis();
    return idx;
  }

  for (size_t i = 0; i < MAX_CONFIG_IMPORT_SESSIONS; i++) {
    if (configImportSessions[i].active) continue;
    configImportSessions[i].active = true;
    configImportSessions[i].chatId = chatId;
    configImportSessions[i].payload = "";
    configImportSessions[i].updatedMs = millis();
    return static_cast<int>(i);
  }
  return -1;
}

void cancelConfigImportSession(const String& chatId) {
  int idx = findConfigImportSession(chatId);
  if (idx < 0) return;
  configImportSessions[idx] = ConfigImportSession();
}

bool appendConfigImportChunk(const String& chatId, const String& chunk, String& err) {
  int idx = findConfigImportSession(chatId);
  if (idx < 0) {
    err = "No active import session.";
    return false;
  }

  String processed = chunk;
  String trimmed = chunk;
  trimmed.trim();

  // Allow direct reuse of exported messages by ignoring wrappers and stripping part headers.
  if (trimmed.startsWith("CONFIG_EXPORT_BEGIN") || trimmed.startsWith("CONFIG_EXPORT_END")) {
    configImportSessions[idx].updatedMs = millis();
    return true;
  }
  if (trimmed.startsWith("CONFIG_EXPORT_PART")) {
    int nl = trimmed.indexOf('\n');
    if (nl >= 0) processed = trimmed.substring(nl + 1);
    else processed = "";
  }

  size_t nextLen = configImportSessions[idx].payload.length() + processed.length() + 1;
  if (nextLen > CONFIG_IMPORT_MAX_LEN) {
    err = "Import payload too large.";
    return false;
  }

  configImportSessions[idx].payload += processed;
  configImportSessions[idx].payload += "\n";
  configImportSessions[idx].updatedMs = millis();
  return true;
}

bool buildConfigExportPayload(String& outPayload, String& err) {
  outPayload = "";
  err = "";

  if (!saveState()) {
    err = "Failed to save current state before export.";
    return false;
  }

  String stateRaw;
  if (!readFileString(STATE_FILE, stateRaw) || stateRaw.length() == 0) {
    err = "Failed to read state file.";
    return false;
  }

  DynamicJsonDocument stateDoc(32768);
  DeserializationError stateErr = deserializeJson(stateDoc, stateRaw);
  if (stateErr) {
    err = String("State JSON parse error: ") + stateErr.c_str();
    return false;
  }

  DynamicJsonDocument exportDoc(49152);
  exportDoc["format"] = "alarm_export_v1";
  exportDoc["version"] = 1;
  exportDoc["created_ms"] = millis();
  exportDoc["state"] = stateDoc.as<JsonVariantConst>();

  String wifiRaw;
  if (readFileString(WIFI_CONFIG_FILE, wifiRaw) && wifiRaw.length() > 0) {
    DynamicJsonDocument wifiDoc(1024);
    DeserializationError wifiErr = deserializeJson(wifiDoc, wifiRaw);
    if (!wifiErr) {
      exportDoc["wifi"] = wifiDoc.as<JsonVariantConst>();
    } else {
      JsonObject wifi = exportDoc.createNestedObject("wifi");
      wifi["ssid"] = wifiCreds.ssid;
      wifi["password"] = wifiCreds.password;
    }
  } else {
    JsonObject wifi = exportDoc.createNestedObject("wifi");
    wifi["ssid"] = wifiCreds.ssid;
    wifi["password"] = wifiCreds.password;
  }

  if (serializeJson(exportDoc, outPayload) == 0) {
    err = "Failed to serialize export payload.";
    return false;
  }
  return true;
}

bool applyImportedConfig(const String& payload, String& err) {
  err = "";
  if (payload.length() == 0) {
    err = "Empty payload.";
    return false;
  }
  if (payload.length() > CONFIG_IMPORT_MAX_LEN) {
    err = "Payload too large.";
    return false;
  }

  String stateBackup;
  String wifiBackup;
  bool hadStateBackup = readFileString(STATE_FILE, stateBackup);
  bool hadWifiBackup = readFileString(WIFI_CONFIG_FILE, wifiBackup);

  DynamicJsonDocument doc(65536);
  DeserializationError parseErr = deserializeJson(doc, payload);
  if (parseErr) {
    err = String("JSON parse error: ") + parseErr.c_str();
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) {
    err = "Root must be a JSON object.";
    return false;
  }

  JsonVariant stateVar = doc.as<JsonVariant>();
  if (root.containsKey("state")) stateVar = root["state"];
  if (!stateVar.is<JsonObject>()) {
    err = "Missing or invalid state object.";
    return false;
  }

  String stateSerialized;
  if (serializeJson(stateVar, stateSerialized) == 0) {
    err = "Failed to serialize state from import payload.";
    return false;
  }

  bool wifiProvided = false;
  String wifiSsid = "";
  String wifiPassword = "";
  if (root.containsKey("wifi") && root["wifi"].is<JsonObject>()) {
    JsonObject wifiObj = root["wifi"].as<JsonObject>();
    wifiSsid = wifiObj["ssid"] | "";
    wifiPassword = wifiObj["password"] | "";
    wifiSsid.trim();
    if (wifiSsid.length() == 0) {
      err = "wifi.ssid cannot be empty when wifi object is provided.";
      return false;
    }
    wifiProvided = true;
  }

  if (!writeFileString(STATE_FILE, stateSerialized)) {
    err = "Failed to write state file.";
    return false;
  }

  if (wifiProvided) {
    if (!saveWiFiCredentialsFile(wifiSsid, wifiPassword)) {
      err = "Failed to write WiFi config file.";
      if (hadStateBackup) writeFileString(STATE_FILE, stateBackup);
      else SPIFFS.remove(STATE_FILE);
      return false;
    }
    setWifiCreds(wifiSsid, wifiPassword);
  }

  if (!loadState()) {
    if (hadStateBackup) writeFileString(STATE_FILE, stateBackup);
    else SPIFFS.remove(STATE_FILE);
    if (hadWifiBackup) writeFileString(WIFI_CONFIG_FILE, wifiBackup);
    else SPIFFS.remove(WIFI_CONFIG_FILE);
    loadState();
    initWiFiCredentials();
    clearRuntimeState();
    err = "Imported state failed to load; previous config restored.";
    return false;
  }

  if (!wifiProvided) initWiFiCredentials();
  clearRuntimeState();  // runtime listeners are always volatile
  return true;
}

void sendRfSignal(uint32_t code, uint8_t bits, uint8_t repeats = 6) {
  BusyScope busy("rf_send");
  unsigned long startMs = millis();
  for (uint8_t i = 0; i < repeats; i++) {
    rf.send(code, bits);
    cooperativeDelayMs(120);
  }
  logSlowOperation("rf.send", millis() - startMs, 300);
}

void sendPanelSignal(bool arm) {
  uint32_t code = arm ? PANEL_ARM_CODE : PANEL_DISARM_CODE;
  uint8_t bits = RF_BITS;

  int remoteIdx = findRemoteByKey(arm ? PANEL_ARM_REMOTE_KEY : PANEL_DISARM_REMOTE_KEY);
  if (remoteIdx >= 0) {
    code = remotes[remoteIdx].code;
    bits = remotes[remoteIdx].bits;
  }

  sendRfSignal(code, bits);
}

String pushBaseUrl() {
  String url = pushServer.https ? "https://" : "http://";
  url += String(pushServer.host);
  url += ":";
  url += String(pushServer.port);
  return url;
}

bool postPushEvent(DynamicJsonDocument& doc) {
  BusyScope busy("push_post");
  unsigned long postStartMs = millis();
  if (!pushServer.enabled || pushServer.host[0] == '\0') return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (pushBackoffUntilMs != 0 && millis() < pushBackoffUntilMs) return false;

  doc["device"] = "esp32-alarm";
  doc["uptime_ms"] = millis();
  doc["ip"] = WiFi.localIP().toString();
  doc["listen_saved_armed"] = listenSavedArmed;
  doc["listen_all_runtime_count"] = runtimeListenAllCount;

  String body;
  serializeJson(doc, body);
  String url = pushBaseUrl() + PUSH_EVENT_ROUTE;

  int statusCode = -1;
  HTTPClient http;

  if (pushServer.https) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, url)) return false;
    http.setConnectTimeout(PUSH_HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(PUSH_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    statusCode = http.POST(body);
    http.end();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) return false;
    http.setConnectTimeout(PUSH_HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(PUSH_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    statusCode = http.POST(body);
    http.end();
  }

  bool ok = statusCode >= 200 && statusCode < 300;
  unsigned long tookMs = millis() - postStartMs;
  if (!ok || tookMs >= PERF_SLOW_OP_LOG_MS) {
    logSlowOperation("push post status=" + String(statusCode), tookMs, 300);
  }
  if (ok) {
    pushConsecutiveFailures = 0;
    pushBackoffUntilMs = 0;
    return true;
  }

  if (pushConsecutiveFailures < 6) pushConsecutiveFailures++;
  uint32_t backoff = 300UL * (1UL << pushConsecutiveFailures);  // 600..19200ms
  if (backoff > 8000UL) backoff = 8000UL;
  pushBackoffUntilMs = millis() + backoff;
  return false;
}

void pushModeEvent(const String& mode, bool active, const String& chatId, int groupId = -1) {
  DynamicJsonDocument doc(1536);
  doc["event"] = "mode_change";
  doc["mode"] = mode;
  doc["active"] = active;
  doc["chat_id"] = chatId;

  if (groupId >= 0 && groupId < MAX_GROUPS) {
    doc["group_id"] = groupId;
    doc["group_name"] = groupLabel(static_cast<uint8_t>(groupId));
  }

  JsonArray armedGroups = doc.createNestedArray("armed_groups");
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    if (groupArmed[g]) armedGroups.add(g);
  }

  postPushEvent(doc);
}

void pushResetEvent(const String& chatId) {
  DynamicJsonDocument doc(512);
  doc["event"] = "reset_all";
  doc["chat_id"] = chatId;
  postPushEvent(doc);
}

void pushSensorEvent(
  uint32_t code,
  uint8_t bits,
  uint8_t protocol,
  uint16_t pulseUs,
  bool known,
  bool throughSavedMode,
  bool throughGroupMode,
  bool throughSensorMode,
  const String& source,
  const SensorEntry* sensor
) {
  if (!pushServer.enabled) return;
  if (!(runtimeListenAllCount > 0 || throughSavedMode || throughGroupMode || throughSensorMode)) return;

  DynamicJsonDocument doc(2048);
  doc["event"] = "sensor_rx";
  doc["source"] = source;
  doc["code"] = code;
  doc["bits"] = bits;
  doc["protocol"] = protocol;
  doc["pulse_us"] = pulseUs;
  doc["known_sensor"] = known;
  doc["through_listen_all"] = runtimeListenAllCount > 0;
  doc["through_saved_mode"] = throughSavedMode;
  doc["through_group_mode"] = throughGroupMode;
  doc["through_sensor_mode"] = throughSensorMode;

  if (known && sensor != nullptr) {
    doc["sensor_name"] = sensorDisplayName(*sensor);
    doc["group_id"] = sensor->group;
    doc["group_name"] = groupLabel(sensor->group);
    doc["group_armed"] = groupArmed[sensor->group];
    doc["sensor_notify_enabled"] = sensor->notifyEnabled;
  }

  postPushEvent(doc);
}

bool parseServerTarget(String raw, bool& https, String& host, uint16_t& port, bool& portPresentInTarget) {
  raw.trim();
  if (raw.length() == 0) return false;

  https = false;
  host = raw;
  portPresentInTarget = false;

  String lower = lowerCopy(host);
  if (lower.startsWith("http://")) {
    host = host.substring(7);
    https = false;
  } else if (lower.startsWith("https://")) {
    host = host.substring(8);
    https = true;
  }

  int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);

  int colon = host.lastIndexOf(':');
  if (colon > 0) {
    String p = host.substring(colon + 1);
    uint16_t parsedPort = 0;
    if (!parseUInt16(p, parsedPort)) return false;
    port = parsedPort;
    host = host.substring(0, colon);
    portPresentInTarget = true;
  }

  host.trim();
  return host.length() > 0;
}

String buildHelpText(bool authed) {
  String msg;
  msg += "Alarm bot commands:\n";
  msg += "/auth <code>\n";
  msg += "/help\n";

  if (!authed) {
    msg += "\nAuthenticate first for full access.";
    return msg;
  }

  msg += "\nModes:\n";
  msg += "/listen_all_on | /listen_all_off\n";
  msg += "/listen_saved_on | /listen_saved_off\n";
  msg += "(also supports plain text variants from your spec)\n";

  msg += "\nSensors:\n";
  msg += "/sensor_add <code> <group 0-99> [name]\n";
  msg += "/sensor_remove <code>\n";
  msg += "/sensor_name <code> <name>\n";
  msg += "/sensor_notify_on <code>\n";
  msg += "/sensor_notify_off <code>\n";
  msg += "/sensor_notify_list\n";
  msg += "/sensors\n";

  msg += "\nRemotes:\n";
  msg += "/remote_add <key> <code> [bits]\n";
  msg += "/remote_remove <key>\n";
  msg += "/remote_send <key>\n";
  msg += "/remotes\n";

  msg += "\nBark detector:\n";
  msg += "/bark_on | /bark_off\n";
  msg += "/bark_status\n";
  msg += "/bark_mode <ao|do|both>\n";
  msg += "/bark_do_level <0|1>\n";
  msg += "/bark_threshold <0-4095>\n";
  msg += "/bark_cooldown <ms>\n";
  msg += "/bark_code <code>\n";
  msg += "/bark_emit_rf_on | /bark_emit_rf_off\n";
  msg += "/bark_test\n";

  msg += "\nGroups:\n";
  msg += "/group_set <group> <name>\n";
  msg += "/group_reset <group>\n";
  msg += "/group_arm <group>\n";
  msg += "/group_disarm <group>\n";
  msg += "/groups\n";

  msg += "\nPush server:\n";
  msg += "/server_set <host_or_url> [port]\n";
  msg += "/server_show\n";
  msg += "/server_clear\n";

  msg += "\nRouter SMS (direct):\n";
  msg += "/sms_status\n";
  msg += "/sms_on | /sms_off\n";
  msg += "/sms_router_list\n";
  msg += "/sms_router_set <id>\n";
  msg += "/sms_fw_list\n";
  msg += "/sms_fw_set <id>\n";
  msg += "/sms_ip <router_ip_or_host>\n";
  msg += "/sms_password <router_password>\n";
  msg += "/sms_to_add <phone_number>\n";
  msg += "/sms_to_remove <phone_number>\n";
  msg += "/sms_to_list\n";
  msg += "/sms_to_clear\n";
  msg += "/sms_to <phone_number> (alias for add)\n";
  msg += "/sms_mode_saved_on | /sms_mode_saved_off\n";
  msg += "/sms_mode_group_on | /sms_mode_group_off\n";
  msg += "/sms_mode_sensor_on | /sms_mode_sensor_off\n";
  msg += "/sms_test [message]\n";

  msg += "\nSystem:\n";
  msg += "/status\n";
  msg += "/wol <mac> [broadcast_ip] [port]\n";
  msg += "/config_export\n";
  msg += "/config_import <json>\n";
  msg += "/config_import_begin\n";
  msg += "/config_import_end\n";
  msg += "/config_import_cancel\n";
  msg += "/reset_all\n";
  return msg;
}

String buildStatusText() {
  size_t sensorCount = 0;
  size_t armedGroups = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) if (sensors[i].used) sensorCount++;
  for (uint8_t g = 0; g < MAX_GROUPS; g++) if (groupArmed[g]) armedGroups++;
  size_t remoteCount = countSavedRemotes();
  size_t sensorNotifyCount = countSensorNotifyEnabled();

  String msg;
  msg += "Status:\n";
  msg += "WiFi: ";
  msg += (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected";
  msg += "\nIP: ";
  msg += WiFi.localIP().toString();
  msg += "\nConfigured SSID: ";
  msg += String(wifiCreds.ssid);
  msg += "\nAuthorized chats: " + String(authorizedCount);
  msg += "\nRuntime listen-all chats: " + String(runtimeListenAllCount);
  msg += "\nSaved mode armed: ";
  msg += listenSavedArmed ? "yes" : "no";
  msg += "\nSensors saved: " + String(sensorCount);
  msg += "\nSensor notify enabled: " + String(sensorNotifyCount);
  msg += "\nRemotes saved: " + String(remoteCount);
  msg += "\nArmed groups: " + String(armedGroups);
  msg += "\nBark detector: ";
  msg += barkConfig.enabled ? "on" : "off";
  msg += " | mode=" + barkModeText(barkConfig.inputMode);
  msg += " | doLevel=" + String(barkConfig.doActiveLevel);
  msg += " | threshold=" + String(barkConfig.threshold);
  msg += " | cooldownMs=" + String(barkConfig.cooldownMs);
  msg += " | code=" + String(barkConfig.code);
  msg += " | emitRF=";
  msg += barkConfig.emitRf ? "yes" : "no";
  msg += " | lastNoise=" + String(barkLastTriggerNoiseLevel);
  msg += " | lastDoPct=" + String(barkLastDoActivePct) + "%";
  msg += "\nWiFi AP mode: ";
  msg += wifiProvisioningMode ? "enabled" : "off";
  if (wifiProvisioningMode) {
    msg += "\nAP SSID: " + wifiProvisionApSsid;
    msg += "\nAP IP: " + WiFi.softAPIP().toString();
  }
  msg += "\nOffline AP mode: ";
  msg += offlineApMode ? "enabled" : "off";
  if (offlineApMode) {
    msg += "\nAP SSID: " + wifiProvisionApSsid;
    msg += "\nAP IP: " + WiFi.softAPIP().toString();
  }
  msg += "\nPush server: ";
  if (pushServer.enabled && pushServer.host[0] != '\0') {
    msg += pushBaseUrl();
  } else {
    msg += "disabled";
  }

  String smsCfgReason;
  bool smsReady = isSmsConfigured(smsCfgReason);
  msg += "\nSMS direct: ";
  msg += smsConfig.enabled ? "enabled" : "disabled";
  msg += " | router=" + String(smsConfig.routerId);
  msg += " | fw=" + String(smsConfig.firmwareId);
  msg += "\nSMS router host: " + String(smsConfig.routerHost);
  msg += "\nSMS recipients (" + String(smsRecipientCount()) + "/" + String(MAX_SMS_RECIPIENTS) + "): ";
  msg += smsRecipientsInline();
  msg += "\nSMS modes: saved=";
  msg += smsConfig.savedModeEnabled ? "on" : "off";
  msg += " group=";
  msg += smsConfig.groupModeEnabled ? "on" : "off";
  msg += " sensor=";
  msg += smsConfig.sensorModeEnabled ? "on" : "off";
  msg += "\nSMS config state: ";
  msg += smsReady ? "ready" : ("incomplete (" + smsCfgReason + ")");
  msg += "\nSMS last result: " + smsLastResult;
  return msg;
}

String buildSensorsList() {
  String msg = "Saved sensors:\n";
  size_t found = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used) continue;
    found++;
    msg += String(found) + ". ";
    msg += String(sensors[i].code);
    msg += " | ";
    msg += sensorDisplayName(sensors[i]);
    msg += " | group ";
    msg += String(sensors[i].group);
    msg += " (" + groupLabel(sensors[i].group) + ")";
    msg += " | notify=";
    msg += sensors[i].notifyEnabled ? "on" : "off";
    msg += "\n";
  }
  if (found == 0) msg += "(none)\n";
  return msg;
}

String buildSensorNotifyList() {
  String msg = "Sensor notify list:\n";
  size_t found = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used || !sensors[i].notifyEnabled) continue;
    found++;
    msg += String(found) + ". ";
    msg += String(sensors[i].code);
    msg += " | ";
    msg += sensorDisplayName(sensors[i]);
    msg += " | group ";
    msg += String(sensors[i].group);
    msg += " (" + groupLabel(sensors[i].group) + ")";
    msg += "\n";
  }
  if (found == 0) msg += "(none)\n";
  return msg;
}

String buildBarkStatusText() {
  String msg = "Bark detector:\n";
  bool aoRelevant = (barkConfig.inputMode != BARK_MODE_DO);
  msg += "enabled: ";
  msg += barkConfig.enabled ? "yes" : "no";
  msg += "\nmic_pin: GPIO";
  msg += String(BARK_MIC_PIN);
  msg += "\nmode: ";
  msg += barkModeText(barkConfig.inputMode);
  msg += "\ndo_active_level: ";
  msg += String(barkConfig.doActiveLevel);
  msg += "\nthreshold: ";
  msg += String(barkConfig.threshold);
  msg += "\ncooldown_ms: ";
  msg += String(barkConfig.cooldownMs);
  msg += "\nvirtual_sensor_code: ";
  msg += String(barkConfig.code);
  msg += "\nbits: ";
  msg += String(barkConfig.bits);
  msg += "\nemit_rf: ";
  msg += barkConfig.emitRf ? "yes" : "no";
  msg += "\nlast_ao_level: ";
  if (aoRelevant) msg += String(barkLastAoLevel);
  else msg += "n/a (do mode)";
  msg += "\nlast_do_level: ";
  msg += String(barkLastDoLevel);
  msg += "\nlast_window_peak: ";
  if (aoRelevant) msg += String(barkLastWindowPeak);
  else msg += "n/a (do mode)";
  msg += "\nlast_trigger_noise: ";
  msg += String(barkLastTriggerNoiseLevel);
  msg += "\nlast_do_active_pct: ";
  msg += String(barkLastDoActivePct);
  msg += "%";
  msg += "\nlast_trigger_do_active: ";
  msg += barkLastTriggerDoActive ? "yes" : "no";
  return msg;
}

String buildRemotesList() {
  String msg = "Saved remotes:\n";
  size_t found = 0;
  for (size_t i = 0; i < MAX_REMOTES; i++) {
    if (!remotes[i].used) continue;
    found++;
    msg += String(found);
    msg += ". ";
    msg += String(remotes[i].key);
    msg += " | code=";
    msg += String(remotes[i].code);
    msg += " | bits=";
    msg += String(remotes[i].bits);
    msg += "\n";
  }
  if (found == 0) msg += "(none)\n";
  return msg;
}

String buildGroupsList() {
  String msg = "Groups:\n";
  bool any = false;
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    uint16_t sensorCount = countSensorsInGroup(g);
    bool hasName = groupNames[g][0] != '\0';
    if (!hasName && sensorCount == 0 && !groupArmed[g]) continue;
    any = true;
    msg += String(g);
    msg += " | ";
    msg += groupLabel(g);
    msg += " | sensors: ";
    msg += String(sensorCount);
    msg += " | armed: ";
    msg += groupArmed[g] ? "yes" : "no";
    if (sensorCount > 0) {
      msg += "\n  attached: ";
      bool first = true;
      for (size_t i = 0; i < MAX_SENSORS; i++) {
        if (!sensors[i].used || sensors[i].group != g) continue;
        if (!first) msg += ", ";
        first = false;
        msg += sensorDisplayName(sensors[i]);
        msg += " [";
        msg += String(sensors[i].code);
        msg += "]";
      }
    }
    msg += "\n";
  }
  if (!any) msg += "(none)\n";
  return msg;
}

bool connectWiFiWithRetries(uint8_t maxAttempts);
String htmlEscape(const String& input);
int buildSsidOptionsFromScanCount(int n, String& ssidOptions);
void tickProvisioningWifiScan();
String webWifiSetupPageHtml(
  const String& ssidOptionsHtml,
  const String& currentSsid,
  bool scanHasResults,
  bool scanInProgress
);
void handleCommand(const String& chatId, String text);
void startProvisioningApMode();
void setupWebRoutes();
void ensureWebServerStarted();
void processWebServer();
bool ensureWebApiAuthorized();
String webLoginPageHtml();
String webAppPageHtml();
void sendWebJsonError(int code, const String& message);
void sendWebJsonOk(DynamicJsonDocument& doc);
bool runWebCommandForSession(const String& sessionToken, const String& command, String& reply);
void fillOverviewJson(DynamicJsonDocument& doc);
void handleSensorCodeEvent(
  uint32_t code,
  uint8_t bits,
  uint8_t protocol,
  uint16_t pulseUs,
  bool applyDuplicateFilter,
  const String& source
);

void ensureWiFiConnected() {
  if (wifiProvisioningMode) return;
  if (offlineApMode) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - wifiRetryMs < WIFI_RETRY_INTERVAL_MS) return;
  wifiRetryMs = millis();

  Serial.println("WiFi disconnected, reconnecting...");
  if (connectWiFiWithRetries(WIFI_MAX_CONNECT_ATTEMPTS)) return;
  startProvisioningApMode();
}

bool connectWiFiWithRetries(uint8_t maxAttempts) {
  if (!wifiCreds.valid) return false;

  WiFi.setAutoReconnect(true);

  if (wifiProvisioningMode) {
    WiFi.softAPdisconnect(true);
    wifiProvisioningMode = false;
  }

  WiFi.mode(WIFI_STA);
  for (uint8_t attempt = 1; attempt <= maxAttempts; attempt++) {
    Serial.printf("WiFi attempt %u/%u -> SSID: %s\n", attempt, maxAttempts, wifiCreds.ssid);
    WiFi.begin(wifiCreds.ssid, wifiCreds.password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_ATTEMPT_TIMEOUT_MS) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      offlineApMode = false;
      Serial.print("WiFi connected. IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    Serial.println("WiFi attempt failed.");
    WiFi.disconnect();
    delay(250);
  }
  return false;
}

String htmlEscape(const String& input) {
  String out = input;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

void tickProvisioningWifiScan() {
  if (!wifiProvisioningMode && !offlineApMode) return;

  int state = WiFi.scanComplete();
  unsigned long now = millis();

  if (state == WIFI_SCAN_STATE_RUNNING) {
    if (wifiScanStartedMs != 0 && (now - wifiScanStartedMs) > WIFI_SCAN_TIMEOUT_MS) {
      Serial.println("WiFi scan timeout, restarting scan.");
      WiFi.scanDelete();
      wifiScanStartedMs = 0;
      wifiScanNextRefreshMs = 0;
    }
    return;
  }

  if (state >= 0) {
    String options;
    int listed = buildSsidOptionsFromScanCount(state, options);
    if (listed > 0) {
      wifiScanOptionsCache = options;
      wifiScanCacheMs = now;
      Serial.printf("WiFi scan completed: %d SSIDs listed.\n", listed);
    } else {
      Serial.println("WiFi scan completed: 0 visible SSIDs.");
    }
    WiFi.scanDelete();
    wifiScanStartedMs = 0;
    wifiScanNextRefreshMs = now + WIFI_SCAN_REFRESH_MS;
    return;
  }

  if (state == WIFI_SCAN_STATE_FAILED && wifiScanStartedMs != 0) {
    if (wifiScanNextRefreshMs != 0 && now < wifiScanNextRefreshMs) return;
    Serial.println("WiFi scan failed, retrying soon.");
    WiFi.scanDelete();
    wifiScanStartedMs = 0;
    wifiScanNextRefreshMs = now + WIFI_SCAN_RETRY_MS;
  }

  if (wifiScanStartedMs != 0) return;
  if (wifiScanNextRefreshMs != 0 && now < wifiScanNextRefreshMs) return;

  // Keep AP available and ensure STA is not stuck in connect/reconnect while we request scan.
  WiFi.disconnect(false, false);
  int rc = WiFi.scanNetworks(true, true);
  if (rc == WIFI_SCAN_STATE_RUNNING) {
    wifiScanStartedMs = now;
    Serial.println("WiFi async scan started.");
    return;
  }
  if (rc >= 0) {
    String options;
    int listed = buildSsidOptionsFromScanCount(rc, options);
    if (listed > 0) {
      wifiScanOptionsCache = options;
      wifiScanCacheMs = now;
      Serial.printf("WiFi immediate scan: %d SSIDs listed.\n", listed);
    }
    WiFi.scanDelete();
    wifiScanNextRefreshMs = now + WIFI_SCAN_REFRESH_MS;
    return;
  }
  Serial.printf("WiFi scan start returned %d, retrying.\n", rc);
  wifiScanNextRefreshMs = now + WIFI_SCAN_RETRY_MS;
}

int buildSsidOptionsFromScanCount(int n, String& ssidOptions) {
  ssidOptions = "";
  if (n <= 0) return 0;
  ssidOptions.reserve(2500);
  int listed = 0;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    ssid.trim();
    if (ssid.length() == 0) continue;
    ssidOptions += "<option value='";
    ssidOptions += htmlEscape(ssid);
    ssidOptions += "'>";
    ssidOptions += htmlEscape(ssid);
    ssidOptions += " (";
    ssidOptions += String(WiFi.RSSI(i));
    ssidOptions += " dBm)</option>";
    listed++;
  }
  return listed;
}

String webWifiSetupPageHtml(
  const String& ssidOptionsHtml,
  const String& currentSsid,
  bool scanHasResults,
  bool scanInProgress
) {
  String page = R"WIFI_SETUP_HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Alarm WiFi Setup</title>
  <style>
    :root { color-scheme: light; --bg:#f2f6fb; --card:#ffffff; --line:#d4ddea; --text:#152033; --muted:#5c6f84; --accent:#1d3557; --accent2:#2a9d8f; --warn:#7d4f00; }
    * { box-sizing:border-box; }
    body { margin:0; font-family:"Trebuchet MS","Segoe UI",sans-serif; background:radial-gradient(circle at top,#e9f0fa,#f2f6fb 55%); color:var(--text); min-height:100vh; display:flex; align-items:center; justify-content:center; padding:16px; }
    .card { width:100%; max-width:720px; background:var(--card); border:1px solid var(--line); border-radius:16px; padding:16px; box-shadow:0 10px 24px rgba(17,24,39,0.08); display:grid; gap:12px; }
    h1 { margin:0; font-size:24px; line-height:1.15; }
    .sub { margin:0; color:var(--muted); font-size:13px; }
    .stack { display:grid; gap:8px; }
    label { font-size:13px; color:#233448; font-weight:700; }
    input, select { width:100%; border:1px solid var(--line); border-radius:10px; padding:10px; font-size:14px; background:#fff; color:var(--text); }
    .actions { display:flex; gap:8px; flex-wrap:wrap; }
    .btn, button { border:none; border-radius:10px; padding:10px 12px; font-size:14px; font-weight:700; color:#fff; background:linear-gradient(135deg,var(--accent),var(--accent2)); text-decoration:none; display:inline-flex; align-items:center; justify-content:center; }
    .btn.ghost { background:#5f6b76; }
    .btn.secondary { background:linear-gradient(135deg,#364f6b,#3f6680); }
    .warn { border:1px solid #f4d5a5; background:#fff3df; color:var(--warn); border-radius:10px; padding:10px; font-size:13px; }
    .note { margin:0; color:var(--muted); font-size:12px; }
    .divider { height:1px; background:var(--line); margin:2px 0; }
    @media (max-width: 560px) {
      .btn, button { width:100%; }
      .actions { display:grid; }
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>Alarm WiFi Setup</h1>
    <p class="sub">Connect to a router or continue in offline AP mode with the local web interface.</p>
    __SCAN_ALERT__
    <form method="POST" action="/save" class="stack">
      <label>Detected SSIDs</label>
      <select name="ssid_select">
        <option value="">-- select --</option>
        __SSID_OPTIONS__
      </select>
      <label>Or hidden SSID</label>
      <input name="ssid" placeholder="WiFi SSID">
      <label>Password</label>
      <input type="password" name="password" placeholder="WiFi password">
      <div class="actions">
        <button type="submit">Save and Reboot</button>
        <a class="btn ghost" href="/wifi_setup">Rescan Networks</a>
      </div>
    </form>
    <div class="divider"></div>
    <form method="POST" action="/continue_offline">
      <button class="btn secondary" type="submit">Continue Without WiFi</button>
    </form>
    <p class="note">AP SSID: __AP_SSID__</p>
    <p class="note">Stored SSID: __CURRENT_SSID__</p>
  </div>
  __AUTO_REFRESH__
</body>
</html>
)WIFI_SETUP_HTML";

  String scanAlert = "";
  String autoRefresh = "";
  if (!scanHasResults && scanInProgress) {
    scanAlert = "<div class='warn'>Scanning nearby WiFi networks. This page refreshes automatically.</div>";
    autoRefresh = "<script>setTimeout(function(){window.location.replace('/wifi_setup');}, 1800);</script>";
  } else if (!scanHasResults) {
    scanAlert = "<div class='warn'>No SSIDs detected right now. Tap <b>Rescan Networks</b> or enter hidden SSID manually.</div>";
  }
  String safeCurrentSsid = currentSsid.length() ? htmlEscape(currentSsid) : "(none)";
  page.replace("__SCAN_ALERT__", scanAlert);
  page.replace("__SSID_OPTIONS__", ssidOptionsHtml);
  page.replace("__AP_SSID__", htmlEscape(wifiProvisionApSsid));
  page.replace("__CURRENT_SSID__", safeCurrentSsid);
  page.replace("__AUTO_REFRESH__", autoRefresh);
  return page;
}

String webLoginPageHtml() {
  return R"LOGIN_HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Alarm Login</title>
  <style>
    :root { color-scheme: light; --bg:#f5f7fb; --card:#ffffff; --text:#18202a; --muted:#5e6b7a; --line:#d7dee8; --accent:#006d77; --accent2:#0a9396; }
    * { box-sizing:border-box; }
    body { margin:0; font-family: "Trebuchet MS", "Segoe UI", sans-serif; background:linear-gradient(160deg,#f7f9fc,#ebf4f6); color:var(--text); min-height:100vh; display:flex; align-items:center; justify-content:center; padding:20px; }
    .card { width:100%; max-width:420px; background:var(--card); border:1px solid var(--line); border-radius:16px; padding:20px; box-shadow:0 12px 30px rgba(0,0,0,0.08); }
    h1 { margin:0 0 8px; font-size:24px; }
    p { margin:0 0 16px; color:var(--muted); }
    input { width:100%; padding:12px; border:1px solid var(--line); border-radius:12px; font-size:16px; margin-bottom:12px; }
    button { width:100%; border:none; border-radius:12px; padding:12px; background:linear-gradient(135deg,var(--accent),var(--accent2)); color:#fff; font-size:16px; font-weight:700; }
    .note { margin-top:12px; font-size:13px; color:var(--muted); }
    .err { margin-top:10px; color:#b00020; min-height:20px; font-size:14px; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Alarm Web</h1>
    <p>Enter the same authentication code used in Telegram.</p>
    <form id="loginForm">
      <input id="code" type="password" placeholder="Authentication code" autocomplete="current-password" required>
      <button type="submit">Sign In</button>
    </form>
    <div id="err" class="err"></div>
    <div class="note">Session is local to this browser and expires automatically.</div>
  </div>

  <script>
    const form = document.getElementById('loginForm');
    const err = document.getElementById('err');
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      err.textContent = '';
      const code = document.getElementById('code').value.trim();
      const body = new URLSearchParams();
      body.set('code', code);
      try {
        const res = await fetch('/api/login', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body.toString()
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok || !data.ok) {
          err.textContent = data.error || 'Login failed';
          return;
        }
        window.location.href = '/app';
      } catch (ex) {
        err.textContent = 'Network error';
      }
    });
  </script>
</body>
</html>
)LOGIN_HTML";
}

String webAppPageHtml() {
  return R"APP_HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Alarm Control</title>
  <style>
    :root { color-scheme: light; --bg:#f2f6fb; --card:#ffffff; --line:#d4ddea; --text:#152033; --muted:#5c6f84; --accent:#1d3557; --accent2:#2a9d8f; --danger:#bc4749; --ok:#2d6a4f; --warn:#c77d00; }
    * { box-sizing:border-box; }
    body { margin:0; font-family:"Trebuchet MS","Segoe UI",sans-serif; background:radial-gradient(circle at top,#e9f0fa,#f2f6fb 55%); color:var(--text); }
    .shell { max-width:1240px; margin:0 auto; padding:12px; display:grid; gap:12px; }
    .card { background:var(--card); border:1px solid var(--line); border-radius:14px; padding:12px; box-shadow:0 6px 16px rgba(17,24,39,0.06); }
    .header { display:grid; gap:10px; }
    h1 { margin:0; font-size:24px; line-height:1.1; }
    h2 { margin:0 0 10px; font-size:17px; }
    .sub { color:var(--muted); font-size:13px; }
    .nav { display:flex; gap:8px; flex-wrap:wrap; }
    .navBtn { border:1px solid var(--line); border-radius:999px; padding:8px 12px; background:#fff; color:#1d2f45; font-weight:700; }
    .navBtn.active { background:#eaf2ff; border-color:#b6cceb; }
    .chips { display:flex; flex-wrap:wrap; gap:8px; }
    .chip { font-size:12px; padding:6px 10px; border-radius:999px; border:1px solid var(--line); background:#f8fbff; color:#233448; }
    .chip.ok { background:#e7f5eb; color:#1b5e34; border-color:#b8e2c5; }
    .chip.warn { background:#fff3df; color:#7d4f00; border-color:#f4d5a5; }
    .grid { display:grid; gap:12px; }
    .row2 { display:grid; gap:12px; }
    .btn, button { border:none; border-radius:10px; padding:10px 11px; font-size:14px; font-weight:700; color:#fff; background:linear-gradient(135deg,var(--accent),var(--accent2)); }
    .btn.secondary { background:#5f6b76; }
    .btn.warn { background:linear-gradient(135deg,#d62828,var(--danger)); }
    .btn.yellow { background:linear-gradient(135deg,#e09f3e,var(--warn)); }
    .btn.ghost { background:#5f6b76; }
    .btn.small { padding:7px 9px; font-size:12px; }
    .stack { display:grid; gap:8px; }
    .inline { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    input, select, textarea { width:100%; border:1px solid var(--line); border-radius:10px; padding:10px; font-size:14px; background:#fff; color:var(--text); }
    textarea { min-height:190px; resize:vertical; font-family:ui-monospace, SFMono-Regular, Menlo, monospace; font-size:12px; line-height:1.35; }
    .cols { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
    pre { margin:0; white-space:pre-wrap; word-break:break-word; max-height:360px; overflow:auto; font-size:12px; line-height:1.4; background:#f8fbff; border:1px solid var(--line); border-radius:10px; padding:10px; }
    .list { display:grid; gap:8px; }
    .item { border:1px solid var(--line); border-radius:12px; padding:10px; background:#fcfdff; }
    .itemHead { display:flex; justify-content:space-between; align-items:center; gap:8px; margin-bottom:8px; }
    .title { font-weight:700; }
    .mutedTiny { color:var(--muted); font-size:12px; }
    .tableWrap { overflow:auto; border:1px solid var(--line); border-radius:10px; }
    table { width:100%; border-collapse:collapse; min-width:760px; font-size:13px; }
    th, td { padding:8px; border-bottom:1px solid #e4ebf3; text-align:left; vertical-align:middle; }
    th { background:#f7fbff; position:sticky; top:0; z-index:1; }
    .switch { position:relative; width:46px; height:26px; display:inline-block; }
    .switch input { opacity:0; width:0; height:0; }
    .slider { position:absolute; cursor:pointer; inset:0; background:#9aa7b7; transition:.18s; border-radius:999px; }
    .slider:before { position:absolute; content:""; height:20px; width:20px; left:3px; top:3px; background:white; transition:.18s; border-radius:50%; }
    .switch input:checked + .slider { background:#2a9d8f; }
    .switch input:checked + .slider:before { transform:translateX(20px); }
    .page { display:none; }
    .page.active { display:grid; gap:12px; }
    .nowrap { white-space:nowrap; }
    #flash { position:fixed; right:10px; bottom:10px; max-width:90vw; padding:10px 12px; border-radius:10px; border:1px solid #b9d8c3; background:#e7f5eb; color:#1b5e34; display:none; z-index:999; font-size:13px; }
    #flash.err { background:#fdebec; border-color:#efb8c1; color:#8b1a2b; }
    @media (min-width: 900px) {
      .header { grid-template-columns: 1fr auto; align-items:center; }
      .grid { grid-template-columns: 1fr 1fr; }
      .row2 { grid-template-columns: 1.2fr 1fr; }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="card header">
      <div>
        <h1>Alarm System</h1>
        <div class="sub" id="meta">Loading...</div>
      </div>
      <div class="inline">
        <span class="chip" id="chipSaved">Saved mode: -</span>
        <span class="chip" id="chipAll">Listen-all clients: -</span>
        <span class="chip" id="chipBark">Bark: -</span>
        <span class="chip" id="chipSms">SMS: -</span>
        <button class="btn ghost small" id="logoutBtn">Logout</button>
      </div>
      <div class="nav">
        <button class="navBtn active" id="nav-dashboard" onclick="showPage('dashboard')">Dashboard</button>
        <button class="navBtn" id="nav-groups" onclick="showPage('groups')">Groups</button>
        <button class="navBtn" id="nav-sensors" onclick="showPage('sensors')">Sensors</button>
        <button class="navBtn" id="nav-remotes" onclick="showPage('remotes')">Remotes</button>
        <button class="navBtn" id="nav-wol" onclick="showPage('wol')">Wake on LAN</button>
        <button class="navBtn" id="nav-system" onclick="showPage('system')">System</button>
        <button class="navBtn" id="nav-learning" onclick="showPage('learning')">Sensor Learning</button>
      </div>
    </div>

    <div id="page-dashboard" class="page active">
      <div class="grid">
        <div class="card stack">
          <h2>Main Modes</h2>
          <div class="inline">
            <span class="nowrap">Listen All (this session)</span>
            <label class="switch"><input id="toggleListenAll" type="checkbox"><span class="slider"></span></label>
          </div>
          <div class="inline">
            <span class="nowrap">Saved Mode Armed</span>
            <label class="switch"><input id="toggleSavedMode" type="checkbox"><span class="slider"></span></label>
          </div>
          <div class="inline">
            <span class="nowrap">Bark Detector</span>
            <label class="switch"><input id="toggleBark" type="checkbox"><span class="slider"></span></label>
          </div>
        </div>

        <div class="card stack">
          <h2>Quick State</h2>
          <div class="mutedTiny">Values come from current ESP32 state and refresh automatically.</div>
          <div class="inline">
            <span class="chip" id="chipSensors">Sensors: 0</span>
            <span class="chip" id="chipGroups">Armed groups: 0</span>
            <span class="chip" id="chipRemotes">Remotes: 0</span>
          </div>
        </div>
      </div>

      <div class="row2">
        <div class="card stack">
          <h2>System Status</h2>
          <pre id="statusText">Loading...</pre>
        </div>
        <div class="card stack">
          <h2>Live Event Log</h2>
          <pre id="logs"></pre>
        </div>
      </div>
    </div>

    <div id="page-groups" class="page">
      <div class="card stack">
        <h2>Group Management</h2>
        <div class="sub">Configured groups are listed below with prefilled fields and armed toggles.</div>
        <div id="groupsList" class="list"><div class="mutedTiny">Loading groups...</div></div>
      </div>
      <div class="card stack">
        <h2>New/Direct Group Edit</h2>
        <div class="cols">
          <input id="groupIdNew" type="number" min="0" max="99" placeholder="Group ID (0-99)">
          <input id="groupNameNew" placeholder="Group Name">
        </div>
        <div class="inline">
          <span>Armed</span>
          <label class="switch"><input id="groupArmedNew" type="checkbox"><span class="slider"></span></label>
          <button class="btn" onclick="saveGroupNew()">Save Group</button>
          <button class="btn warn" onclick="resetGroupNew()">Reset Group</button>
        </div>
      </div>
    </div>

    <div id="page-sensors" class="page">
      <div class="card stack">
        <h2>Sensor Management</h2>
        <div class="sub">All sensors are editable in-place. Notify is a toggle.</div>
        <div class="tableWrap">
          <table>
            <thead>
              <tr>
                <th>Code</th>
                <th>Name</th>
                <th>Group</th>
                <th>Notify</th>
                <th>Save</th>
                <th>Remove</th>
              </tr>
            </thead>
            <tbody id="sensorsRows">
              <tr><td colspan="6">Loading sensors...</td></tr>
            </tbody>
          </table>
        </div>
      </div>
      <div class="card stack">
        <h2>Add Sensor</h2>
        <div class="cols">
          <input id="sensorAddCode" type="number" placeholder="Sensor Code">
          <input id="sensorAddGroup" type="number" min="0" max="99" placeholder="Group ID">
        </div>
        <input id="sensorAddName" placeholder="Sensor Name (optional)">
        <div class="inline">
          <span>Notify</span>
          <label class="switch"><input id="sensorAddNotify" type="checkbox"><span class="slider"></span></label>
          <button class="btn" onclick="addSensorNew()">Add Sensor</button>
        </div>
      </div>
    </div>

    <div id="page-remotes" class="page">
      <div class="card stack">
        <h2>Remote Management</h2>
        <div class="sub">Edit remote code/bits in-place, send test, or remove.</div>
        <div class="tableWrap">
          <table>
            <thead>
              <tr>
                <th>Key</th>
                <th>Code</th>
                <th>Bits</th>
                <th>Save</th>
                <th>Send</th>
                <th>Remove</th>
              </tr>
            </thead>
            <tbody id="remotesRows">
              <tr><td colspan="6">Loading remotes...</td></tr>
            </tbody>
          </table>
        </div>
      </div>
      <div class="card stack">
        <h2>Add Remote</h2>
        <div class="cols">
          <input id="remoteAddKey" placeholder="Remote key">
          <input id="remoteAddCode" type="number" placeholder="Remote code">
        </div>
        <div class="cols">
          <input id="remoteAddBits" type="number" min="1" max="32" value="24" placeholder="Bits">
          <button class="btn" onclick="addRemoteNew()">Add Remote</button>
        </div>
      </div>
      <div class="card stack">
        <h2>Remote Learning</h2>
        <div class="sub">Press RF433 remote buttons near the receiver. Detected candidates can be saved directly.</div>
        <div id="remoteLearnList" class="list"><div class="mutedTiny">No RF remote candidates detected yet.</div></div>
      </div>
    </div>

    <div id="page-wol" class="page">
      <div class="card stack">
        <h2>Wake on LAN</h2>
        <input id="wolMac" placeholder="MAC address (e.g. 00:E0:7B:68:04:94)">
        <div class="cols">
          <input id="wolBroadcast" placeholder="Broadcast IP (optional)" value="255.255.255.255">
          <input id="wolPort" type="number" min="1" max="65535" value="9" placeholder="Port">
        </div>
        <button class="btn" onclick="wolSend()">Send Wake Signal</button>
      </div>
    </div>

    <div id="page-system" class="page">
      <div class="card stack">
        <h2>Push Server</h2>
        <div class="sub">Equivalent to Telegram /server_set, /server_clear, /server_show.</div>
        <div class="cols">
          <input id="pushHost" placeholder="Host or domain (example.com or 192.168.1.50)">
          <input id="pushPort" type="number" min="1" max="65535" value="8080" placeholder="Port">
        </div>
        <div class="inline">
          <span>HTTPS</span>
          <label class="switch"><input id="pushHttps" type="checkbox"><span class="slider"></span></label>
          <span>Enabled</span>
          <label class="switch"><input id="pushEnabled" type="checkbox"><span class="slider"></span></label>
          <button class="btn" onclick="savePushServer()">Save Server</button>
          <button class="btn warn" onclick="clearPushServer()">Clear Server</button>
        </div>
      </div>

      <div class="card stack">
        <h2>Router SMS (Direct)</h2>
        <div class="sub">Sends SMS directly via selected router/firmware profile when alarm modes are active (up to 5 recipients).</div>
        <div class="cols">
          <select id="smsRouterId"></select>
          <select id="smsFirmwareId"></select>
        </div>
        <div class="cols">
          <input id="smsHost" placeholder="Router IP or host (e.g. 192.168.0.1)">
          <div class="inline">
            <input id="smsPhoneAdd" placeholder="Add phone (e.g. +12025550123)">
            <button class="btn secondary small" onclick="addSmsPhone()">Add</button>
          </div>
        </div>
        <div id="smsPhonesList" class="list"><div class="mutedTiny">No SMS recipients added.</div></div>
        <input id="smsPassword" type="password" placeholder="Router admin password">
        <div class="inline">
          <span>Enabled</span>
          <label class="switch"><input id="smsEnabled" type="checkbox"><span class="slider"></span></label>
          <span>Saved mode</span>
          <label class="switch"><input id="smsSavedMode" type="checkbox"><span class="slider"></span></label>
          <span>Group mode</span>
          <label class="switch"><input id="smsGroupMode" type="checkbox"><span class="slider"></span></label>
          <span>Sensor mode</span>
          <label class="switch"><input id="smsSensorMode" type="checkbox"><span class="slider"></span></label>
        </div>
        <div class="cols">
          <input id="smsTestMessage" placeholder="Test message (optional)" value="Alarm system SMS test">
          <div class="inline">
            <button class="btn" onclick="saveSmsSettings()">Save SMS Settings</button>
            <button class="btn secondary" onclick="sendSmsTest()">Send SMS Test</button>
          </div>
        </div>
      </div>

      <div class="card stack">
        <h2>Bark Advanced</h2>
        <div class="sub">Equivalent to /bark_mode, /bark_do_level, /bark_threshold, /bark_cooldown, /bark_code, /bark_emit_rf_*, /bark_test.</div>
        <div class="cols">
          <select id="barkModeAdv">
            <option value="do">DO</option>
            <option value="ao">AO</option>
            <option value="both">BOTH</option>
          </select>
          <select id="barkDoLevelAdv">
            <option value="0">DO active level 0</option>
            <option value="1">DO active level 1</option>
          </select>
        </div>
        <div class="cols">
          <input id="barkThresholdAdv" type="number" min="0" max="4095" placeholder="Threshold">
          <input id="barkCooldownAdv" type="number" min="500" max="120000" placeholder="Cooldown ms">
        </div>
        <div class="cols">
          <input id="barkCodeAdv" type="number" min="1" max="16777215" placeholder="Virtual sensor code">
          <div class="inline">
            <span>Emit RF</span>
            <label class="switch"><input id="barkEmitRfAdv" type="checkbox"><span class="slider"></span></label>
          </div>
        </div>
        <div class="inline">
          <button class="btn" onclick="saveBarkAdvanced()">Save Bark Settings</button>
          <button class="btn secondary" onclick="runBarkTest()">Run Bark Test</button>
        </div>
      </div>

      <div class="card stack">
        <h2>Config Export / Import</h2>
        <div class="sub">Equivalent to /config_export and /config_import.</div>
        <div class="inline">
          <button class="btn secondary" onclick="exportConfigToBox()">Export To Box</button>
          <button class="btn secondary" onclick="downloadConfigExport()">Download Export</button>
          <button class="btn" onclick="importConfigFromBox()">Import From Box</button>
        </div>
        <textarea id="configBlob" placeholder="Configuration JSON..."></textarea>
        <div class="mutedTiny">Import can reset web runtime sessions. If needed, sign in again after import.</div>
      </div>

      <div class="card stack">
        <h2>Danger Zone</h2>
        <div class="sub">Equivalent to /reset_all. This removes all persistent data.</div>
        <button class="btn warn" onclick="resetAllData()">Reset All Data</button>
      </div>
    </div>

    <div id="page-learning" class="page">
      <div class="card stack">
        <h2>Sensor Learning</h2>
        <div class="sub">Trigger real sensors. New/unknown codes appear below and can be added with one click.</div>
        <div class="inline">
          <span>Listen All (required)</span>
          <label class="switch"><input id="toggleLearningListen" type="checkbox"><span class="slider"></span></label>
        </div>
      </div>
      <div class="card stack">
        <h2>Detected Codes</h2>
        <div id="learnList" class="list"><div class="mutedTiny">No detected sensor codes yet.</div></div>
      </div>
      <div class="card stack">
        <h2>Recent Raw Log</h2>
        <pre id="learnRaw"></pre>
      </div>
    </div>
  </div>

  <div id="flash"></div>

  <script>
    let logSeq = 0;
    let overview = null;
    let smsPhones = [];
    const learnCandidates = {};
    const remoteLearnCandidates = {};
    let logsPollInFlight = false;
    let overviewLoadInFlight = false;
    let statusPollInFlight = false;

    const byId = (id) => document.getElementById(id);
    const statusEl = byId('statusText');
    const logsEl = byId('logs');
    const learnRawEl = byId('learnRaw');
    const groupsEl = byId('groupsList');
    const sensorsRowsEl = byId('sensorsRows');
    const remotesRowsEl = byId('remotesRows');
    const learnListEl = byId('learnList');
    const remoteLearnListEl = byId('remoteLearnList');
    const smsPhonesListEl = byId('smsPhonesList');
    const metaEl = byId('meta');
    const chipSavedEl = byId('chipSaved');
    const chipAllEl = byId('chipAll');
    const chipBarkEl = byId('chipBark');
    const chipSmsEl = byId('chipSms');
    const chipSensorsEl = byId('chipSensors');
    const chipGroupsEl = byId('chipGroups');
    const chipRemotesEl = byId('chipRemotes');
    const flashEl = byId('flash');
    const toggleListenAllEl = byId('toggleListenAll');
    const toggleSavedModeEl = byId('toggleSavedMode');
    const toggleBarkEl = byId('toggleBark');
    const toggleLearningListenEl = byId('toggleLearningListen');

    const appendLog = (line) => {
      const atBottom = logsEl.scrollTop + logsEl.clientHeight >= logsEl.scrollHeight - 8;
      logsEl.textContent += line + '\n';
      const lines = logsEl.textContent.split('\n');
      if (lines.length > 160) logsEl.textContent = lines.slice(lines.length - 160).join('\n');
      if (atBottom) logsEl.scrollTop = logsEl.scrollHeight;
    };

    const appendLearnRaw = (line) => {
      const atBottom = learnRawEl.scrollTop + learnRawEl.clientHeight >= learnRawEl.scrollHeight - 8;
      learnRawEl.textContent += line + '\n';
      const lines = learnRawEl.textContent.split('\n');
      if (lines.length > 160) learnRawEl.textContent = lines.slice(lines.length - 160).join('\n');
      if (atBottom) learnRawEl.scrollTop = learnRawEl.scrollHeight;
    };

    const esc = (v) => {
      return String(v || '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    };

    const flash = (msg, isErr = false) => {
      flashEl.textContent = msg;
      flashEl.className = isErr ? 'err' : '';
      flashEl.style.display = 'block';
      setTimeout(() => { flashEl.style.display = 'none'; }, 2600);
    };

    const setChip = (el, text, cls = '') => {
      el.textContent = text;
      el.className = 'chip ' + cls;
    };

    const showPage = (name) => {
      const pages = ['dashboard', 'groups', 'sensors', 'remotes', 'wol', 'system', 'learning'];
      pages.forEach((p) => {
        byId('page-' + p).classList.toggle('active', p === name);
        byId('nav-' + p).classList.toggle('active', p === name);
      });
    };

    const api = async (path, options = {}) => {
      const res = await fetch(path, options);
      if (res.status === 401) {
        location.href = '/login';
        throw new Error('Unauthorized');
      }
      const data = await res.json().catch(() => ({}));
      if (!res.ok || data.ok === false) {
        throw new Error(data.error || 'Request failed');
      }
      return data;
    };

    const postAction = async (action, fields = {}, options = {}) => {
      const silent = !!options.silent;
      const body = new URLSearchParams();
      body.set('action', action);
      Object.keys(fields).forEach((k) => {
        const v = fields[k];
        if (v !== undefined && v !== null) body.set(k, String(v));
      });
      const data = await api('/api/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body.toString()
      });
      if (data.reply && !silent) flash(data.reply, false);
      if (data.overview) renderOverview(data.overview);
      return data;
    };

    const renderGroups = (groupsAll) => {
      const groups = (groupsAll || []).filter((g) => g.has_data);
      if (!groups.length) {
        groupsEl.innerHTML = '<div class="mutedTiny">No configured groups yet.</div>';
        return;
      }
      groupsEl.innerHTML = groups.map((g) => `
        <div class="item">
          <div class="itemHead">
            <div>
              <div class="title">Group ${g.id}</div>
              <div class="mutedTiny">sensors=${g.sensor_count} notify=${g.notify_sensor_count}</div>
            </div>
            <div class="inline">
              <span class="mutedTiny">Armed</span>
              <label class="switch">
                <input id="group-arm-${g.id}" type="checkbox" ${g.armed ? 'checked' : ''} onchange="saveGroup(${g.id})">
                <span class="slider"></span>
              </label>
            </div>
          </div>
          <div class="cols">
            <input id="group-name-${g.id}" value="${esc(g.custom_name || '')}" placeholder="Group name">
            <div class="inline">
              <button class="btn small" onclick="saveGroup(${g.id})">Save</button>
              <button class="btn warn small" onclick="resetGroup(${g.id})">Reset</button>
            </div>
          </div>
        </div>
      `).join('');
    };

    const renderSensors = (sensors) => {
      if (!sensors || !sensors.length) {
        sensorsRowsEl.innerHTML = '<tr><td colspan="6">No sensors saved.</td></tr>';
        return;
      }
      sensorsRowsEl.innerHTML = sensors.map((s) => `
        <tr>
          <td>${s.code}</td>
          <td><input id="sensor-name-${s.code}" value="${esc(s.name)}"></td>
          <td><input id="sensor-group-${s.code}" type="number" min="0" max="99" value="${s.group}"></td>
          <td>
            <label class="switch">
              <input id="sensor-notify-${s.code}" type="checkbox" ${s.notify_enabled ? 'checked' : ''}>
              <span class="slider"></span>
            </label>
          </td>
          <td><button class="btn small" onclick="saveSensor(${s.code})">Save</button></td>
          <td><button class="btn warn small" onclick="removeSensor(${s.code})">Remove</button></td>
        </tr>
      `).join('');
    };

    const renderRemotes = (remotes) => {
      if (!remotes || !remotes.length) {
        remotesRowsEl.innerHTML = '<tr><td colspan="6">No remotes saved.</td></tr>';
        return;
      }
      remotesRowsEl.innerHTML = remotes.map((r) => `
        <tr>
          <td>${esc(r.key)}</td>
          <td><input id="remote-code-${esc(r.key)}" type="number" value="${r.code}"></td>
          <td><input id="remote-bits-${esc(r.key)}" type="number" min="1" max="32" value="${r.bits}"></td>
          <td><button class="btn small" onclick="saveRemote('${esc(r.key)}')">Save</button></td>
          <td><button class="btn secondary small" onclick="sendRemote('${esc(r.key)}')">Send</button></td>
          <td><button class="btn warn small" onclick="removeRemote('${esc(r.key)}')">Remove</button></td>
        </tr>
      `).join('');
    };

    const renderSmsPhones = () => {
      if (!smsPhones.length) {
        smsPhonesListEl.innerHTML = '<div class="mutedTiny">No SMS recipients added.</div>';
        return;
      }
      smsPhonesListEl.innerHTML = smsPhones.map((p, idx) => `
        <div class="item">
          <div class="itemHead">
            <div>
              <div class="title">${idx + 1}. ${esc(p)}</div>
              <div class="mutedTiny">recipient</div>
            </div>
            <div class="inline">
              <button class="btn warn small" onclick="removeSmsPhone('${esc(p)}')">Remove</button>
            </div>
          </div>
        </div>
      `).join('');
    };

    const renderStatusSummary = (ov) => {
      metaEl.textContent = (ov.wifi_connected ? 'WiFi connected' : 'WiFi disconnected') + ' | IP: ' + (ov.ip || '-') + ' | SSID: ' + (ov.ssid || '-');
      setChip(chipSavedEl, 'Saved mode: ' + (ov.listen_saved_armed ? 'ARMED' : 'DISARMED'), ov.listen_saved_armed ? 'warn' : 'ok');
      setChip(chipAllEl, 'Listen-all clients: ' + (ov.runtime_listen_all_count || 0), (ov.runtime_listen_all_count || 0) > 0 ? 'warn' : 'ok');
      setChip(chipBarkEl, 'Bark: ' + (ov.bark_enabled ? 'ON' : 'OFF'), ov.bark_enabled ? 'warn' : 'ok');
      const smsEnabled = ov.sms ? !!ov.sms.enabled : !!ov.sms_enabled;
      const smsConfigured = ov.sms ? !!ov.sms.configured : (ov.sms_configured !== false);
      setChip(chipSmsEl, 'SMS: ' + (smsEnabled ? 'ON' : 'OFF'), smsEnabled ? (smsConfigured ? 'ok' : 'warn') : 'ok');
      setChip(chipSensorsEl, 'Sensors: ' + (ov.sensor_total || 0));
      setChip(chipGroupsEl, 'Armed groups: ' + (ov.armed_group_total || 0));
      setChip(chipRemotesEl, 'Remotes: ' + (ov.remote_total || 0));
      statusEl.textContent = ov.status_text || '';

      toggleListenAllEl.checked = !!ov.session_listen_all_enabled;
      toggleLearningListenEl.checked = !!ov.session_listen_all_enabled;
      toggleSavedModeEl.checked = !!ov.listen_saved_armed;
      toggleBarkEl.checked = !!ov.bark_enabled;
    };

    const renderOverview = (ov) => {
      overview = ov;
      renderStatusSummary(ov);
      renderGroups(ov.groups_all || []);
      renderSensors(ov.sensors_flat || []);
      renderRemotes(ov.remotes || []);

      const push = ov.push_server || {};
      const bark = ov.bark || {};
      const sms = ov.sms || {};
      byId('pushHost').value = push.host || '';
      byId('pushPort').value = String(push.port || 8080);
      byId('pushHttps').checked = !!push.https;
      byId('pushEnabled').checked = !!push.enabled;

      const routers = sms.compatible_routers || [];
      const firmwares = sms.supported_firmwares || [];
      const routerSel = byId('smsRouterId');
      const fwSel = byId('smsFirmwareId');

      if (routers.length) {
        routerSel.innerHTML = routers.map((r) =>
          '<option value="' + esc(r.id) + '">' + esc(r.name || r.id) + '</option>'
        ).join('');
      } else if (!routerSel.innerHTML.trim()) {
        routerSel.innerHTML = '<option value="tl-mr100">TP-Link TL-MR100</option>';
      }

      if (firmwares.length) {
        fwSel.innerHTML = firmwares.map((f) =>
          '<option value="' + esc(f.id) + '">' + esc(f.name || f.id) + '</option>'
        ).join('');
      } else if (!fwSel.innerHTML.trim()) {
        fwSel.innerHTML = '<option value="mr100-gdpr-v1">GDPR encrypted web API v1</option>';
      }

      routerSel.value = sms.router_id || routerSel.value || 'tl-mr100';
      fwSel.value = sms.firmware_id || fwSel.value || 'mr100-gdpr-v1';
      byId('smsEnabled').checked = !!sms.enabled;
      byId('smsSavedMode').checked = sms.saved_mode_enabled !== false;
      byId('smsGroupMode').checked = sms.group_mode_enabled !== false;
      byId('smsSensorMode').checked = sms.sensor_mode_enabled !== false;
      byId('smsHost').value = sms.router_host || '192.168.0.1';
      smsPhones = Array.isArray(sms.recipient_phones) ? sms.recipient_phones.slice(0, 5) : [];
      if (!smsPhones.length && sms.recipient_phone) smsPhones = [sms.recipient_phone];
      renderSmsPhones();
      if (sms.router_password_set !== true) byId('smsPassword').value = '';

      byId('barkModeAdv').value = bark.mode || 'do';
      byId('barkDoLevelAdv').value = String(bark.do_active_level != null ? bark.do_active_level : 0);
      byId('barkThresholdAdv').value = String(bark.threshold != null ? bark.threshold : 3000);
      byId('barkCooldownAdv').value = String(bark.cooldown_ms != null ? bark.cooldown_ms : 7000);
      byId('barkCodeAdv').value = String(bark.code != null ? bark.code : 7654321);
      byId('barkEmitRfAdv').checked = !!bark.emit_rf;
    };

    const ingestLearnCandidate = (text) => {
      const match = text.match(/code=(\d+)/);
      if (!match) return;
      const code = match[1];
      const known = text.indexOf('sensor=') >= 0;
      if (!learnCandidates[code]) {
        learnCandidates[code] = { code, count: 0, known: false, last: '' };
      }
      learnCandidates[code].count += 1;
      learnCandidates[code].known = learnCandidates[code].known || known;
      learnCandidates[code].last = text;
      renderLearnCandidates();
    };

    const renderLearnCandidates = () => {
      const items = Object.values(learnCandidates).sort((a, b) => b.count - a.count || Number(b.code) - Number(a.code));
      if (!items.length) {
        learnListEl.innerHTML = '<div class="mutedTiny">No detected sensor codes yet.</div>';
        return;
      }
      learnListEl.innerHTML = items.map((c) => `
        <div class="item">
          <div class="itemHead">
            <div>
              <div class="title">Code ${esc(c.code)}</div>
              <div class="mutedTiny">seen=${c.count} ${c.known ? '| already known sensor' : '| new candidate'}</div>
            </div>
            <div class="inline">
              ${c.known ? '<span class="chip ok">Known</span>' : `<button class="btn small" onclick="addLearnSensor(${c.code})">Add Sensor</button>`}
            </div>
          </div>
        </div>
      `).join('');
    };

    const ingestRemoteCandidate = (text) => {
      if (!text || text.indexOf('[rf]') !== 0) return;

      const codeMatch = text.match(/code=(\d+)/);
      if (!codeMatch) return;
      const bitsMatch = text.match(/bits=(\d+)/);
      const protoMatch = text.match(/proto=(\d+)/);
      const pulseMatch = text.match(/pulse=(\d+)us/);

      const code = codeMatch[1];
      const bits = bitsMatch ? Number(bitsMatch[1]) : 24;
      const proto = protoMatch ? Number(protoMatch[1]) : 0;
      const pulse = pulseMatch ? Number(pulseMatch[1]) : 0;
      const key = String(code) + "_" + String(bits);

      if (!remoteLearnCandidates[key]) {
        remoteLearnCandidates[key] = { code, bits, proto, pulse, count: 0, last: '' };
      }
      remoteLearnCandidates[key].count += 1;
      remoteLearnCandidates[key].proto = proto;
      remoteLearnCandidates[key].pulse = pulse;
      remoteLearnCandidates[key].last = text;
      renderRemoteLearnCandidates();
    };

    const renderRemoteLearnCandidates = () => {
      const items = Object.values(remoteLearnCandidates).sort((a, b) => b.count - a.count || Number(b.code) - Number(a.code));
      if (!items.length) {
        remoteLearnListEl.innerHTML = '<div class="mutedTiny">No RF remote candidates detected yet.</div>';
        return;
      }
      remoteLearnListEl.innerHTML = items.map((c) => `
        <div class="item">
          <div class="itemHead">
            <div>
              <div class="title">Code ${esc(c.code)}</div>
              <div class="mutedTiny">bits=${c.bits} proto=${c.proto} pulse=${c.pulse}us seen=${c.count}</div>
            </div>
            <div class="inline">
              <button class="btn secondary small" onclick="prefillLearnRemote(${c.code}, ${c.bits})">Prefill</button>
              <button class="btn small" onclick="addLearnRemote(${c.code}, ${c.bits})">Add as Remote</button>
            </div>
          </div>
        </div>
      `).join('');
    };

    const loadOverview = async () => {
      if (overviewLoadInFlight) return;
      overviewLoadInFlight = true;
      try {
        const data = await api('/api/overview');
        renderOverview(data.overview || {});
      } finally {
        overviewLoadInFlight = false;
      }
    };

    const loadStatusSummary = async () => {
      if (statusPollInFlight) return;
      statusPollInFlight = true;
      try {
        const data = await api('/api/status');
        renderStatusSummary(data.status || {});
      } catch (e) {
      } finally {
        statusPollInFlight = false;
      }
    };

    const pollLogs = async () => {
      if (logsPollInFlight) return;
      logsPollInFlight = true;
      try {
        const data = await api('/api/logs?since=' + logSeq);
        const events = data.events || [];
        for (const ev of events) {
          logSeq = Math.max(logSeq, ev.seq || 0);
          const line = '[' + (ev.seq || 0) + '] ' + (ev.text || '');
          appendLog(line);
          appendLearnRaw(line);
          ingestLearnCandidate(ev.text || '');
          ingestRemoteCandidate(ev.text || '');
        }
        if (data.has_more) setTimeout(pollLogs, 40);
      } catch (e) {
      } finally {
        logsPollInFlight = false;
      }
    };

    const setListenAll = async (enabled) => {
      try { await postAction('listen_all_set', { enabled: enabled ? 1 : 0 }); } catch (e) { flash(e.message, true); }
    };

    const setSavedMode = async (enabled) => {
      try { await postAction('listen_saved_set', { enabled: enabled ? 1 : 0 }); } catch (e) { flash(e.message, true); }
    };

    const setBark = async (enabled) => {
      try { await postAction('bark_set', { enabled: enabled ? 1 : 0 }); } catch (e) { flash(e.message, true); }
    };

    const saveGroup = async (id) => {
      const name = byId('group-name-' + id).value.trim();
      const armed = byId('group-arm-' + id).checked;
      try { await postAction('group_save', { group: id, name, armed: armed ? 1 : 0 }); } catch (e) { flash(e.message, true); }
    };

    const resetGroup = async (id) => {
      if (!confirm('Reset group ' + id + ' and remove all its sensors?')) return;
      try { await postAction('group_reset', { group: id }); } catch (e) { flash(e.message, true); }
    };

    const saveGroupNew = async () => {
      const group = byId('groupIdNew').value.trim();
      const name = byId('groupNameNew').value.trim();
      const armed = byId('groupArmedNew').checked;
      if (!group) return flash('Group ID is required', true);
      try { await postAction('group_save', { group, name, armed: armed ? 1 : 0 }); } catch (e) { flash(e.message, true); }
    };

    const resetGroupNew = async () => {
      const group = byId('groupIdNew').value.trim();
      if (!group) return flash('Group ID is required', true);
      await resetGroup(group);
    };

    const saveSensor = async (code) => {
      const name = byId('sensor-name-' + code).value.trim();
      const group = byId('sensor-group-' + code).value.trim();
      const notify = byId('sensor-notify-' + code).checked ? 'on' : 'off';
      try { await postAction('sensor_upsert', { code, group, name, notify }); } catch (e) { flash(e.message, true); }
    };

    const removeSensor = async (code) => {
      if (!confirm('Remove sensor ' + code + '?')) return;
      try { await postAction('sensor_remove', { code }); } catch (e) { flash(e.message, true); }
    };

    const addSensorNew = async () => {
      const code = byId('sensorAddCode').value.trim();
      const group = byId('sensorAddGroup').value.trim();
      const name = byId('sensorAddName').value.trim();
      const notify = byId('sensorAddNotify').checked ? 'on' : 'off';
      if (!code || !group) return flash('Sensor code and group are required', true);
      try { await postAction('sensor_upsert', { code, group, name, notify }); } catch (e) { flash(e.message, true); }
    };

    const saveRemote = async (key) => {
      const code = byId('remote-code-' + key).value.trim();
      const bits = byId('remote-bits-' + key).value.trim();
      try { await postAction('remote_upsert', { key, code, bits }); } catch (e) { flash(e.message, true); }
    };

    const sendRemote = async (key) => {
      try { await postAction('remote_send', { key }); } catch (e) { flash(e.message, true); }
    };

    const removeRemote = async (key) => {
      if (!confirm('Remove remote ' + key + '?')) return;
      try { await postAction('remote_remove', { key }); } catch (e) { flash(e.message, true); }
    };

    const addRemoteNew = async () => {
      const key = byId('remoteAddKey').value.trim();
      const code = byId('remoteAddCode').value.trim();
      const bits = byId('remoteAddBits').value.trim();
      if (!key || !code) return flash('Remote key and code are required', true);
      try { await postAction('remote_upsert', { key, code, bits }); } catch (e) { flash(e.message, true); }
    };

    const prefillLearnRemote = (code, bits) => {
      const keyEl = byId('remoteAddKey');
      const codeEl = byId('remoteAddCode');
      const bitsEl = byId('remoteAddBits');
      if (!keyEl.value) keyEl.value = 'remote_' + String(code).slice(-4);
      codeEl.value = String(code);
      bitsEl.value = String(bits || 24);
      flash('Remote add form prefilled', false);
    };

    const addLearnRemote = async (code, bits) => {
      const suggestedKey = 'remote_' + String(code).slice(-4);
      const keyInput = prompt('Remote key for code ' + code + ':', suggestedKey);
      if (keyInput === null) return;
      const key = keyInput.trim();
      if (!key) return flash('Remote key is required', true);

      const bitsInput = prompt('Bits for remote ' + key + ':', String(bits || 24));
      if (bitsInput === null) return;
      const bitsText = bitsInput.trim();
      if (!bitsText) return flash('Bits are required', true);

      try {
        await postAction('remote_upsert', { key, code, bits: bitsText });
        flash('Remote learned and saved', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    const wolSend = async () => {
      const mac = byId('wolMac').value.trim();
      const broadcast = byId('wolBroadcast').value.trim();
      const port = byId('wolPort').value.trim();
      if (!mac) return flash('MAC is required', true);
      try { await postAction('wol', { mac, broadcast, port }); } catch (e) { flash(e.message, true); }
    };

    const savePushServer = async () => {
      const host = byId('pushHost').value.trim();
      const port = byId('pushPort').value.trim();
      const https = byId('pushHttps').checked;
      const enabled = byId('pushEnabled').checked;

      if (!enabled) {
        try { await postAction('server_clear'); } catch (e) { flash(e.message, true); }
        return;
      }
      if (!host) return flash('Push host is required when enabled', true);
      try {
        await postAction('server_set', { host, port, https: https ? 1 : 0 });
      } catch (e) {
        flash(e.message, true);
      }
    };

    const clearPushServer = async () => {
      if (!confirm('Clear push server configuration?')) return;
      try { await postAction('server_clear'); } catch (e) { flash(e.message, true); }
    };

    const addSmsPhone = () => {
      const input = byId('smsPhoneAdd');
      const phone = input.value.trim();
      if (!phone) return flash('Phone is required', true);
      if (!/^\+?\d{1,20}$/.test(phone)) return flash('Invalid phone format', true);
      if (smsPhones.includes(phone)) return flash('Phone already added', true);
      if (smsPhones.length >= 5) return flash('Maximum 5 recipients allowed', true);
      smsPhones.push(phone);
      input.value = '';
      renderSmsPhones();
      flash('Phone added', false);
    };

    const removeSmsPhone = (phone) => {
      smsPhones = smsPhones.filter((p) => p !== phone);
      renderSmsPhones();
    };

    const saveSmsSettings = async () => {
      const router_id = byId('smsRouterId').value.trim();
      const firmware_id = byId('smsFirmwareId').value.trim();
      const router_host = byId('smsHost').value.trim();
      const router_password = byId('smsPassword').value;
      const enabled = byId('smsEnabled').checked;
      const saved_mode_enabled = byId('smsSavedMode').checked;
      const group_mode_enabled = byId('smsGroupMode').checked;
      const sensor_mode_enabled = byId('smsSensorMode').checked;

      if (!router_host) return flash('Router host is required', true);
      if (!smsPhones.length) return flash('Add at least one destination phone', true);
      try {
        await postAction('sms_config_set', {
          enabled: enabled ? 1 : 0,
          saved_mode_enabled: saved_mode_enabled ? 1 : 0,
          group_mode_enabled: group_mode_enabled ? 1 : 0,
          sensor_mode_enabled: sensor_mode_enabled ? 1 : 0,
          router_id,
          firmware_id,
          router_host,
          router_password,
          recipient_phones: smsPhones.join(',')
        });
      } catch (e) {
        flash(e.message, true);
      }
    };

    const sendSmsTest = async () => {
      const message = byId('smsTestMessage').value.trim();
      try {
        await postAction('sms_test', { message });
      } catch (e) {
        flash(e.message, true);
      }
    };

    const saveBarkAdvanced = async () => {
      const mode = byId('barkModeAdv').value;
      const level = byId('barkDoLevelAdv').value;
      const threshold = byId('barkThresholdAdv').value.trim();
      const cooldown = byId('barkCooldownAdv').value.trim();
      const code = byId('barkCodeAdv').value.trim();
      const emit = byId('barkEmitRfAdv').checked;

      if (!threshold || !cooldown || !code) return flash('Bark threshold, cooldown and code are required', true);

      try {
        await postAction('bark_mode_set', { mode }, { silent: true });
        await postAction('bark_do_level_set', { level }, { silent: true });
        await postAction('bark_threshold_set', { value: threshold }, { silent: true });
        await postAction('bark_cooldown_set', { value: cooldown }, { silent: true });
        await postAction('bark_code_set', { value: code }, { silent: true });
        await postAction('bark_emit_rf_set', { enabled: emit ? 1 : 0 }, { silent: true });
        await loadOverview();
        flash('Bark settings saved', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    const runBarkTest = async () => {
      try { await postAction('bark_test'); } catch (e) { flash(e.message, true); }
    };

    const fetchConfigExport = async () => {
      const res = await fetch('/api/config_export', { method: 'GET', cache: 'no-store' });
      if (res.status === 401) {
        location.href = '/login';
        throw new Error('Unauthorized');
      }
      const text = await res.text();
      if (!res.ok) {
        let errMsg = text || 'Config export failed';
        try {
          const errObj = JSON.parse(text);
          if (errObj && errObj.error) errMsg = errObj.error;
        } catch (e) {
        }
        throw new Error(errMsg);
      }
      return text;
    };

    const exportConfigToBox = async () => {
      try {
        const payload = await fetchConfigExport();
        byId('configBlob').value = payload;
        flash('Config exported to text box', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    const downloadConfigExport = async () => {
      try {
        const payload = await fetchConfigExport();
        const blob = new Blob([payload], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'alarm_config_export.json';
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        flash('Config export downloaded', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    const importConfigFromBox = async () => {
      const payload = byId('configBlob').value.trim();
      if (!payload) return flash('Paste config JSON first', true);
      if (!confirm('Import config now? This can reset runtime sessions.')) return;

      try {
        const res = await fetch('/api/config_import', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: payload
        });
        if (res.status === 401) {
          location.href = '/login';
          return;
        }
        const data = await res.json().catch(() => ({}));
        if (!res.ok || data.ok === false) throw new Error(data.error || 'Config import failed');
        if (data.overview) renderOverview(data.overview);
        flash(data.reply || 'Config import applied', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    const resetAllData = async () => {
      if (!confirm('Reset all persistent data? This cannot be undone.')) return;
      try { await postAction('reset_all'); } catch (e) { flash(e.message, true); }
    };

    const addLearnSensor = async (code) => {
      const group = prompt('Group for sensor code ' + code + ' (0-99):', '0');
      if (group === null) return;
      const name = prompt('Name for sensor code ' + code + ':', 'Sensor ' + code);
      if (name === null) return;
      try {
        await postAction('sensor_upsert', { code, group, name });
        flash('Sensor added from learning list', false);
      } catch (e) {
        flash(e.message, true);
      }
    };

    byId('logoutBtn').addEventListener('click', async () => {
      try { await api('/api/logout', { method: 'POST' }); } catch (e) {}
      location.href = '/login';
    });

    toggleListenAllEl.addEventListener('change', () => setListenAll(toggleListenAllEl.checked));
    toggleLearningListenEl.addEventListener('change', () => setListenAll(toggleLearningListenEl.checked));
    toggleSavedModeEl.addEventListener('change', () => setSavedMode(toggleSavedModeEl.checked));
    toggleBarkEl.addEventListener('change', () => setBark(toggleBarkEl.checked));

    window.showPage = showPage;
    window.saveGroup = saveGroup;
    window.resetGroup = resetGroup;
    window.saveGroupNew = saveGroupNew;
    window.resetGroupNew = resetGroupNew;
    window.saveSensor = saveSensor;
    window.removeSensor = removeSensor;
    window.addSensorNew = addSensorNew;
    window.saveRemote = saveRemote;
    window.sendRemote = sendRemote;
    window.removeRemote = removeRemote;
    window.addRemoteNew = addRemoteNew;
    window.prefillLearnRemote = prefillLearnRemote;
    window.addLearnRemote = addLearnRemote;
    window.wolSend = wolSend;
    window.savePushServer = savePushServer;
    window.clearPushServer = clearPushServer;
    window.addSmsPhone = addSmsPhone;
    window.removeSmsPhone = removeSmsPhone;
    window.saveSmsSettings = saveSmsSettings;
    window.sendSmsTest = sendSmsTest;
    window.saveBarkAdvanced = saveBarkAdvanced;
    window.runBarkTest = runBarkTest;
    window.exportConfigToBox = exportConfigToBox;
    window.downloadConfigExport = downloadConfigExport;
    window.importConfigFromBox = importConfigFromBox;
    window.resetAllData = resetAllData;
    window.addLearnSensor = addLearnSensor;

    loadOverview().catch((e) => flash('Overview error: ' + e.message, true));
    pollLogs();
    setInterval(() => loadStatusSummary().catch(() => {}), 2500);
    setInterval(() => loadOverview().catch(() => {}), 25000);
    setInterval(pollLogs, 1800);
  </script>
</body>
</html>
)APP_HTML";
}
void sendWebJsonError(int code, const String& message) {
  DynamicJsonDocument doc(512);
  doc["ok"] = false;
  doc["error"] = message;

  size_t expected = measureJson(doc);
  String payload;
  payload.reserve(expected + 8);
  serializeJson(doc, payload);
  provisionServer.sendHeader("Cache-Control", "no-store");
  provisionServer.send(code, "application/json", payload);
}

void sendWebJsonOk(DynamicJsonDocument& doc) {
  size_t expected = measureJson(doc);
  String payload;
  payload.reserve(expected + 8);
  serializeJson(doc, payload);
  provisionServer.sendHeader("Cache-Control", "no-store");
  provisionServer.send(200, "application/json", payload);
}

bool ensureWebApiAuthorized() {
  String token = currentWebSessionToken();
  if (!isValidWebSession(token)) {
    sendWebJsonError(401, "Unauthorized");
    return false;
  }
  return true;
}

bool runWebCommandForSession(const String& sessionToken, const String& command, String& reply) {
  if (!isValidWebSession(sessionToken)) {
    reply = "Unauthorized";
    return false;
  }

  String webChatId = String(WEB_CHAT_PREFIX) + sessionToken;
  webCommandCaptureActive = true;
  webCommandCaptureChatId = webChatId;
  webCommandCaptureReply = "";
  handleCommand(webChatId, command);
  webCommandCaptureActive = false;
  webCommandCaptureChatId = "";

  reply = webCommandCaptureReply;
  if (reply.length() == 0) reply = "OK";
  webCommandCaptureReply = "";

  addWebEvent("[WEB_ACTION] " + command);
  return true;
}

void fillOverviewJson(DynamicJsonDocument& doc) {
  JsonObject ov = doc.createNestedObject("overview");

  size_t sensorCount = 0;
  size_t armedGroupCount = 0;
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].used) sensorCount++;
  }
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    if (groupArmed[g]) armedGroupCount++;
  }

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  String ipText = wifiConnected ? WiFi.localIP().toString() :
    ((wifiProvisioningMode || offlineApMode) ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  String ssidText = (wifiProvisioningMode || offlineApMode) ? wifiProvisionApSsid : String(wifiCreds.ssid);
  ov["wifi_connected"] = wifiConnected;
  ov["ip"] = ipText;
  ov["ssid"] = ssidText;
  ov["listen_saved_armed"] = listenSavedArmed;
  ov["runtime_listen_all_count"] = runtimeListenAllCount;
  ov["authorized_chats"] = authorizedCount;
  ov["sensor_total"] = sensorCount;
  ov["sensor_notify_total"] = countSensorNotifyEnabled();
  ov["remote_total"] = countSavedRemotes();
  ov["armed_group_total"] = armedGroupCount;
  ov["bark_enabled"] = barkConfig.enabled;
  ov["status_text"] = buildStatusText();

  JsonArray groupsAllArr = ov.createNestedArray("groups_all");
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    uint16_t sensorCountInGroup = countSensorsInGroup(g);
    uint16_t notifyCountInGroup = countNotifySensorsInGroup(g);
    bool hasName = groupNames[g][0] != '\0';
    bool hasData = hasName || groupArmed[g] || sensorCountInGroup > 0 || notifyCountInGroup > 0;
    if (!hasData) continue;

    JsonObject groupAll = groupsAllArr.createNestedObject();
    groupAll["id"] = g;
    groupAll["name"] = groupLabel(g);
    groupAll["custom_name"] = groupNames[g];
    groupAll["armed"] = groupArmed[g];
    groupAll["sensor_count"] = sensorCountInGroup;
    groupAll["notify_sensor_count"] = notifyCountInGroup;
    groupAll["has_data"] = hasData;
  }

  JsonArray sensorsFlat = ov.createNestedArray("sensors_flat");
  for (size_t i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].used) continue;
    JsonObject s = sensorsFlat.createNestedObject();
    s["code"] = sensors[i].code;
    s["bits"] = sensors[i].bits;
    s["group"] = sensors[i].group;
    s["group_name"] = groupLabel(sensors[i].group);
    s["name"] = sensorDisplayName(sensors[i]);
    s["notify_enabled"] = sensors[i].notifyEnabled;
  }

  JsonArray remotesArr = ov.createNestedArray("remotes");
  for (size_t i = 0; i < MAX_REMOTES; i++) {
    if (!remotes[i].used) continue;
    JsonObject r = remotesArr.createNestedObject();
    r["key"] = remotes[i].key;
    r["code"] = remotes[i].code;
    r["bits"] = remotes[i].bits;
  }

  JsonObject push = ov.createNestedObject("push_server");
  push["enabled"] = pushServer.enabled;
  push["https"] = pushServer.https;
  push["host"] = pushServer.host;
  push["port"] = pushServer.port;

  String smsCfgReason;
  bool smsReady = isSmsConfigured(smsCfgReason);
  JsonObject sms = ov.createNestedObject("sms");
  sms["enabled"] = smsConfig.enabled;
  sms["saved_mode_enabled"] = smsConfig.savedModeEnabled;
  sms["group_mode_enabled"] = smsConfig.groupModeEnabled;
  sms["sensor_mode_enabled"] = smsConfig.sensorModeEnabled;
  sms["router_id"] = smsConfig.routerId;
  sms["firmware_id"] = smsConfig.firmwareId;
  sms["router_host"] = smsConfig.routerHost;
  sms["router_password_set"] = (String(smsConfig.routerPassword).length() > 0);
  sms["recipient_count"] = smsRecipientCount();
  JsonArray smsPhones = sms.createNestedArray("recipient_phones");
  for (size_t i = 0; i < MAX_SMS_RECIPIENTS; i++) {
    if (smsConfig.recipientPhones[i][0] == '\0') continue;
    smsPhones.add(smsConfig.recipientPhones[i]);
  }
  sms["configured"] = smsReady;
  sms["config_reason"] = smsCfgReason;
  sms["last_result"] = smsLastResult;

  JsonArray routers = sms.createNestedArray("compatible_routers");
  JsonObject routerEntry = routers.createNestedObject();
  routerEntry["id"] = SMS_ROUTER_TL_MR100;
  routerEntry["name"] = "TP-Link TL-MR100";

  JsonArray firmwares = sms.createNestedArray("supported_firmwares");
  JsonObject fwEntry = firmwares.createNestedObject();
  fwEntry["id"] = SMS_FIRMWARE_MR100_GDPR_V1;
  fwEntry["name"] = "GDPR encrypted web API v1";

  JsonObject bark = ov.createNestedObject("bark");
  bark["enabled"] = barkConfig.enabled;
  bark["mode"] = barkModeText(barkConfig.inputMode);
  bark["do_active_level"] = barkConfig.doActiveLevel;
  bark["threshold"] = barkConfig.threshold;
  bark["cooldown_ms"] = barkConfig.cooldownMs;
  bark["code"] = barkConfig.code;
  bark["bits"] = barkConfig.bits;
  bark["emit_rf"] = barkConfig.emitRf;
  bark["last_ao_level"] = barkLastAoLevel;
  bark["last_do_level"] = barkLastDoLevel;
  bark["last_window_peak"] = barkLastWindowPeak;
  bark["last_trigger_noise"] = barkLastTriggerNoiseLevel;
  bark["last_do_active_pct"] = barkLastDoActivePct;
  bark["last_trigger_do_active"] = barkLastTriggerDoActive;
}

void setupWebRoutes() {
  const char* headers[] = {"Cookie"};
  provisionServer.collectHeaders(headers, 1);

  provisionServer.on("/", HTTP_GET, []() {
    if (wifiProvisioningMode) {
      provisionServer.sendHeader("Location", "/wifi_setup");
      provisionServer.send(302, "text/plain", "Redirecting");
      return;
    }
    if (isValidWebSession(currentWebSessionToken())) {
      provisionServer.sendHeader("Location", "/app");
      provisionServer.send(302, "text/plain", "Redirecting");
      return;
    }
    provisionServer.sendHeader("Location", "/login");
    provisionServer.send(302, "text/plain", "Redirecting");
  });

  provisionServer.on("/login", HTTP_GET, []() {
    if (isValidWebSession(currentWebSessionToken())) {
      provisionServer.sendHeader("Location", "/app");
      provisionServer.send(302, "text/plain", "Redirecting");
      return;
    }
    provisionServer.send(200, "text/html", webLoginPageHtml());
  });

  provisionServer.on("/app", HTTP_GET, []() {
    if (!isValidWebSession(currentWebSessionToken())) {
      provisionServer.sendHeader("Location", "/login");
      provisionServer.send(302, "text/plain", "Redirecting");
      return;
    }
    provisionServer.send(200, "text/html", webAppPageHtml());
  });

  provisionServer.on("/api/login", HTTP_POST, []() {
    String code = provisionServer.arg("code");
    code.trim();
    if (code != AUTHENTICATION_CODE) {
      sendWebJsonError(401, "Authentication failed.");
      return;
    }

    String token = createWebSessionToken();
    String cookie = String(WEB_SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly; SameSite=Lax";
    provisionServer.sendHeader("Set-Cookie", cookie);

    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    sendWebJsonOk(doc);
  });

  provisionServer.on("/api/logout", HTTP_POST, []() {
    String token = currentWebSessionToken();
    if (token.length() > 0) removeWebSession(token);

    String cookie = String(WEB_SESSION_COOKIE) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
    provisionServer.sendHeader("Set-Cookie", cookie);

    DynamicJsonDocument doc(128);
    doc["ok"] = true;
    sendWebJsonOk(doc);
  });

  provisionServer.on("/api/overview", HTTP_GET, []() {
    unsigned long t0 = millis();
    if (!ensureWebApiAuthorized()) return;
    DynamicJsonDocument doc(WEB_OVERVIEW_DOC_CAPACITY);
    doc["ok"] = true;
    fillOverviewJson(doc);
    String token = currentWebSessionToken();
    String webChatId = String(WEB_CHAT_PREFIX) + token;
    doc["overview"]["session_listen_all_enabled"] = isRuntimeListenAllEnabledForChat(webChatId);
    sendWebJsonOk(doc);
    unsigned long took = millis() - t0;
    if (took > 80) Serial.printf("[WEB] /api/overview took %lums\n", took);
  });

  provisionServer.on("/api/status", HTTP_GET, []() {
    unsigned long t0 = millis();
    if (!ensureWebApiAuthorized()) return;
    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    JsonObject st = doc.createNestedObject("status");

    size_t sensorCount = 0;
    size_t armedGroupCount = 0;
    for (size_t i = 0; i < MAX_SENSORS; i++) {
      if (sensors[i].used) sensorCount++;
    }
    for (uint8_t g = 0; g < MAX_GROUPS; g++) {
      if (groupArmed[g]) armedGroupCount++;
    }

    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    String ipText = wifiConnected ? WiFi.localIP().toString() :
      ((wifiProvisioningMode || offlineApMode) ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
    String ssidText = (wifiProvisioningMode || offlineApMode) ? wifiProvisionApSsid : String(wifiCreds.ssid);
    st["wifi_connected"] = wifiConnected;
    st["ip"] = ipText;
    st["ssid"] = ssidText;
    st["status_text"] = buildStatusText();
    st["listen_saved_armed"] = listenSavedArmed;
    st["runtime_listen_all_count"] = runtimeListenAllCount;
    st["sensor_total"] = sensorCount;
    st["armed_group_total"] = armedGroupCount;
    st["remote_total"] = countSavedRemotes();
    st["bark_enabled"] = barkConfig.enabled;
    st["sms_enabled"] = smsConfig.enabled;
    st["sms_last_result"] = smsLastResult;
    st["sms_recipient_count"] = smsRecipientCount();
    String smsCfgReason;
    st["sms_configured"] = isSmsConfigured(smsCfgReason);
    st["sms_config_reason"] = smsCfgReason;

    String token = currentWebSessionToken();
    String webChatId = String(WEB_CHAT_PREFIX) + token;
    st["session_listen_all_enabled"] = isRuntimeListenAllEnabledForChat(webChatId);
    sendWebJsonOk(doc);
    unsigned long took = millis() - t0;
    if (took > 50) Serial.printf("[WEB] /api/status took %lums\n", took);
  });

  provisionServer.on("/api/action", HTTP_POST, []() {
    unsigned long t0 = millis();
    if (!ensureWebApiAuthorized()) return;

    String action = lowerCopy(provisionServer.arg("action"));
    action.trim();
    if (action.length() == 0) {
      sendWebJsonError(400, "Missing action.");
      return;
    }

    String token = currentWebSessionToken();
    String cmd = "";
    String cmd2 = "";
    String err = "";

    if (action == "listen_all_set") {
      String enabled = lowerCopy(provisionServer.arg("enabled"));
      enabled.trim();
      bool on = (enabled == "1" || enabled == "true" || enabled == "on");
      cmd = on ? "/listen_all_on" : "/listen_all_off";
    } else if (action == "listen_saved_set") {
      String enabled = lowerCopy(provisionServer.arg("enabled"));
      enabled.trim();
      bool on = (enabled == "1" || enabled == "true" || enabled == "on");
      cmd = on ? "/listen_saved_on" : "/listen_saved_off";
    } else if (action == "bark_set") {
      String enabled = lowerCopy(provisionServer.arg("enabled"));
      enabled.trim();
      bool on = (enabled == "1" || enabled == "true" || enabled == "on");
      cmd = on ? "/bark_on" : "/bark_off";
    } else if (action == "listen_all_on") {
      cmd = "/listen_all_on";
    } else if (action == "listen_all_off") {
      cmd = "/listen_all_off";
    } else if (action == "listen_saved_on") {
      cmd = "/listen_saved_on";
    } else if (action == "listen_saved_off") {
      cmd = "/listen_saved_off";
    } else if (action == "group_save") {
      uint8_t group = 0;
      String groupText = provisionServer.arg("group");
      String name = provisionServer.arg("name");
      String armedText = lowerCopy(provisionServer.arg("armed"));
      name.trim();
      armedText.trim();

      if (!parseGroupId(groupText, group)) {
        err = "Invalid group id.";
      } else {
        if (name.length() > 0) {
          cmd = "/group_set " + String(group) + " " + name;
          bool arm = (armedText == "1" || armedText == "true" || armedText == "on");
          cmd2 = arm ? "/group_arm " + String(group) : "/group_disarm " + String(group);
        } else {
          bool arm = (armedText == "1" || armedText == "true" || armedText == "on");
          cmd = arm ? "/group_arm " + String(group) : "/group_disarm " + String(group);
        }
      }
    } else if (action == "group_set") {
      uint8_t group = 0;
      String groupText = provisionServer.arg("group");
      String name = provisionServer.arg("name");
      name.trim();
      if (!parseGroupId(groupText, group) || name.length() == 0) err = "group_set needs group(0-99) and name.";
      else cmd = "/group_set " + String(group) + " " + name;
    } else if (action == "group_reset") {
      uint8_t group = 0;
      if (!parseGroupId(provisionServer.arg("group"), group)) err = "Invalid group id.";
      else cmd = "/group_reset " + String(group);
    } else if (action == "group_arm") {
      uint8_t group = 0;
      if (!parseGroupId(provisionServer.arg("group"), group)) err = "Invalid group id.";
      else cmd = "/group_arm " + String(group);
    } else if (action == "group_disarm") {
      uint8_t group = 0;
      if (!parseGroupId(provisionServer.arg("group"), group)) err = "Invalid group id.";
      else cmd = "/group_disarm " + String(group);
    } else if (action == "sensor_upsert") {
      uint32_t code = 0;
      uint8_t group = 0;
      String codeText = provisionServer.arg("code");
      String groupText = provisionServer.arg("group");
      String name = provisionServer.arg("name");
      String notify = lowerCopy(provisionServer.arg("notify"));
      name.trim();
      notify.trim();

      if (!parseUInt32(codeText, code) || !parseGroupId(groupText, group)) {
        err = "sensor_upsert needs valid code and group.";
      } else {
        cmd = "/sensor_add " + String(code) + " " + String(group);
        if (name.length() > 0) cmd += " " + name;
        if (notify == "on") cmd2 = "/sensor_notify_on " + String(code);
        else if (notify == "off") cmd2 = "/sensor_notify_off " + String(code);
      }
    } else if (action == "sensor_remove") {
      uint32_t code = 0;
      if (!parseUInt32(provisionServer.arg("code"), code)) err = "Invalid sensor code.";
      else cmd = "/sensor_remove " + String(code);
    } else if (action == "sensor_notify") {
      uint32_t code = 0;
      String enabled = lowerCopy(provisionServer.arg("enabled"));
      enabled.trim();
      if (!parseUInt32(provisionServer.arg("code"), code)) {
        err = "Invalid sensor code.";
      } else if (enabled == "1" || enabled == "true" || enabled == "on") {
        cmd = "/sensor_notify_on " + String(code);
      } else {
        cmd = "/sensor_notify_off " + String(code);
      }
    } else if (action == "remote_upsert") {
      String key = provisionServer.arg("key");
      String codeText = provisionServer.arg("code");
      String bitsText = provisionServer.arg("bits");
      key.trim();
      bitsText.trim();
      uint32_t code = 0;
      uint8_t bits = RF_BITS;
      if (key.length() == 0 || !parseUInt32(codeText, code)) {
        err = "remote_upsert needs key and code.";
      } else if (bitsText.length() > 0 && !parseBits(bitsText, bits)) {
        err = "Invalid bits.";
      } else {
        cmd = "/remote_add " + key + " " + String(code);
        if (bitsText.length() > 0) cmd += " " + String(bits);
      }
    } else if (action == "remote_remove") {
      String key = provisionServer.arg("key");
      key.trim();
      if (key.length() == 0) err = "Missing key.";
      else cmd = "/remote_remove " + key;
    } else if (action == "remote_send") {
      String key = provisionServer.arg("key");
      key.trim();
      if (key.length() == 0) err = "Missing key.";
      else cmd = "/remote_send " + key;
    } else if (action == "bark_on") {
      cmd = "/bark_on";
    } else if (action == "bark_off") {
      cmd = "/bark_off";
    } else if (action == "bark_mode_set") {
      String mode = lowerCopy(provisionServer.arg("mode"));
      mode.trim();
      if (!(mode == "ao" || mode == "do" || mode == "both")) err = "Invalid bark mode.";
      else cmd = "/bark_mode " + mode;
    } else if (action == "bark_do_level_set") {
      String level = provisionServer.arg("level");
      level.trim();
      if (!(level == "0" || level == "1")) err = "Invalid bark do level.";
      else cmd = "/bark_do_level " + level;
    } else if (action == "bark_threshold_set") {
      uint16_t value = 0;
      if (!parseUInt16(provisionServer.arg("value"), value) || value > ADC_MAX_VALUE) err = "Invalid bark threshold.";
      else cmd = "/bark_threshold " + String(value);
    } else if (action == "bark_cooldown_set") {
      uint32_t value = 0;
      if (!parseUInt32(provisionServer.arg("value"), value) || value < 500 || value > 120000) err = "Invalid bark cooldown.";
      else cmd = "/bark_cooldown " + String(value);
    } else if (action == "bark_code_set") {
      uint32_t value = 0;
      if (!parseUInt32(provisionServer.arg("value"), value) || value == 0 || value > 16777215UL) err = "Invalid bark code.";
      else cmd = "/bark_code " + String(value);
    } else if (action == "bark_emit_rf_set") {
      String enabled = lowerCopy(provisionServer.arg("enabled"));
      enabled.trim();
      bool on = (enabled == "1" || enabled == "true" || enabled == "on");
      cmd = on ? "/bark_emit_rf_on" : "/bark_emit_rf_off";
    } else if (action == "bark_test") {
      cmd = "/bark_test";
    } else if (action == "server_set") {
      String host = provisionServer.arg("host");
      String port = provisionServer.arg("port");
      String httpsText = lowerCopy(provisionServer.arg("https"));
      host.trim();
      port.trim();
      httpsText.trim();
      bool https = (httpsText == "1" || httpsText == "true" || httpsText == "on");
      if (host.length() == 0) {
        err = "Missing server host.";
      } else {
        String target = String(https ? "https://" : "http://") + host;
        cmd = "/server_set " + target;
        if (port.length() > 0) {
          uint16_t p = 0;
          if (!parseUInt16(port, p) || p == 0) {
            err = "Invalid server port.";
          } else {
            cmd += " " + String(p);
          }
        }
      }
    } else if (action == "server_clear") {
      cmd = "/server_clear";
    } else if (action == "sms_config_set") {
      String enabledText = lowerCopy(provisionServer.arg("enabled"));
      String savedText = lowerCopy(provisionServer.arg("saved_mode_enabled"));
      String groupText = lowerCopy(provisionServer.arg("group_mode_enabled"));
      String sensorText = lowerCopy(provisionServer.arg("sensor_mode_enabled"));
      String routerId = lowerCopy(provisionServer.arg("router_id"));
      String firmwareId = lowerCopy(provisionServer.arg("firmware_id"));
      String routerHost = provisionServer.arg("router_host");
      String routerPassword = provisionServer.arg("router_password");
      String recipientPhonesCsv = provisionServer.arg("recipient_phones");

      enabledText.trim();
      savedText.trim();
      groupText.trim();
      sensorText.trim();
      routerId.trim();
      firmwareId.trim();
      routerHost.trim();
      recipientPhonesCsv.trim();

      if (!isSmsRouterSupported(routerId)) {
        sendWebJsonError(400, "Unsupported router type.");
        return;
      }
      if (!isSmsFirmwareSupported(firmwareId)) {
        sendWebJsonError(400, "Unsupported firmware type.");
        return;
      }
      if (routerHost.length() == 0 || routerHost.length() >= SMS_ROUTER_HOST_LEN) {
        sendWebJsonError(400, "Invalid router host.");
        return;
      }
      if (routerPassword.length() == 0) routerPassword = String(smsConfig.routerPassword);
      if (routerPassword.length() == 0 || routerPassword.length() >= SMS_ROUTER_PASSWORD_LEN) {
        sendWebJsonError(400, "Invalid router password length.");
        return;
      }
      String parsedPhones[MAX_SMS_RECIPIENTS];
      size_t parsedCount = 0;
      String parseReason;
      if (!parseSmsRecipientsCsv(recipientPhonesCsv, parsedPhones, parsedCount, parseReason)) {
        sendWebJsonError(400, "Invalid recipients: " + parseReason);
        return;
      }

      smsConfig.enabled = (enabledText == "1" || enabledText == "true" || enabledText == "on");
      smsConfig.savedModeEnabled = (savedText == "1" || savedText == "true" || savedText == "on");
      smsConfig.groupModeEnabled = (groupText == "1" || groupText == "true" || groupText == "on");
      smsConfig.sensorModeEnabled = (sensorText == "1" || sensorText == "true" || sensorText == "on");
      copyToBuf(smsConfig.routerId, SMS_ROUTER_ID_LEN, routerId);
      copyToBuf(smsConfig.firmwareId, SMS_FIRMWARE_ID_LEN, firmwareId);
      copyToBuf(smsConfig.routerHost, SMS_ROUTER_HOST_LEN, routerHost);
      copyToBuf(smsConfig.routerPassword, SMS_ROUTER_PASSWORD_LEN, routerPassword);
      clearSmsRecipients();
      for (size_t i = 0; i < parsedCount; i++) {
        String addReason;
        addSmsRecipient(parsedPhones[i], addReason);
      }
      saveState();

      addWebEvent(
        "[WEB_ACTION] sms_config_set enabled=" + String(smsConfig.enabled ? "1" : "0") +
        " host=" + String(smsConfig.routerHost) +
        " recipients=" + String(smsRecipientCount())
      );

      DynamicJsonDocument doc(WEB_OVERVIEW_DOC_CAPACITY);
      doc["ok"] = true;
      doc["reply"] = "SMS settings saved.";
      fillOverviewJson(doc);
      String webChatId = String(WEB_CHAT_PREFIX) + token;
      doc["overview"]["session_listen_all_enabled"] = isRuntimeListenAllEnabledForChat(webChatId);
      sendWebJsonOk(doc);
      unsigned long took = millis() - t0;
      if (took > 100) Serial.printf("[WEB] /api/action took %lums (%s)\n", took, action.c_str());
      return;
    } else if (action == "sms_test") {
      String text = provisionServer.arg("message");
      text.trim();
      if (text.length() == 0) text = "Alarm system SMS test";

      String cfgReason;
      if (!isSmsConfigured(cfgReason)) {
        sendWebJsonError(400, "SMS config incomplete: " + cfgReason);
        return;
      }

      String summary;
      String smsErr;
      bool ok = sendSmsToConfiguredRecipients(text, summary, smsErr);

      if (ok) {
        smsLastResult = summary;
        addWebEvent("[SMS] web test " + summary);
      } else {
        smsLastResult = "error: " + summary + " | " + smsErr;
        addWebEvent("[SMS] web test failed: " + summary + " | " + smsErr);
      }

      DynamicJsonDocument doc(WEB_OVERVIEW_DOC_CAPACITY);
      doc["ok"] = true;
      doc["reply"] = ok ? ("SMS test " + summary + ".") : ("SMS test failed: " + summary + " | " + smsErr);
      fillOverviewJson(doc);
      String webChatId = String(WEB_CHAT_PREFIX) + token;
      doc["overview"]["session_listen_all_enabled"] = isRuntimeListenAllEnabledForChat(webChatId);
      sendWebJsonOk(doc);
      unsigned long took = millis() - t0;
      if (took > 100) Serial.printf("[WEB] /api/action took %lums (%s)\n", took, action.c_str());
      return;
    } else if (action == "reset_all") {
      cmd = "/reset_all";
    } else if (action == "wol") {
      String mac = provisionServer.arg("mac");
      String broadcast = provisionServer.arg("broadcast");
      String port = provisionServer.arg("port");
      mac.trim();
      broadcast.trim();
      port.trim();
      if (mac.length() == 0) {
        err = "Missing MAC.";
      } else {
        cmd = "/wol " + mac;
        if (broadcast.length() > 0) cmd += " " + broadcast;
        if (port.length() > 0) {
          if (broadcast.length() == 0) cmd += " 255.255.255.255";
          cmd += " " + port;
        }
      }
    } else {
      err = "Unknown action.";
    }

    if (err.length() > 0) {
      sendWebJsonError(400, err);
      return;
    }

    String reply = "";
    if (!runWebCommandForSession(token, cmd, reply)) {
      sendWebJsonError(401, "Unauthorized");
      return;
    }

    if (cmd2.length() > 0) {
      String reply2 = "";
      if (!runWebCommandForSession(token, cmd2, reply2)) {
        sendWebJsonError(401, "Unauthorized");
        return;
      }
      if (reply2.length() > 0) reply += "\n" + reply2;
    }

    DynamicJsonDocument doc(WEB_OVERVIEW_DOC_CAPACITY);
    doc["ok"] = true;
    doc["reply"] = reply;
    fillOverviewJson(doc);
    String webChatId = String(WEB_CHAT_PREFIX) + token;
    doc["overview"]["session_listen_all_enabled"] = isRuntimeListenAllEnabledForChat(webChatId);
    sendWebJsonOk(doc);
    unsigned long took = millis() - t0;
    if (took > 100) Serial.printf("[WEB] /api/action took %lums (%s)\n", took, action.c_str());
  });

  provisionServer.on("/api/config_export", HTTP_GET, []() {
    if (!ensureWebApiAuthorized()) return;
    String payload;
    String err;
    if (!buildConfigExportPayload(payload, err)) {
      sendWebJsonError(500, "Config export failed: " + err);
      return;
    }
    provisionServer.sendHeader("Cache-Control", "no-store");
    provisionServer.send(200, "application/json", payload);
  });

  provisionServer.on("/api/config_import", HTTP_POST, []() {
    if (!ensureWebApiAuthorized()) return;

    String payload = "";
    if (provisionServer.hasArg("plain")) payload = provisionServer.arg("plain");
    if (payload.length() == 0) payload = provisionServer.arg("payload");
    payload.trim();

    if (payload.length() == 0) {
      sendWebJsonError(400, "Empty import payload.");
      return;
    }
    if (payload.length() > CONFIG_IMPORT_MAX_LEN) {
      sendWebJsonError(400, "Import payload too large.");
      return;
    }

    String err;
    if (!applyImportedConfig(payload, err)) {
      sendWebJsonError(400, "Config import failed: " + err);
      return;
    }

    DynamicJsonDocument doc(WEB_OVERVIEW_DOC_CAPACITY);
    doc["ok"] = true;
    doc["reply"] = "Config import applied. Please sign in again.";
    fillOverviewJson(doc);
    sendWebJsonOk(doc);
  });

  provisionServer.on("/api/cmd", HTTP_POST, []() {
    if (!ensureWebApiAuthorized()) return;

    String cmd = provisionServer.arg("cmd");
    cmd.trim();
    if (cmd.length() == 0) {
      sendWebJsonError(400, "Missing cmd.");
      return;
    }

    String cmdLower = lowerCopy(cmd);
    if (cmdLower.startsWith("/auth") || cmdLower.startsWith("authenticate ")) {
      sendWebJsonError(400, "Use web login for authentication.");
      return;
    }

    String reply = "";
    if (!runWebCommandForSession(currentWebSessionToken(), cmd, reply)) {
      sendWebJsonError(401, "Unauthorized");
      return;
    }

    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    doc["reply"] = reply;
    sendWebJsonOk(doc);
  });

  provisionServer.on("/api/logs", HTTP_GET, []() {
    unsigned long t0 = millis();
    if (!ensureWebApiAuthorized()) return;

    uint32_t since = 0;
    String sinceArg = provisionServer.arg("since");
    if (sinceArg.length() > 0) parseUInt32(sinceArg, since);

    DynamicJsonDocument doc(12288);
    doc["ok"] = true;
    doc["latest_seq"] = webEventSeq;
    JsonArray arr = doc.createNestedArray("events");
    uint16_t sent = 0;
    bool hasMore = false;

    size_t start = (webEventCount == WEB_EVENT_CAPACITY) ? webEventWritePos : 0;
    for (size_t i = 0; i < webEventCount; i++) {
      size_t idx = (start + i) % WEB_EVENT_CAPACITY;
      const WebEventEntry& item = webEvents[idx];
      if (item.seq <= since) continue;
      if (sent >= WEB_LOGS_MAX_BATCH) {
        hasMore = true;
        break;
      }
      JsonObject e = arr.createNestedObject();
      e["seq"] = item.seq;
      e["ms"] = item.tsMs;
      e["text"] = item.text;
      sent++;
    }
    doc["has_more"] = hasMore;
    sendWebJsonOk(doc);
    unsigned long took = millis() - t0;
    if (took > 50) Serial.printf("[WEB] /api/logs took %lums (events=%u)\n", took, static_cast<unsigned>(sent));
  });

  provisionServer.on("/ping", HTTP_GET, []() {
    provisionServer.send(200, "text/plain", "pong");
  });
}

void ensureWebServerStarted() {
  if (webServerStarted) return;
  provisionServer.begin();
  webServerStarted = true;
  Serial.println("Web interface started.");
}

void processWebServer() {
  tickProvisioningWifiScan();
  if (!webServerStarted) return;
  unsigned long t0 = millis();
  provisionServer.handleClient();
  logSlowOperation("web.handleClient", millis() - t0, 120);
}

void startProvisioningApMode() {
  if (wifiProvisioningMode) return;

  wifiProvisionApSsid = WIFI_AP_SSID;
  wifiScanOptionsCache = "";
  wifiScanCacheMs = 0;
  wifiScanStartedMs = 0;
  wifiScanNextRefreshMs = 0;

  // Stop STA reconnect loops; they can starve scan with ESP_ERR_WIFI_STATE (-2).
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(120);
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(wifiProvisionApSsid.c_str(), WIFI_AP_PASSWORD);
  if (!apOk) {
    Serial.println("Failed to start AP provisioning mode.");
    return;
  }

  provisionServer.on("/wifi_setup", HTTP_GET, []() {
    if (!wifiProvisioningMode && !offlineApMode) {
      provisionServer.sendHeader("Location", "/app");
      provisionServer.send(302, "text/plain", "Redirecting");
      return;
    }
    tickProvisioningWifiScan();
    bool scanInProgress = (WiFi.scanComplete() == WIFI_SCAN_STATE_RUNNING);
    String ssidOptions = wifiScanOptionsCache;
    bool hasResults = ssidOptions.length() > 0;
    provisionServer.send(
      200,
      "text/html",
      webWifiSetupPageHtml(ssidOptions, String(wifiCreds.ssid), hasResults, scanInProgress)
    );
  });

  provisionServer.on("/save", HTTP_POST, []() {
    if (!wifiProvisioningMode && !offlineApMode) {
      provisionServer.send(404, "text/plain", "Not in provisioning mode.");
      return;
    }
    String ssid = provisionServer.arg("ssid");
    String ssidSelect = provisionServer.arg("ssid_select");
    String password = provisionServer.arg("password");
    ssid.trim();
    ssidSelect.trim();

    if (ssid.length() == 0) ssid = ssidSelect;
    if (ssid.length() == 0) {
      provisionServer.send(400, "text/plain", "SSID is required.");
      return;
    }

    if (!saveWiFiCredentialsFile(ssid, password)) {
      provisionServer.send(500, "text/plain", "Failed to save WiFi credentials.");
      return;
    }

    setWifiCreds(ssid, password);
    offlineApMode = false;
    provisionServer.send(200, "text/html",
      "<html><body><h3>Saved. Rebooting...</h3><p>Device will restart now.</p></body></html>");
    delay(900);
    ESP.restart();
  });

  provisionServer.on("/continue_offline", HTTP_POST, []() {
    if (!wifiProvisioningMode && !offlineApMode) {
      provisionServer.send(404, "text/plain", "Not in provisioning mode.");
      return;
    }

    offlineApMode = true;
    wifiProvisioningMode = false;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(wifiProvisionApSsid.c_str(), WIFI_AP_PASSWORD);
    provisionServer.sendHeader("Location", "/app");
    provisionServer.send(303, "text/plain", "Redirecting");
    Serial.println("Offline AP mode enabled by user.");
  });

  provisionServer.onNotFound([]() {
    provisionServer.sendHeader("Location", wifiProvisioningMode ? "/wifi_setup" : "/");
    provisionServer.send(302, "text/plain", "Redirecting");
  });

  ensureWebServerStarted();
  wifiProvisioningMode = true;
  offlineApMode = false;
  tickProvisioningWifiScan();  // kick off background scan immediately
  Serial.println("WiFi failed after 3 attempts. AP provisioning mode enabled.");
  Serial.printf("AP SSID: %s | AP PASS: %s\n", wifiProvisionApSsid.c_str(), WIFI_AP_PASSWORD);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
}

void handleCommand(const String& chatId, String text) {
  String rawText = text;
  text = normalizeSpaces(text);
  if (text.length() == 0) return;

  String firstToken = text;
  String args = "";
  int firstSpace = text.indexOf(' ');
  if (firstSpace >= 0) {
    firstToken = text.substring(0, firstSpace);
    args = text.substring(firstSpace + 1);
    args.trim();
  }
  firstToken.trim();

  String cmd = lowerCopy(firstToken);
  if (cmd.startsWith("/")) {
    int atPos = cmd.indexOf('@');
    if (atPos > 0) cmd = cmd.substring(0, atPos);
  }

  String lower = lowerCopy(text);
  String rawArgs = "";
  int rawFirstSpace = rawText.indexOf(' ');
  if (rawFirstSpace >= 0) {
    rawArgs = rawText.substring(rawFirstSpace + 1);
    rawArgs.trim();
  }

  if (cmd == "/start" || cmd == "/help") {
    sendToChat(chatId, buildHelpText(isAuthorized(chatId)));
    return;
  }

  // Authenticate <code> (supports "/auth <code>" and "authenticate <code>")
  if (cmd == "/auth" || lower.startsWith("authenticate ")) {
    String code = (cmd == "/auth") ? args : argsAfterFirstToken(text);
    if (code.length() == 0) {
      sendToChat(chatId, "Usage: /auth <code>");
      return;
    }
    if (code == AUTHENTICATION_CODE) {
      if (!addAuthorizedChat(chatId)) {
        sendToChat(chatId, "Auth failed: authorized list is full.");
        return;
      }
      saveState();
      sendToChat(chatId, "Authentication successful. Full access granted.");
    } else {
      sendToChat(chatId, "Authentication failed: wrong code.");
    }
    return;
  }

  if (!isAuthorized(chatId)) {
    sendToChat(chatId, "Not authorized. Use /auth <code> first.");
    return;
  }

  cleanupConfigImportSessions();
  int importIdx = findConfigImportSession(chatId);

  if (importIdx >= 0 && configImportSessions[importIdx].active) {
    if (cmd == "/config_import_cancel") {
      cancelConfigImportSession(chatId);
      sendToChat(chatId, "Config import canceled.");
      return;
    }

    if (cmd == "/config_import_end") {
      String payload = configImportSessions[importIdx].payload;
      payload.trim();
      if (payload.length() == 0) {
        sendToChat(chatId, "Import buffer is empty. Send JSON chunks first.");
        return;
      }

      String importErr;
      if (!applyImportedConfig(payload, importErr)) {
        sendToChat(chatId, "Config import failed: " + importErr);
        return;
      }

      cancelConfigImportSession(chatId);
      sendToChat(chatId, "Config import applied successfully.");
      return;
    }

    if (cmd == "/config_import_begin") {
      startConfigImportSession(chatId);
      sendToChat(chatId, "Import buffer reset. Send JSON chunks, then /config_import_end.");
      return;
    }

    if (cmd.startsWith("/")) {
      sendToChat(
        chatId,
        "Config import mode is active. Send JSON text chunks, or use /config_import_end or /config_import_cancel."
      );
      return;
    }

    String appendErr;
    if (!appendConfigImportChunk(chatId, rawText, appendErr)) {
      sendToChat(chatId, "Failed to append import chunk: " + appendErr);
      return;
    }

    int idxUpdated = findConfigImportSession(chatId);
    size_t bytes = (idxUpdated >= 0) ? configImportSessions[idxUpdated].payload.length() : 0;
    sendToChat(chatId, "Chunk received. Buffered bytes: " + String(bytes));
    return;
  }

  if (cmd == "/config_import_begin") {
    int idx = startConfigImportSession(chatId);
    if (idx < 0) {
      sendToChat(chatId, "Cannot start import session: no free session slots.");
      return;
    }
    sendToChat(chatId, "Config import started. Send JSON chunks, then /config_import_end.");
    return;
  }

  if (cmd == "/config_import_cancel") {
    sendToChat(chatId, "No active config import session.");
    return;
  }

  if (cmd == "/config_import_end") {
    sendToChat(chatId, "No active config import session. Use /config_import_begin first.");
    return;
  }

  if (cmd == "/config_import") {
    String payload = rawArgs;
    payload.trim();
    if (payload.length() == 0) {
      sendToChat(chatId, "Usage: /config_import <json> OR use /config_import_begin ... /config_import_end");
      return;
    }

    String importErr;
    if (!applyImportedConfig(payload, importErr)) {
      sendToChat(chatId, "Config import failed: " + importErr);
      return;
    }
    sendToChat(chatId, "Config import applied successfully.");
    return;
  }

  if (cmd == "/config_export") {
    String payload;
    String exportErr;
    if (!buildConfigExportPayload(payload, exportErr)) {
      sendToChat(chatId, "Config export failed: " + exportErr);
      return;
    }
    sendChunkedMessage(chatId, "CONFIG_EXPORT", payload);
    return;
  }

  // 1) Listen to all sensors (runtime only)
  if (
    cmd == "/listen_all_on" ||
    cmd == "/listen_all" ||
    lower == "listen to all sensors"
  ) {
    if (!addRuntimeListenAll(chatId)) {
      sendToChat(chatId, "Cannot enable listen-all: runtime listener list is full.");
      return;
    }
    sendToChat(chatId, "Listen-all enabled (runtime only, not persisted).");
    pushModeEvent("listen_all", true, chatId);
    return;
  }

  // 2) Stop listen all sensors (runtime only)
  if (
    cmd == "/listen_all_off" ||
    lower == "stop listen to all sensors"
  ) {
    removeRuntimeListenAll(chatId);
    sendToChat(chatId, "Listen-all disabled for this chat.");
    pushModeEvent("listen_all", false, chatId);
    return;
  }

  // 3) Listen to saved only sensors (persistent + panel ARM)
  if (
    cmd == "/listen_saved_on" ||
    cmd == "/arm_saved" ||
    lower == "listen to saved only sensors"
  ) {
    listenSavedArmed = true;
    if (!saveState()) {
      sendToChat(chatId, "Saved mode set, but failed to persist state.");
    }
    sendPanelSignal(true);
    sendToChat(chatId, "Saved-sensors mode armed. Panel ARM code sent.");
    pushModeEvent("listen_saved", true, chatId);
    return;
  }

  // 4) Stop listen saved sensors (persistent + panel DISARM)
  if (
    cmd == "/listen_saved_off" ||
    cmd == "/disarm_saved" ||
    lower == "stop listen to saved sensors"
  ) {
    listenSavedArmed = false;
    if (!saveState()) {
      sendToChat(chatId, "Saved mode cleared, but failed to persist state.");
    }
    sendPanelSignal(false);
    sendToChat(chatId, "Saved-sensors mode disarmed. Panel DISARM code sent.");
    pushModeEvent("listen_saved", false, chatId);
    return;
  }

  if (cmd == "/status") {
    sendToChat(chatId, buildStatusText());
    return;
  }

  if (cmd == "/wol") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /wol <mac> [broadcast_ip] [port]");
      return;
    }

    String macText = args;
    String broadcastText = "";
    String portText = "";

    int sp1 = args.indexOf(' ');
    if (sp1 >= 0) {
      macText = args.substring(0, sp1);
      String rest = args.substring(sp1 + 1);
      rest.trim();

      int sp2 = rest.indexOf(' ');
      if (sp2 < 0) {
        String maybePort = rest;
        maybePort.trim();
        uint16_t tmpPort = 0;
        if (parseUInt16(maybePort, tmpPort)) {
          portText = maybePort;
        } else {
          broadcastText = maybePort;
        }
      } else {
        broadcastText = rest.substring(0, sp2);
        portText = rest.substring(sp2 + 1);
        portText.trim();
      }
    }

    uint8_t mac[6];
    if (!parseMacAddress(macText, mac)) {
      sendToChat(chatId, "Invalid MAC. Example: /wol 00:E0:7B:68:04:94");
      return;
    }

    IPAddress broadcastIp(255, 255, 255, 255);
    if (broadcastText.length() > 0 && !broadcastIp.fromString(broadcastText.c_str())) {
      sendToChat(chatId, "Invalid broadcast IP. Example: 255.255.255.255");
      return;
    }

    uint16_t port = 9;
    if (portText.length() > 0) {
      if (!parseUInt16(portText, port) || port == 0) {
        sendToChat(chatId, "Invalid port. Allowed range is 1..65535.");
        return;
      }
    }

    String wolErr;
    if (!sendWakeOnLan(mac, broadcastIp, port, wolErr)) {
      sendToChat(chatId, "WOL failed: " + wolErr);
      return;
    }

    sendToChat(
      chatId,
      "WOL sent to " + macToString(mac) +
      " via " + broadcastIp.toString() + ":" + String(port)
    );
    return;
  }

  if (cmd == "/sensors") {
    sendToChat(chatId, buildSensorsList());
    return;
  }

  if (cmd == "/sensor_notify_list") {
    sendToChat(chatId, buildSensorNotifyList());
    return;
  }

  if (cmd == "/remotes") {
    sendToChat(chatId, buildRemotesList());
    return;
  }

  if (cmd == "/bark_status") {
    sendToChat(chatId, buildBarkStatusText());
    return;
  }

  if (cmd == "/groups") {
    sendToChat(chatId, buildGroupsList());
    return;
  }

  if (cmd == "/server_show") {
    String msg = "Push server: ";
    if (pushServer.enabled && pushServer.host[0] != '\0') {
      msg += pushBaseUrl();
      msg += "\nRoute: ";
      msg += PUSH_EVENT_ROUTE;
    } else {
      msg += "disabled";
    }
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/server_clear") {
    pushServer = PushServerConfig();
    saveState();
    sendToChat(chatId, "Push server cleared.");
    return;
  }

  if (cmd == "/server_set") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /server_set <host_or_url> [port]");
      return;
    }

    int sp = args.indexOf(' ');
    String target = (sp < 0) ? args : args.substring(0, sp);
    String portText = (sp < 0) ? "" : args.substring(sp + 1);
    portText.trim();

    bool https = false;
    String host = "";
    uint16_t port = 8080;
    bool portInTarget = false;
    if (!parseServerTarget(target, https, host, port, portInTarget)) {
      sendToChat(chatId, "Usage: /server_set <host_or_url> [port]");
      return;
    }

    if (portText.length() > 0) {
      uint16_t manualPort = 0;
      if (!parseUInt16(portText, manualPort)) {
        sendToChat(chatId, "Invalid port.");
        return;
      }
      port = manualPort;
    } else if (!portInTarget) {
      port = 8080;
    }

    pushServer.enabled = true;
    pushServer.https = https;
    pushServer.port = port;
    copyToBuf(pushServer.host, PUSH_HOST_LEN, host);
    saveState();

    String msg = "Push server set to ";
    msg += pushBaseUrl();
    msg += "\nRoute: ";
    msg += PUSH_EVENT_ROUTE;
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/sms_status") {
    String cfgReason;
    bool ready = isSmsConfigured(cfgReason);
    String msg = "SMS direct status:\n";
    msg += "enabled: ";
    msg += smsConfig.enabled ? "yes" : "no";
    msg += "\nrouter: ";
    msg += String(smsConfig.routerId);
    msg += " (";
    msg += smsRouterLabel(String(smsConfig.routerId));
    msg += ")";
    msg += "\nfirmware: ";
    msg += String(smsConfig.firmwareId);
    msg += " (";
    msg += smsFirmwareLabel(String(smsConfig.firmwareId));
    msg += ")";
    msg += "\nrouter host: " + String(smsConfig.routerHost);
    msg += "\nrecipients (" + String(smsRecipientCount()) + "/" + String(MAX_SMS_RECIPIENTS) + "):\n";
    msg += smsRecipientsListText();
    msg += "\nrouter password set: ";
    msg += (String(smsConfig.routerPassword).length() > 0) ? "yes" : "no";
    msg += "\nmodes: saved=";
    msg += smsConfig.savedModeEnabled ? "on" : "off";
    msg += " group=";
    msg += smsConfig.groupModeEnabled ? "on" : "off";
    msg += " sensor=";
    msg += smsConfig.sensorModeEnabled ? "on" : "off";
    msg += "\nconfig ready: ";
    msg += ready ? "yes" : ("no (" + cfgReason + ")");
    msg += "\nlast result: " + smsLastResult;
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/sms_router_list") {
    sendToChat(
      chatId,
      "Compatible routers:\n"
      "1) tl-mr100 (TP-Link TL-MR100)"
    );
    return;
  }

  if (cmd == "/sms_fw_list") {
    sendToChat(
      chatId,
      "Supported firmwares:\n"
      "1) mr100-gdpr-v1 (GDPR encrypted web API v1)"
    );
    return;
  }

  if (cmd == "/sms_router_set") {
    String routerId = lowerCopy(args);
    routerId.trim();
    if (routerId.length() == 0 || !isSmsRouterSupported(routerId)) {
      sendToChat(chatId, "Usage: /sms_router_set <id>\nAvailable: tl-mr100");
      return;
    }
    copyToBuf(smsConfig.routerId, SMS_ROUTER_ID_LEN, routerId);
    saveState();
    sendToChat(chatId, "SMS router set: " + routerId);
    return;
  }

  if (cmd == "/sms_fw_set") {
    String fw = lowerCopy(args);
    fw.trim();
    if (fw.length() == 0 || !isSmsFirmwareSupported(fw)) {
      sendToChat(chatId, "Usage: /sms_fw_set <id>\nAvailable: mr100-gdpr-v1");
      return;
    }
    copyToBuf(smsConfig.firmwareId, SMS_FIRMWARE_ID_LEN, fw);
    saveState();
    sendToChat(chatId, "SMS firmware set: " + fw);
    return;
  }

  if (cmd == "/sms_ip") {
    String host = args;
    host.trim();
    if (host.length() == 0 || host.length() >= SMS_ROUTER_HOST_LEN) {
      sendToChat(chatId, "Usage: /sms_ip <router_ip_or_host>");
      return;
    }
    copyToBuf(smsConfig.routerHost, SMS_ROUTER_HOST_LEN, host);
    saveState();
    sendToChat(chatId, "SMS router host set: " + host);
    return;
  }

  if (cmd == "/sms_password") {
    String password = rawArgs;
    password.trim();
    if (password.length() == 0 || password.length() >= SMS_ROUTER_PASSWORD_LEN) {
      sendToChat(chatId, "Usage: /sms_password <router_password>");
      return;
    }
    copyToBuf(smsConfig.routerPassword, SMS_ROUTER_PASSWORD_LEN, password);
    saveState();
    sendToChat(chatId, "SMS router password updated.");
    return;
  }

  if (cmd == "/sms_to_list") {
    String msg = "SMS recipients (" + String(smsRecipientCount()) + "/" + String(MAX_SMS_RECIPIENTS) + "):\n";
    msg += smsRecipientsListText();
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/sms_to_clear") {
    clearSmsRecipients();
    saveState();
    sendToChat(chatId, "All SMS recipients cleared.");
    return;
  }

  if (cmd == "/sms_to_remove") {
    String phone = args;
    phone.trim();
    if (phone.length() == 0) {
      sendToChat(chatId, "Usage: /sms_to_remove <phone_number>");
      return;
    }
    if (!removeSmsRecipient(phone)) {
      sendToChat(chatId, "Phone not found in recipients list.");
      return;
    }
    saveState();
    sendToChat(chatId, "SMS recipient removed: " + phone);
    return;
  }

  if (cmd == "/sms_to" || cmd == "/sms_to_add") {
    String phone = args;
    phone.trim();
    if (!isPhoneNumberValid(phone) || phone.length() >= SMS_PHONE_LEN) {
      sendToChat(chatId, "Usage: /sms_to_add <phone_number>\nExample: /sms_to_add +12025550123");
      return;
    }
    String addReason;
    if (!addSmsRecipient(phone, addReason)) {
      sendToChat(chatId, "Cannot add SMS recipient: " + addReason);
      return;
    }
    if (addReason == "already exists") {
      sendToChat(chatId, "SMS recipient already exists: " + phone);
      return;
    }
    saveState();
    sendToChat(
      chatId,
      "SMS recipient added: " + phone +
      "\nTotal: " + String(smsRecipientCount()) + "/" + String(MAX_SMS_RECIPIENTS)
    );
    return;
  }

  if (cmd == "/sms_on" || cmd == "/sms_off") {
    smsConfig.enabled = (cmd == "/sms_on");
    saveState();
    sendToChat(chatId, String("SMS direct is now ") + (smsConfig.enabled ? "enabled." : "disabled."));
    return;
  }

  if (
    cmd == "/sms_mode_saved_on" ||
    cmd == "/sms_mode_saved_off" ||
    cmd == "/sms_mode_group_on" ||
    cmd == "/sms_mode_group_off" ||
    cmd == "/sms_mode_sensor_on" ||
    cmd == "/sms_mode_sensor_off"
  ) {
    if (cmd == "/sms_mode_saved_on") smsConfig.savedModeEnabled = true;
    else if (cmd == "/sms_mode_saved_off") smsConfig.savedModeEnabled = false;
    else if (cmd == "/sms_mode_group_on") smsConfig.groupModeEnabled = true;
    else if (cmd == "/sms_mode_group_off") smsConfig.groupModeEnabled = false;
    else if (cmd == "/sms_mode_sensor_on") smsConfig.sensorModeEnabled = true;
    else smsConfig.sensorModeEnabled = false;
    saveState();
    sendToChat(
      chatId,
      "SMS mode toggles updated: saved=" + String(smsConfig.savedModeEnabled ? "on" : "off") +
      " group=" + String(smsConfig.groupModeEnabled ? "on" : "off") +
      " sensor=" + String(smsConfig.sensorModeEnabled ? "on" : "off")
    );
    return;
  }

  if (cmd == "/sms_test") {
    String text = rawArgs;
    text.trim();
    if (text.length() == 0) text = "Alarm system SMS test";

    String cfgReason;
    if (!isSmsConfigured(cfgReason)) {
      sendToChat(chatId, "SMS configuration is incomplete: " + cfgReason);
      return;
    }

    sendToChat(chatId, "Sending SMS test...");
    String summary;
    String smsErr;
    bool ok = sendSmsToConfiguredRecipients(text, summary, smsErr);
    if (ok) {
      smsLastResult = summary;
      sendToChat(chatId, "SMS test " + summary + ".");
    } else {
      smsLastResult = "error: " + summary + " | " + smsErr;
      sendToChat(chatId, "SMS test failed: " + summary + " | " + smsErr);
    }
    return;
  }

  if (cmd == "/sensor_add") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /sensor_add <code> <group 0-99> [name]");
      return;
    }

    int sp1 = args.indexOf(' ');
    if (sp1 < 0) {
      sendToChat(chatId, "Usage: /sensor_add <code> <group 0-99> [name]");
      return;
    }

    String codeText = args.substring(0, sp1);
    String rest = args.substring(sp1 + 1);
    rest.trim();

    int sp2 = rest.indexOf(' ');
    String groupText = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String name = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
    name.trim();

    uint32_t code = 0;
    uint8_t groupId = 0;
    if (!parseUInt32(codeText, code) || !parseGroupId(groupText, groupId)) {
      sendToChat(chatId, "Invalid code/group. Usage: /sensor_add <code> <group 0-99> [name]");
      return;
    }

    int idx = findSensorByCode(code);
    if (idx < 0) {
      idx = firstFreeSensorSlot();
      if (idx < 0) {
        sendToChat(chatId, "Sensor list is full.");
        return;
      }
      sensors[idx].used = true;
      sensors[idx].code = code;
      sensors[idx].bits = RF_BITS;
    }

    sensors[idx].group = groupId;
    if (name.length() > 0) {
      copyToBuf(sensors[idx].name, SENSOR_NAME_LEN, name);
    } else if (sensors[idx].name[0] == '\0') {
      String defaultName = String("Sensor ") + String(code);
      copyToBuf(sensors[idx].name, SENSOR_NAME_LEN, defaultName);
    }

    saveState();
    String msg = "Sensor saved: ";
    msg += String(code);
    msg += " -> group ";
    msg += String(groupId);
    msg += " (" + groupLabel(groupId) + ")";
    msg += " name: " + sensorDisplayName(sensors[idx]);
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/sensor_remove") {
    String codeText = args;
    codeText.trim();
    uint32_t code = 0;
    if (!parseUInt32(codeText, code)) {
      sendToChat(chatId, "Usage: /sensor_remove <code>");
      return;
    }
    int idx = findSensorByCode(code);
    if (idx < 0) {
      sendToChat(chatId, "Sensor not found.");
      return;
    }
    sensors[idx] = SensorEntry();
    saveState();
    sendToChat(chatId, "Sensor removed.");
    return;
  }

  if (cmd == "/sensor_name") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /sensor_name <code> <name>");
      return;
    }

    int sp = args.indexOf(' ');
    if (sp < 0) {
      sendToChat(chatId, "Usage: /sensor_name <code> <name>");
      return;
    }
    String codeText = args.substring(0, sp);
    String name = args.substring(sp + 1);
    name.trim();
    if (name.length() == 0) {
      sendToChat(chatId, "Name cannot be empty.");
      return;
    }

    uint32_t code = 0;
    if (!parseUInt32(codeText, code)) {
      sendToChat(chatId, "Invalid sensor code.");
      return;
    }
    int idx = findSensorByCode(code);
    if (idx < 0) {
      sendToChat(chatId, "Sensor not found.");
      return;
    }
    copyToBuf(sensors[idx].name, SENSOR_NAME_LEN, name);
    saveState();
    sendToChat(chatId, "Sensor name updated.");
    return;
  }

  if (cmd == "/sensor_notify_on" || cmd == "/sensor_notify_off") {
    String codeText = args;
    codeText.trim();
    uint32_t code = 0;
    if (!parseUInt32(codeText, code)) {
      sendToChat(chatId, String("Usage: ") + cmd + " <code>");
      return;
    }
    int idx = findSensorByCode(code);
    if (idx < 0) {
      sendToChat(chatId, "Sensor not found.");
      return;
    }
    sensors[idx].notifyEnabled = (cmd == "/sensor_notify_on");
    saveState();
    sendToChat(
      chatId,
      String("Sensor notify ") + (sensors[idx].notifyEnabled ? "enabled: " : "disabled: ") +
      sensorDisplayName(sensors[idx]) + " [" + String(sensors[idx].code) + "]"
    );
    return;
  }

  if (cmd == "/bark_on") {
    barkConfig.enabled = true;
    saveState();
    sendToChat(chatId, "Bark detector enabled.");
    return;
  }

  if (cmd == "/bark_off") {
    barkConfig.enabled = false;
    saveState();
    sendToChat(chatId, "Bark detector disabled.");
    return;
  }

  if (cmd == "/bark_mode") {
    String mode = lowerCopy(args);
    mode.trim();
    if (mode == "ao") barkConfig.inputMode = BARK_MODE_AO;
    else if (mode == "do") barkConfig.inputMode = BARK_MODE_DO;
    else if (mode == "both") barkConfig.inputMode = BARK_MODE_BOTH;
    else {
      sendToChat(chatId, "Usage: /bark_mode <ao|do|both>");
      return;
    }
    saveState();
    sendToChat(chatId, "Bark mode set to " + barkModeText(barkConfig.inputMode));
    return;
  }

  if (cmd == "/bark_do_level") {
    uint32_t level = 0;
    if (!parseUInt32(args, level) || level > 1) {
      sendToChat(chatId, "Usage: /bark_do_level <0|1>");
      return;
    }
    barkConfig.doActiveLevel = static_cast<uint8_t>(level);
    saveState();
    sendToChat(chatId, "Bark DO active level set to " + String(barkConfig.doActiveLevel));
    return;
  }

  if (cmd == "/bark_threshold") {
    uint32_t threshold = 0;
    if (!parseUInt32(args, threshold) || threshold > ADC_MAX_VALUE) {
      sendToChat(chatId, "Usage: /bark_threshold <0-4095>");
      return;
    }
    barkConfig.threshold = static_cast<uint16_t>(threshold);
    saveState();
    sendToChat(chatId, "Bark threshold set to " + String(barkConfig.threshold));
    return;
  }

  if (cmd == "/bark_cooldown") {
    uint32_t cooldown = 0;
    if (!parseUInt32(args, cooldown) || cooldown < 500 || cooldown > 120000) {
      sendToChat(chatId, "Usage: /bark_cooldown <ms> (500..120000)");
      return;
    }
    barkConfig.cooldownMs = cooldown;
    saveState();
    sendToChat(chatId, "Bark cooldown set to " + String(barkConfig.cooldownMs) + " ms");
    return;
  }

  if (cmd == "/bark_code") {
    uint32_t code = 0;
    if (!parseUInt32(args, code) || code == 0 || code > 16777215UL) {
      sendToChat(chatId, "Usage: /bark_code <code> (1..16777215)");
      return;
    }
    barkConfig.code = code;
    saveState();
    String msg = "Bark virtual sensor code set to " + String(barkConfig.code);
    msg += "\nTip: add this code as a sensor and enable notify/group if you want alarm messages outside listen-all.";
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/bark_emit_rf_on" || cmd == "/bark_emit_rf_off") {
    barkConfig.emitRf = (cmd == "/bark_emit_rf_on");
    saveState();
    sendToChat(chatId, String("Bark RF transmit is now ") + (barkConfig.emitRf ? "enabled" : "disabled"));
    return;
  }

  if (cmd == "/bark_test") {
    uint16_t testNoise = barkLastTriggerNoiseLevel;
    if (testNoise == 0) testNoise = barkLastWindowPeak;
    if (testNoise == 0) testNoise = barkLastAoLevel;
    if (testNoise == 0) testNoise = barkLastDoActivePct;
    if (testNoise == 0) testNoise = barkConfig.threshold;
    handleSensorCodeEvent(
      barkConfig.code,
      barkConfig.bits,
      1,
      testNoise,
      false,
      "bark_test"
    );
    sendToChat(chatId, "Bark test event generated (noise=" + String(testNoise) + ").");
    return;
  }

  if (cmd == "/remote_add") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /remote_add <key> <code> [bits]");
      return;
    }

    int sp1 = args.indexOf(' ');
    if (sp1 < 0) {
      sendToChat(chatId, "Usage: /remote_add <key> <code> [bits]");
      return;
    }

    String rawKey = args.substring(0, sp1);
    String rest = args.substring(sp1 + 1);
    rest.trim();

    int sp2 = rest.indexOf(' ');
    String codeText = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String bitsText = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
    bitsText.trim();

    String key = sanitizeRemoteKey(rawKey);
    if (key.length() == 0) {
      sendToChat(chatId, "Invalid key. Use letters, digits, _ or - only.");
      return;
    }

    uint32_t code = 0;
    if (!parseUInt32(codeText, code)) {
      sendToChat(chatId, "Invalid code. Usage: /remote_add <key> <code> [bits]");
      return;
    }

    uint8_t bits = RF_BITS;
    if (bitsText.length() > 0 && !parseBits(bitsText, bits)) {
      sendToChat(chatId, "Invalid bits. Allowed range is 1..32.");
      return;
    }

    int idx = findRemoteByKey(key);
    if (idx < 0) {
      idx = firstFreeRemoteSlot();
      if (idx < 0) {
        sendToChat(chatId, "Remote list is full.");
        return;
      }
      remotes[idx].used = true;
      copyToBuf(remotes[idx].key, REMOTE_KEY_LEN, key);
    }

    remotes[idx].code = code;
    remotes[idx].bits = bits;
    saveState();

    String msg = "Remote saved: ";
    msg += key;
    msg += " -> code=";
    msg += String(code);
    msg += " bits=";
    msg += String(bits);
    if (key == PANEL_ARM_REMOTE_KEY || key == PANEL_DISARM_REMOTE_KEY) {
      msg += "\nThis key is used by saved-mode ARM/DISARM panel signals.";
    }
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/remote_remove") {
    String key = sanitizeRemoteKey(args);
    if (key.length() == 0) {
      sendToChat(chatId, "Usage: /remote_remove <key>");
      return;
    }
    int idx = findRemoteByKey(key);
    if (idx < 0) {
      sendToChat(chatId, "Remote not found.");
      return;
    }
    remotes[idx] = RemoteEntry();
    saveState();
    sendToChat(chatId, "Remote removed: " + key);
    return;
  }

  if (cmd == "/remote_send") {
    String key = sanitizeRemoteKey(args);
    if (key.length() == 0) {
      sendToChat(chatId, "Usage: /remote_send <key>");
      return;
    }
    int idx = findRemoteByKey(key);
    if (idx < 0) {
      sendToChat(chatId, "Remote not found.");
      return;
    }
    sendRfSignal(remotes[idx].code, remotes[idx].bits);
    sendToChat(chatId,
      "Remote sent: " + key +
      " (code=" + String(remotes[idx].code) +
      ", bits=" + String(remotes[idx].bits) + ")");
    return;
  }

  if (cmd == "/group_set") {
    if (args.length() == 0) {
      sendToChat(chatId, "Usage: /group_set <group 0-99> <name>");
      return;
    }

    int sp = args.indexOf(' ');
    if (sp < 0) {
      sendToChat(chatId, "Usage: /group_set <group 0-99> <name>");
      return;
    }
    String groupText = args.substring(0, sp);
    String name = args.substring(sp + 1);
    name.trim();

    uint8_t groupId = 0;
    if (!parseGroupId(groupText, groupId) || name.length() == 0) {
      sendToChat(chatId, "Usage: /group_set <group 0-99> <name>");
      return;
    }

    copyToBuf(groupNames[groupId], GROUP_NAME_LEN, name);
    saveState();
    sendToChat(chatId, "Group updated: " + String(groupId) + " -> " + groupLabel(groupId));
    return;
  }

  if (cmd == "/group_reset") {
    String groupText = args;
    groupText.trim();
    uint8_t groupId = 0;
    if (!parseGroupId(groupText, groupId)) {
      sendToChat(chatId, "Usage: /group_reset <group 0-99>");
      return;
    }

    uint16_t removed = removeSensorsInGroup(groupId);
    groupNames[groupId][0] = '\0';
    groupArmed[groupId] = false;
    saveState();

    String msg = "Group ";
    msg += String(groupId);
    msg += " reset. Sensors removed: ";
    msg += String(removed);
    sendToChat(chatId, msg);
    return;
  }

  if (cmd == "/group_arm") {
    String groupText = args;
    groupText.trim();
    uint8_t groupId = 0;
    if (!parseGroupId(groupText, groupId)) {
      sendToChat(chatId, "Usage: /group_arm <group 0-99>");
      return;
    }

    groupArmed[groupId] = true;
    saveState();
    sendToChat(chatId, "Group armed: " + String(groupId) + " (" + groupLabel(groupId) + ")");
    pushModeEvent("group", true, chatId, groupId);
    return;
  }

  if (cmd == "/group_disarm") {
    String groupText = args;
    groupText.trim();
    uint8_t groupId = 0;
    if (!parseGroupId(groupText, groupId)) {
      sendToChat(chatId, "Usage: /group_disarm <group 0-99>");
      return;
    }

    groupArmed[groupId] = false;
    saveState();
    sendToChat(chatId, "Group disarmed: " + String(groupId) + " (" + groupLabel(groupId) + ")");
    pushModeEvent("group", false, chatId, groupId);
    return;
  }

  if (cmd == "/reset_all" || lower == "reset all data") {
    bool wasSavedArmed = listenSavedArmed;

    // Send notification before dropping auth list.
    sendToChat(chatId, "Resetting all persistent data now...");
    pushResetEvent(chatId);

    clearPersistentState();
    clearRuntimeState();
    saveState();

    if (wasSavedArmed) sendPanelSignal(false);

    sendToChat(chatId, "All data reset. Re-authenticate with /auth <code>.");
    return;
  }

  sendToChat(chatId, "Unknown command. Use /help");
}

void handleTelegramMessages(int count) {
  for (int i = 0; i < count; i++) {
    String chatId = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    handleCommand(chatId, text);
  }
}

void pollTelegram() {
  if (telegramPollActive) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - botLastPollMs < BOT_POLL_INTERVAL_MS) return;
  FlagScope pollScope(telegramPollActive);
  BusyScope busy("telegram_poll");
  unsigned long pollStartMs = millis();
  botLastPollMs = millis();

  unsigned long getStartMs = millis();
  int newMessages = bot.getUpdates(bot.last_message_received + 1);
  logSlowOperation("tg getUpdates", millis() - getStartMs, 500);
  if (newMessages < 0) {
    static unsigned long lastErrLogMs = 0;
    unsigned long now = millis();
    if (now - lastErrLogMs > 5000) {
      lastErrLogMs = now;
      Serial.printf("[TG] getUpdates error: %d\n", newMessages);
    }
    return;
  }

  uint8_t safetyLoops = 0;
  int handled = 0;
  while (newMessages > 0 && safetyLoops < 3) {
    handled += newMessages;
    handleTelegramMessages(newMessages);
    processWebServer();  // keep web UI responsive while draining bot backlog
    yield();
    getStartMs = millis();
    newMessages = bot.getUpdates(bot.last_message_received + 1);
    logSlowOperation("tg getUpdates", millis() - getStartMs, 500);
    if (newMessages < 0) break;
    safetyLoops++;
  }
  unsigned long pollTookMs = millis() - pollStartMs;
  if (pollTookMs >= PERF_SLOW_OP_LOG_MS) {
    logSlowOperation("tg poll loop handled=" + String(handled), pollTookMs, PERF_SLOW_OP_LOG_MS);
  }
}

void handleSensorCodeEvent(
  uint32_t code,
  uint8_t bits,
  uint8_t protocol,
  uint16_t pulseUs,
  bool applyDuplicateFilter,
  const String& source
) {
  if (code == 0 || bits != RF_BITS) return;

  if (applyDuplicateFilter) {
    unsigned long now = millis();
    if (code == lastRfCode && (now - lastRfCodeMs) < RF_DUPLICATE_WINDOW_MS) return;
    lastRfCode = code;
    lastRfCodeMs = now;
  }

  SensorEntry* sensor = nullptr;
  int idx = findSensorByCode(code);
  if (idx >= 0) sensor = &sensors[idx];

  bool known = (sensor != nullptr);
  bool throughSavedMode = known && listenSavedArmed;
  bool throughGroupMode = known && groupArmed[sensor->group];
  bool throughSensorMode = known && sensor->notifyEnabled;

  String base = "code=" + String(code);
  base += " bits=" + String(bits);
  base += " proto=" + String(protocol);
  if (isBarkSource(source)) {
    base += " noise=" + String(pulseUs);
    base += " doPct=" + String(barkLastDoActivePct) + "%";
    base += " mode=" + barkModeText(barkConfig.inputMode);
    base += " doLevel=" + String(barkConfig.doActiveLevel);
  } else {
    base += " pulse=" + String(pulseUs) + "us";
  }

  if (known) {
    base += " sensor=" + sensorDisplayName(*sensor);
    base += " group=" + String(sensor->group);
    base += "(" + groupLabel(sensor->group) + ")";
  }

  addWebEvent("[" + source + "] " + base);

  if (runtimeListenAllCount > 0) {
    sendToRuntimeListenAll("[ALL] " + base);
  }

  if (throughSavedMode || throughGroupMode || throughSensorMode) {
    String mode = "";
    if (throughSavedMode) mode += "saved+";
    if (throughGroupMode) mode += "group+";
    if (throughSensorMode) mode += "sensor+";
    if (mode.endsWith("+")) mode.remove(mode.length() - 1);
    String outMsg = "[ALARM " + mode + "] " + base;
    sendToAuthorizedChats(outMsg);
    if (isBarkSource(source)) {
      Serial.printf(
        "[BARK] alarm message sent mode=%s noise=%u ao=%u do=%d doPct=%u chats=%u\n",
        mode.c_str(),
        static_cast<unsigned>(pulseUs),
        static_cast<unsigned>(barkLastAoLevel),
        static_cast<int>(barkLastDoLevel),
        static_cast<unsigned>(barkLastDoActivePct),
        static_cast<unsigned>(authorizedCount)
      );
    }
  } else if (isBarkSource(source)) {
    if (!known) {
      Serial.printf(
        "[BARK] trigger captured but no alarm message: bark code %lu is not saved as a sensor.\n",
        static_cast<unsigned long>(code)
      );
    } else {
      Serial.printf(
        "[BARK] trigger captured but no alarm message: saved=%u group=%u sensor=%u listenAll=%u\n",
        throughSavedMode ? 1U : 0U,
        throughGroupMode ? 1U : 0U,
        throughSensorMode ? 1U : 0U,
        runtimeListenAllCount > 0 ? 1U : 0U
      );
    }
  }

  maybeSendSmsAlert(code, throughSavedMode, throughGroupMode, throughSensorMode, source, base);

  pushSensorEvent(
    code,
    bits,
    protocol,
    pulseUs,
    known,
    throughSavedMode,
    throughGroupMode,
    throughSensorMode,
    source,
    sensor
  );
}

void processBarkDetection() {
  if (!barkConfig.enabled) return;

  unsigned long now = millis();
  if (now - barkLastSampleMs < BARK_SAMPLE_INTERVAL_MS) return;  // 100 Hz sampling
  barkLastSampleMs = now;

  bool aoSamplingActive = (barkConfig.inputMode != BARK_MODE_DO);
  uint16_t aoLevel = 0;
  if (aoSamplingActive) {
    aoLevel = static_cast<uint16_t>(analogRead(BARK_MIC_PIN));
    barkLastAoLevel = aoLevel;
    if (aoLevel > barkWindowPeak) barkWindowPeak = aoLevel;
  } else {
    // In DO-only mode AO on this pin is not meaningful; keep AO diagnostics neutral.
    barkLastAoLevel = 0;
    barkWindowPeak = 0;
  }
  int doLevel = digitalRead(BARK_MIC_PIN);
  barkLastDoLevel = doLevel;
  barkWindowSampleCount++;

  if (doLevel == static_cast<int>(barkConfig.doActiveLevel)) {
    barkWindowDoTriggered = true;
    barkWindowDoActiveSamples++;
  }

  if (barkWindowStartMs == 0) barkWindowStartMs = now;
  if (now - barkWindowStartMs < BARK_WINDOW_MS) return;

  bool aoTrigger = aoSamplingActive && (barkWindowPeak >= barkConfig.threshold);
  bool doTrigger = barkWindowDoTriggered;
  bool trigger = false;
  if (barkConfig.inputMode == BARK_MODE_AO) trigger = aoTrigger;
  else if (barkConfig.inputMode == BARK_MODE_DO) trigger = doTrigger;
  else trigger = aoTrigger || doTrigger;

  barkWindowStartMs = now;
  uint16_t peak = barkWindowPeak;
  uint16_t sampleCount = (barkWindowSampleCount == 0) ? 1 : barkWindowSampleCount;
  uint8_t doActivePct = static_cast<uint8_t>((static_cast<uint32_t>(barkWindowDoActiveSamples) * 100UL) / sampleCount);
  barkLastDoActivePct = doActivePct;
  barkLastWindowPeak = peak;
  barkWindowPeak = 0;
  barkWindowDoTriggered = false;
  barkWindowSampleCount = 0;
  barkWindowDoActiveSamples = 0;

  if (!trigger) return;
  if (now - barkLastTriggerMs < barkConfig.cooldownMs) return;
  barkLastTriggerMs = now;
  uint16_t noiseLevel = peak;
  if (barkConfig.inputMode == BARK_MODE_DO) noiseLevel = doActivePct;
  else if (barkConfig.inputMode == BARK_MODE_BOTH && !aoTrigger && doTrigger) noiseLevel = doActivePct;
  barkLastTriggerNoiseLevel = noiseLevel;
  barkLastTriggerDoActive = doTrigger;

  Serial.printf(
    "[BARK] trigger mode=%s noise=%u doPct=%u aoTrig=%u doTrig=%u do=%d thr=%u\n",
    barkModeText(barkConfig.inputMode).c_str(),
    static_cast<unsigned>(noiseLevel),
    static_cast<unsigned>(doActivePct),
    aoTrigger ? 1U : 0U,
    doTrigger ? 1U : 0U,
    doLevel,
    static_cast<unsigned>(barkConfig.threshold)
  );
  addWebEvent(
    "[BARK] trigger noise=" + String(noiseLevel) +
    " doPct=" + String(doActivePct) + "%" +
    " mode=" + barkModeText(barkConfig.inputMode) +
    " do=" + String(doLevel)
  );

  if (barkConfig.emitRf) {
    sendRfSignal(barkConfig.code, barkConfig.bits, 4);
  }

  // Generate a synthetic Kerui-like event so downstream logic/message format stays identical.
  handleSensorCodeEvent(
    barkConfig.code,
    barkConfig.bits,
    1,
    noiseLevel,
    false,
    "bark"
  );
}

void processRf() {
  if (!rf.available()) return;

  uint32_t code = rf.getReceivedValue();
  uint8_t bits = rf.getReceivedBitlength();
  uint8_t protocol = rf.getReceivedProtocol();
  uint16_t pulseUs = rf.getReceivedDelay();
  rf.resetAvailable();
  handleSensorCodeEvent(code, bits, protocol, pulseUs, true, "rf");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32 Alarm System Bot ===");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  if (!loadState()) {
    Serial.println("State load failed, using defaults");
    clearPersistentState();
  }
  clearRuntimeState();  // runtime-only listeners never persist

  setupWebRoutes();

  initWiFiCredentials();
  if (!connectWiFiWithRetries(WIFI_MAX_CONNECT_ATTEMPTS)) {
    startProvisioningApMode();
  } else {
    ensureWebServerStarted();
    Serial.print("Open web UI: http://");
    Serial.println(WiFi.localIP());
  }
  tgClient.setInsecure();
  tgClient.setTimeout(TELEGRAM_CLIENT_TIMEOUT_MS);

  rf.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  rf.enableTransmit(RF_TX_PIN);
  rf.setRepeatTransmit(6);
  pinMode(BARK_MIC_PIN, INPUT);
#ifdef ADC_11db
  analogSetPinAttenuation(BARK_MIC_PIN, ADC_11db);
#elif defined(ADC_ATTEN_DB_11)
  analogSetPinAttenuation(BARK_MIC_PIN, ADC_ATTEN_DB_11);
#endif

  // Restore "saved listen armed" behavior across outage and re-arm panel.
  if (listenSavedArmed) {
    Serial.println("Restoring saved armed mode -> sending panel ARM signal");
    sendPanelSignal(true);
  }

  Serial.println("Setup completed.");
}

void loop() {
  monitorLoopGap();
  processWebServer();
  ensureWiFiConnected();
  pollTelegram();
  processWebServer();
  processBarkDetection();
  processRf();
  delay(1);
}
