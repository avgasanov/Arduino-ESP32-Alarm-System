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
