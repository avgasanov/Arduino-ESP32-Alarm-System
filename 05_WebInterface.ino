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

