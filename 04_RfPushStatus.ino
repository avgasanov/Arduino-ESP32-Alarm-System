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

