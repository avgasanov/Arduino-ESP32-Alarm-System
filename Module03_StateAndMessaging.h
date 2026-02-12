// SPIFFS persistence, config import/export, auth state, and message helpers.

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
