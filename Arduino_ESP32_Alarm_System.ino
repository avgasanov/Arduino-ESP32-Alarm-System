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
static constexpr const char* WIFI_AP_SSID_PREFIX = "AlarmSetup";
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
