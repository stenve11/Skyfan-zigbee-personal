# Personal notes – Skyfan Zigbee (Steve's fork/backup)

Personal backup of my modified build of https://github.com/rhysfred/skyfan-zigbee
(GPL v3, (c) Rhys Frederick / Front Left Speaker – see LICENCE.md). Not intended to be merged upstream.
Based on upstream commit 25e56eb.

## Setup
- Ventair Skyfan DC ceiling fan (fan + light), Tuya MCU serial at 9600 baud.
- Seeed XIAO ESP32-C6 replacing the Tuya USB stick; Zigbee router (ZCZR).
- Home Assistant + Zigbee2MQTT 2.14.3 add-on, SLZB PoE (ZStack3x0) coordinator.
- Arduino: esp32 core 3.3.12, board XIAO_ESP32C6, partition "Zigbee ZCZR 4MB with spiffs",
  Zigbee Mode = Zigbee ZCZR, USB CDC On Boot = Enabled. Keep exactly ONE .ino in the sketch folder.
- Z2M external converter: zigbee2mqtt/skyfanConverter.mjs (copy to Z2M data folder, add under external_converters).

## Tuya DPIDs
1 fan switch | 2 mode | 3 fan speed 0-5 | 8 direction | 15 light switch | 16 brightness 0-5 |
19 colour temp (0 cool, 1 natural, 2 warm). MCU sends a burst of all DPs on any change (remote or command).

## Changes vs upstream
1. Remote -> Zigbee sync: fan speed/direction/colour temp now reported when changed from the RF remote.
2. Custom cluster 0xFC00 (mfr 0x1818): 0x0001 fanDirection, 0x0002 fanSpeed (exact 0-5),
   0x0003 powerOnLight, 0x0004 powerOnFan (0 default / 1 previous / 2 on / 3 off).
3. Colour temp bug: validity check was `> COOL` (only accepted 0) -> `> WARM` in SkyfanZigbeeLight.cpp.
4. Power-on restore (PowerOnRestore.h): saves light/fan state to NVS after 10 s settle; restores 3 s after
   a genuine power-on reset. Settable from Z2M as config drop-downs.
5. ESP-Zigbee SDK lock (esp_zb_lock_acquire) added around attribute calls outside callbacks.
6. Fan slider bug: fan shadow state seeded from real MCU state (mcuFan) so setting a speed while stopped no
   longer flicks back to off. Fan beeps if a speed is sent while off (MCU quirk); MCU reports remembered
   speed even while stopped.
7. Polling of MCU disabled (TUYA_STATE_POLL_INTERVAL_MS 0) – the MCU pushes changes itself.
8. Converter: `name` on custom cluster + attributes and `write: true` are REQUIRED, otherwise Z2M shows
   cluster 'undefined' / "No converter available" and refuses writes. Light presets cool=154 / neutral=238 / warm=333.
   Fan exposed as modes off/1-5/on with HA speed slider + direction.

## OTA
- Build image: image_builder_tool.py -m 6168 -i 1 -v <N> --tag 0, serve via a local Z2M OTA override index.
- Bump OTA_FILE_VERSION in SkyfanConfig.h every build (currently 0x9).
- Do NOT apply the "update available" for upstream v0.0.5-alpha.2 – it would overwrite this custom firmware.
- Z2M installed_version only refreshes after an OTA query ("check for updates").

## Debugging tips
- Z2M debug lines only reach the frontend with `advanced: log_debug_to_mqtt_frontend: true`; otherwise read add-on log.
- Router: the stick is a Zigbee router, but other devices only route through it as the mesh decides; none may at first.

## Ideas / TODO
- Offline control (works with HA/network down): fan is kept on mains; bind a wall controller via Zigbee.
  Shelly Gen4 1L / i4 can bind momentary-input toggle to groups (2PM Gen4 not documented). Light toggle works via group;
  fan needs firmware work: bindable On/Off + Level endpoint for the fan.
- OTA helper script (build + version bump + local index in one command).
- Real-world power-cut restore test result: pending.
- ESPHome considered: Zigbee support immature, Wi-Fi OTA only, not better for offline.
- Possible upstream issue: missing cluster/attribute `name`, `write:true`, COOL->WARM check.

## OTA via GitHub (set up 2026-11-17)
- Repo workflow builds on tag push (e.g. `git tag v0.1.0 && git push origin v0.1.0`) and publishes `skyfan-zigbee.ota`
  as a release asset; it then appends the entry to `zigbee2mqtt/ota-index.json` automatically.
- Upstream's index entries were removed from this repo's index so Z2M never offers upstream 2.0.0 (would overwrite these fixes).
- Z2M config: `ota: zigbee_ota_override_index_location: https://raw.githubusercontent.com/stenve11/Skyfan-zigbee-personal/main/zigbee2mqtt/ota-index.json`
- Repo must be PUBLIC for Z2M to download without a login.
- Tag versions map to OTA numbers (v0.1.0 = 4321280) - always higher than the early local test builds (1-9).
- Workflow pinned to esp32 core 3.3.12 to match local builds.
