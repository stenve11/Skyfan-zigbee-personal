/*
 * Skyfan Configuration - Centralized configuration constants and enums
 * Copyright (C) 2025 Rhys Frederick at Front Left Speaker
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3, as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SKYFAN_CONFIG_H
#define SKYFAN_CONFIG_H

#include <Arduino.h>

// === Feature Configuration ===
// #define __DEBUG__   // Comment out to disable debug logging
// #define __BOOT_LOG__  // Uncomment to enable boot logging persistence

// === Firmware Version ===
// Injected at build time by CI; fallback for local builds
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev-local"
#endif

// === OTA Configuration ===
// OTA file version format: 0xRRRRRDDD (32-bit)
//   - Upper 20 bits (RRRRR): Tagged releases (>= 0x00001000)
//   - Lower 12 bits (DDD): Dev builds (1-4095)
//
// Release encoding (5 bits each in upper 20 bits):
//   - Major: bits 27-31 (0-31), Minor: bits 22-26 (0-31)
//   - Patch: bits 17-21 (0-31), Prerelease: bits 12-16 (alpha=1-10, beta=12-21, rc=23-30, stable=31)
//
// Examples: dev build 50 = 0x00000032, v0.0.4-alpha.1 = 0x00081000, v1.0.0 = 0x0800F000
#ifndef OTA_FILE_VERSION
#define OTA_FILE_VERSION 0x00000009  // Fallback for local builds - raise for every OTA you build
#endif
#define OTA_DOWNLOADED_FILE_VERSION OTA_FILE_VERSION  // Initially same as running version
#define OTA_HW_VERSION              0x0001            // Hardware revision (increment for breaking HW changes)
#define OTA_MANUFACTURER_CODE       0x1818                 // Front Left Speaker (same as FLS_MANUFACTURER_CODE)
#define OTA_IMAGE_TYPE              0x0001            // Image type identifier

// === Hardware Configuration ===
#define FACTORY_RESET_BUTTON_PIN   BOOT_PIN
#define DEBUG_SERIAL_RX_PIN        21
#define DEBUG_SERIAL_TX_PIN        20
#define DEBUG_SERIAL_BAUD_RATE     115200

// === Zigbee Configuration ===
#define ZIGBEE_FAN_CONTROL_ENDPOINT    1
#define ZIGBEE_LIGHT_CONTROL_ENDPOINT  2
#define ZIGBEE_DEVICE_MANUFACTURER     "Front Left Speaker"

constexpr const char* PRODUCT_ID_FAN_LIGHT = "pktxz1vynowmavuc";
constexpr const char* ZIGBEE_MODEL_NAME_FAN_LIGHT = "Ventair Skyfan/Light ZB Adaptor";
constexpr const char* ZIGBEE_MODEL_NAME_FAN_ONLY  = "Ventair Skyfan ZB Adaptor";

inline bool isLightModel(const char* productId) {
  if (productId == nullptr || productId[0] == '\0') return true;
  return strcmp(productId, PRODUCT_ID_FAN_LIGHT) == 0;
}

// === Custom Cluster Configuration ===
#define VENTAIR_CUSTOM_CLUSTER_ID      0xFC00  // Custom cluster ID. Made up but must be > ESP_ZB_CUSTOM_CLUSTER_ID_MIN_VAL
#define FLS_MANUFACTURER_CODE          0x1818  // Front Left Speaker manufacturer code. Again made up. Meant to be registered
#define CUSTOM_ATTR_FAN_DIRECTION      0x0001  // Custom attribute ID for fan direction
#define CUSTOM_ATTR_FAN_SPEED          0x0002  // Custom attribute ID for exact fan speed (0 = off, 1-5)
#define CUSTOM_ATTR_POWER_ON_LIGHT     0x0003  // Power-on behaviour of the light (0 default, 1 previous, 2 on, 3 off)
#define CUSTOM_ATTR_POWER_ON_FAN       0x0004  // Power-on behaviour of the fan (same values)

// === ZCL Frame Control Bytes (for raw APS reporting) ===
#define ZCL_FRAME_CTRL_GLOBAL_TO_CLIENT          0x18  // Global cmd, server-to-client, disable default response
#define ZCL_FRAME_CTRL_GLOBAL_TO_CLIENT_MANUF    0x1C  // Same as above + manufacturer specific
#define ZCL_CMD_REPORT_ATTRIBUTES                0x0A  // Report Attributes command ID

// === Timing Configuration ===
#define TUYA_HEARTBEAT_INTERVAL_MS     10000  // 10 seconds
#define TUYA_CONNECTION_TIMEOUT_MS     30000  // 30 seconds
#define TUYA_COMMAND_TIMEOUT_MS        500    // 0.5 seconds - ACK timeout
#define TUYA_STATUS_RESPONSE_TIMEOUT_MS 1500  // 1500ms - Status report timeout
#define TUYA_STATE_POLL_INTERVAL_MS    0      // Ask the MCU for its full state this often (0 = off), so remote-control changes are noticed
#define TUYA_INTER_COMMAND_DELAY_MS    10     // 10ms minimum gap between commands
#define ZIGBEE_CONNECTION_POLL_MS      100    // 100ms
#define FACTORY_RESET_DELAY_MS         1000   // 1 second

// === Baud Rate Negotiation ===
#define BAUD_RATE_PRIMARY              9600     // Try first (per Tuya spec)
#define BAUD_RATE_SECONDARY            115200   // Try second (also used as fallback)
#define BAUD_NEGOTIATION_INTERVAL_MS   1000     // 1 second between queries (per Tuya spec)
// Note: Uses TUYA_COMMAND_TIMEOUT_MS (500ms) for response timeout per Tuya spec



// === Power-on behaviour ===
// What the stick does when the fan loses and regains power. The fan's own controller always starts with the
// fan stopped and the light on at its own default brightness and colour temperature.
#define POWER_ON_DEFAULT   0   // leave it as the fan's controller starts
#define POWER_ON_PREVIOUS  1   // put it back as it was before the power cut
#define POWER_ON_ON        2   // light: on at the previous brightness/colour; fan: running at the previous speed
#define POWER_ON_OFF       3   // turn it off

// These two are only the starting values; once changed from Zigbee2MQTT the stick remembers the new setting.
#define POWER_ON_LIGHT_MODE       POWER_ON_PREVIOUS   // one of the four values above
#define POWER_ON_FAN_MODE         POWER_ON_DEFAULT    // DEFAULT keeps the fan stopped after an outage
#define POWER_ON_RESTORE_DELAY_MS 3000                // wait this long after boot so the fan's controller is ready
#define POWER_ON_SAVE_DELAY_MS    10000               // save a change to flash once nothing has changed for this long

// Snapshot of the fan and light, kept in flash for the power-on behaviour above
struct PowerSnapshot {
  uint8_t version;          // 0 = nothing saved
  uint8_t valid;            // bit 0: light values are known, bit 1: fan values are known
  uint8_t lightOn;
  uint8_t lightBrightness;  // 1-5
  uint8_t lightTemp;        // 0-2 (ColourTempLevel)
  uint8_t fanOn;
  uint8_t fanSpeed;         // 1-5
  uint8_t fanDirection;     // 0 = forward, 1 = reverse
};

// === Colour Temperature Configuration ===
// Kelvin values for each temperature setting
#define COLOUR_TEMP_WARM_KELVIN         3000
#define COLOUR_TEMP_NATURAL_KELVIN      4200
#define COLOUR_TEMP_COOL_KELVIN         6500

// Corresponding mired values (1,000,000 / Kelvin)
#define COLOUR_TEMP_WARM_MIRED          333    // 1000000/3000
#define COLOUR_TEMP_NATURAL_MIRED       238    // 1000000/4200
#define COLOUR_TEMP_COOL_MIRED          154    // 1000000/6500

// Mired range for Zigbee colour temperature capability
#define ZIGBEE_COLOUR_TEMP_MIN_MIRED    COLOUR_TEMP_COOL_MIRED
#define ZIGBEE_COLOUR_TEMP_MAX_MIRED    COLOUR_TEMP_WARM_MIRED

// === Range Configuration ===
#define TUYA_BRIGHTNESS_MIN            0
#define TUYA_BRIGHTNESS_MAX            5
#define TUYA_FAN_SPEED_MIN             0      // Integer range for MCU
#define TUYA_FAN_SPEED_MAX             5      // Integer range for MCU
#define ZIGBEE_BRIGHTNESS_MIN          0
#define ZIGBEE_BRIGHTNESS_MAX          254

// === Fan Speed Mapping (Integer values for Tuya MCU) ===
#define FAN_SPEED_LOW_TUYA             1      // Integer value sent to MCU
#define FAN_SPEED_MEDIUM_TUYA          3      // Integer value sent to MCU
#define FAN_SPEED_HIGH_TUYA            5      // Integer value sent to MCU

// === Buffer Configuration ===
#define TUYA_BUFFER_SIZE               256
#define TUYA_RX_BUFFER_SIZE            256

// === Custom Cluster Role ===
enum class CustomClusterRole : uint8_t {
  SERVER = 0,
  CLIENT = 1
};

// === Enhanced Enums ===

// Colour temperature levels with clear naming
enum class ColourTempLevel : uint8_t {
  COOL = 0,      // 6500K
  NATURAL = 1,   // 4200K
  WARM = 2       // 3000K
};

// Fan mode for MCU-specific operations (enum values sent to MCU)
enum class TuyaFanMode : uint8_t {
  NORMAL = 0,
  ECO = 1,
  SLEEP = 2
};

// Fan direction (enum values sent to MCU)
enum class FanDirection : uint8_t {
  FORWARD = 0,
  REVERSE = 1
};

// Protocol states for better state machine readability

enum class TuyaProtocolState : uint8_t {
  WAIT_HEADER_1 = 0,
  WAIT_HEADER_2 = 1,
  WAIT_VERSION = 2,
  WAIT_COMMAND = 3,
  WAIT_LENGTH_HIGH = 4,
  WAIT_LENGTH_LOW = 5,
  WAIT_DATA_AND_CHECKSUM = 6
};

// === Utility Functions ===

// Convert Kelvin to Mired
inline constexpr uint16_t kelvinToMired(uint16_t kelvin) {
  return (kelvin > 0) ? (1000000 / kelvin) : 0;
}

// Convert Mired to Kelvin
inline constexpr uint16_t miredToKelvin(uint16_t mired) {
  return (mired > 0) ? (1000000 / mired) : 0;
}

// Check if value is within range
template<typename T>
inline constexpr bool isInRange(T value, T min, T max) {
  return (value >= min) && (value <= max);
}

// Clamp value to range
template<typename T>
inline constexpr T clamp(T value, T min, T max) {
  return (value < min) ? min : ((value > max) ? max : value);
}

// Validate Tuya fan speed (0-5 integer range)
inline bool isValidTuyaFanSpeed(uint8_t speed) {
  return isInRange(speed, static_cast<uint8_t>(TUYA_FAN_SPEED_MIN), static_cast<uint8_t>(TUYA_FAN_SPEED_MAX));
}

// Level Control (0-254) <-> fan speed (0-5) for remotes bound to the fan endpoint.
// Each speed owns a band of levels: 1-51 -> speed 1, 52-102 -> 2, 103-153 -> 3, 154-204 -> 4, 205-254 -> 5.
// Level 0 means off. A speed is stored as the middle of its band, so a few small steps up or down
// from there do not jump straight past the next speed.
inline uint8_t levelToFanSpeed(uint8_t level) {
  if (level == 0) return 0;
  uint16_t speed = (static_cast<uint16_t>(level) + 50) / 51;
  return (speed > TUYA_FAN_SPEED_MAX) ? static_cast<uint8_t>(TUYA_FAN_SPEED_MAX) : static_cast<uint8_t>(speed);
}

inline uint8_t fanSpeedToLevel(uint8_t speed) {
  if (speed == 0) return 0;
  if (speed > TUYA_FAN_SPEED_MAX) speed = TUYA_FAN_SPEED_MAX;
  return static_cast<uint8_t>(speed * 51 - 25);  // 26, 77, 128, 179, 230
}

// Validate Tuya brightness (0-5 integer range)
inline bool isValidTuyaBrightness(uint8_t brightness) {
  return isInRange(brightness, static_cast<uint8_t>(TUYA_BRIGHTNESS_MIN), static_cast<uint8_t>(TUYA_BRIGHTNESS_MAX));
}

// === Colour Temperature Conversion Functions ===

// Convert Kelvin to appropriate Tuya colour temperature enum
inline ColourTempLevel kelvinToTuyaColourTemp(uint16_t kelvin) {
  if (kelvin <= (COLOUR_TEMP_WARM_KELVIN + COLOUR_TEMP_NATURAL_KELVIN) / 2) {
    return ColourTempLevel::WARM;
  } else if (kelvin <= (COLOUR_TEMP_NATURAL_KELVIN + COLOUR_TEMP_COOL_KELVIN) / 2) {
    return ColourTempLevel::NATURAL;
  } else {
    return ColourTempLevel::COOL;
  }
}

// Convert Mired to appropriate Tuya colour temperature enum  
inline ColourTempLevel miredToTuyaColourTemp(uint16_t mired) {
  return kelvinToTuyaColourTemp(miredToKelvin(mired));
}

// Convert Tuya colour temperature enum to Mired
inline uint16_t tuyaColourTempToMired(ColourTempLevel colourTemp) {
  switch (colourTemp) {
    case ColourTempLevel::WARM:
      return COLOUR_TEMP_WARM_MIRED;
    case ColourTempLevel::NATURAL:
      return COLOUR_TEMP_NATURAL_MIRED;
    case ColourTempLevel::COOL:
      return COLOUR_TEMP_COOL_MIRED;
    default:
      return COLOUR_TEMP_WARM_MIRED;  // Default to warm
  }
}

// Convert Tuya colour temperature enum to Kelvin
inline uint16_t tuyaColourTempToKelvin(ColourTempLevel colourTemp) {
  switch (colourTemp) {
    case ColourTempLevel::WARM:
      return COLOUR_TEMP_WARM_KELVIN;
    case ColourTempLevel::NATURAL:
      return COLOUR_TEMP_NATURAL_KELVIN;
    case ColourTempLevel::COOL:
      return COLOUR_TEMP_COOL_KELVIN;
    default:
      return COLOUR_TEMP_WARM_KELVIN;  // Default to warm
  }
}

// === Range Mapping Functions ===

// Map Zigbee brightness to Tuya brightness with validation
// Rounds to nearest so that each level's own reported Zigbee value maps back to the same level
inline uint8_t zigbeeBrightnessToTuya(uint8_t zigbeeBrightness) {
  uint8_t clamped = clamp(zigbeeBrightness, static_cast<uint8_t>(ZIGBEE_BRIGHTNESS_MIN), static_cast<uint8_t>(ZIGBEE_BRIGHTNESS_MAX));
  return (uint8_t)(((uint16_t)clamped * TUYA_BRIGHTNESS_MAX + ZIGBEE_BRIGHTNESS_MAX / 2) / ZIGBEE_BRIGHTNESS_MAX);
}

// Map Tuya brightness to Zigbee brightness with validation
inline uint8_t tuyaBrightnessToZigbee(uint8_t tuyaBrightness) {
  uint8_t clamped = clamp(tuyaBrightness, static_cast<uint8_t>(TUYA_BRIGHTNESS_MIN), static_cast<uint8_t>(TUYA_BRIGHTNESS_MAX));
  return map(clamped, TUYA_BRIGHTNESS_MIN, TUYA_BRIGHTNESS_MAX, ZIGBEE_BRIGHTNESS_MIN, ZIGBEE_BRIGHTNESS_MAX);
}



#endif // SKYFAN_CONFIG_H