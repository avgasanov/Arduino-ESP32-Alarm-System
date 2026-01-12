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

