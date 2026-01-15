# Arduino ESP32 Alarm System

ESP32 alarm controller with RF433 sensor monitoring, Telegram control, local web UI, bark/noise detection, Wake on LAN, push events, and direct SMS alerts through a supported TP-Link LTE router.

## Features

- RF433 receive for 24-bit Kerui-style alarm sensors.
- RF433 transmit for stored remotes and panel arm/disarm signals.
- Telegram bot commands with local authentication.
- Local ESP32 web interface for dashboard, groups, sensors, remotes, SMS, and config import/export.
- Bark/noise detector using a digital or analog sound sensor on GPIO34.
- Persistent state in SPIFFS for sensors, groups, remotes, push, SMS, and auth chats.
- Runtime-only listen-all mode that does not persist after reboot.
- Wi-Fi provisioning AP after repeated connection failures.
- Direct router SMS support for TP-Link TL-MR100 with the GDPR encrypted web API firmware profile.

## Credentials

The repository does not include real credentials.

1. Copy `secrets.example.h` to `secrets.h`.
2. Set your Wi-Fi SSID and password.
3. Add your Telegram bot token.
4. Replace the authentication code with a private code.

`secrets.h` is ignored by Git so local credentials do not get committed by accident.

## Arduino Dependencies

Install these libraries from Arduino Library Manager:

- `RCSwitch`
- `UniversalTelegramBot`
- `ArduinoJson`

Board package:

- `ESP32 by Espressif Systems`

The project uses the ESP32 huge app partition profile in `sketch.yaml`.

## Wiring

RF receiver:

- `VCC` to `3V3`
- `GND` to `GND`
- `DATA` to `GPIO27`

RF transmitter:

- `VCC` to the transmitter module's required voltage
- `GND` to `GND`
- `DATA` to `GPIO14`

Sound sensor:

- `OUT` or `AO` to `GPIO34`
- `VCC` to `3V3`
- `GND` to `GND`

GPIO34 is input-only on ESP32. If the module outputs 5V, use a divider before connecting it.

## Build

```bash
cp secrets.example.h secrets.h
arduino-cli compile Arduino_ESP32_Alarm_System
```

If your CLI needs an explicit board:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app Arduino_ESP32_Alarm_System
```

## First Run

On boot, the device tries the credentials from SPIFFS first, then `secrets.h`.

If Wi-Fi cannot connect after the configured retries, it starts a setup access point:

- AP SSID: `AlarmSetup-<chip-suffix>`
- AP password: `alarmsetup`
- Setup page: `http://192.168.4.1`

From there you can choose a router Wi-Fi network or continue in offline AP mode.

## Main Commands

Authentication:

- `/auth <code>`
- `authenticate <code>`

Modes:

- `/listen_all_on`
- `/listen_all_off`
- `/listen_saved_on`
- `/listen_saved_off`
- `/status`

Sensors:

- `/sensor_add <code> <group 0-99> [name]`
- `/sensor_remove <code>`
- `/sensor_name <code> <name>`
- `/sensor_notify_on <code>`
- `/sensor_notify_off <code>`
- `/sensors`

Groups:

- `/group_set <group 0-99> <name>`
- `/group_reset <group 0-99>`
- `/group_arm <group 0-99>`
- `/group_disarm <group 0-99>`
- `/groups`

Remotes:

- `/remote_add <key> <code> [bits]`
- `/remote_remove <key>`
- `/remote_send <key>`
- `/remotes`

Bark detector:

- `/bark_on`
- `/bark_off`
- `/bark_status`
- `/bark_mode <ao|do|both>`
- `/bark_do_level <0|1>`
- `/bark_threshold <0-4095>`
- `/bark_cooldown <ms>`
- `/bark_code <code>`
- `/bark_emit_rf_on`
- `/bark_emit_rf_off`
- `/bark_test`

Router SMS:

- `/sms_status`
- `/sms_on`
- `/sms_off`
- `/sms_router_list`
- `/sms_router_set <id>`
- `/sms_fw_list`
- `/sms_fw_set <id>`
- `/sms_ip <router_ip_or_host>`
- `/sms_password <router_password>`
- `/sms_to_add <phone_number>`
- `/sms_to_remove <phone_number>`
- `/sms_to_list`
- `/sms_to_clear`
- `/sms_test [message]`

System:

- `/server_set <host_or_url> [port]`
- `/server_show`
- `/server_clear`
- `/wol <mac> [broadcast_ip] [port]`
- `/config_export`
- `/config_import <json>`
- `/config_import_begin`
- `/config_import_end`
- `/config_import_cancel`
- `/reset_all`

## Notes

- Config export includes operational configuration and may include Wi-Fi/router settings. Treat exported JSON as private.
- Telegram TLS validation is disabled with `setInsecure()` to keep the ESP32 flow simple. Use a pinned certificate or trust anchor before relying on it in a high-security deployment.
- The SMS integration is router and firmware specific. Test with a harmless SMS before enabling alarm alerts.
