// Direct SMS support for router-backed LTE modems.

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
