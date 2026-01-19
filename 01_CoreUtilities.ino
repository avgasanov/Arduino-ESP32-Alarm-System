// Shared helpers, cooperative delays, and lightweight performance logging.

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
