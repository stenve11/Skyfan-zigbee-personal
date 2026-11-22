/*
 * Skyfan Zigbee Library - Extended Zigbee classes and custom attributes for Skyfan controller
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

#include "SkyfanZigbee.h"
#include "TuyaProtocol.h"
#include "Logger.h"

// The ESP-Zigbee SDK requires its lock to be held for API calls made from outside Zigbee callbacks
// (here: the Arduino loop task, which handles the fan's status reports). The short timeout stops us
// hanging if we are already inside a callback where the stack holds the lock.
namespace {
struct ZbLockGuard {
  bool locked;
  ZbLockGuard() : locked(esp_zb_lock_acquire(pdMS_TO_TICKS(100))) {}
  ~ZbLockGuard() {
    if (locked) esp_zb_lock_release();
  }
};
}  // namespace

// Constructor copied from ZigbeeFanControl with modifications for our use case
SkyfanZigbeeFanControl::SkyfanZigbeeFanControl(uint8_t endpoint) : ZigbeeEP(endpoint) {
  _device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID;  // There is no FAN_CONTROL_DEVICE_ID in the Zigbee spec
  fanModeCallback = nullptr;
  fanDirectionCallback = nullptr;

  // Create cluster list with basic, identify, and fan control clusters
  // (copied exactly from ZigbeeFanControl)
  _cluster_list = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(_cluster_list, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(_cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_fan_control_cluster(_cluster_list, esp_zb_fan_control_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  // On/Off, Level Control and Groups so a bound switch, dial or group can drive the fan:
  // On/Off turns it on (at its last speed) or off, Level Control picks the speed (0 = off, 1-254 = speeds 1-5).
  esp_zb_groups_cluster_cfg_t groupsCfg = {.groups_name_support_id = ESP_ZB_ZCL_GROUPS_NAME_SUPPORT_DEFAULT_VALUE};
  esp_zb_cluster_list_add_groups_cluster(_cluster_list, esp_zb_groups_cluster_create(&groupsCfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_on_off_cluster_cfg_t onOffCfg = {.on_off = ESP_ZB_ZCL_ON_OFF_ON_OFF_DEFAULT_VALUE};
  esp_zb_cluster_list_add_on_off_cluster(_cluster_list, esp_zb_on_off_cluster_create(&onOffCfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_level_cluster_cfg_t levelCfg = {.current_level = ESP_ZB_ZCL_LEVEL_CONTROL_CURRENT_LEVEL_DEFAULT_VALUE};
  esp_zb_cluster_list_add_level_cluster(_cluster_list, esp_zb_level_cluster_create(&levelCfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  // Set endpoint config (copied exactly from ZigbeeFanControl)
  _ep_config = {
    .endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_HEATING_COOLING_UNIT_DEVICE_ID, .app_device_version = 0
  };
}

SkyfanZigbeeFanControl::~SkyfanZigbeeFanControl() {
  cleanupCustomCluster();
}

void SkyfanZigbeeFanControl::onFanDirectionChange(void (*callback)(uint8_t direction)) {
  fanDirectionCallback = callback;
}

void SkyfanZigbeeFanControl::onFanModeChange(void (*callback)(ZigbeeFanMode mode)) {
  fanModeCallback = callback;
}

void SkyfanZigbeeFanControl::onFanSpeedChange(void (*callback)(uint8_t speed)) {
  fanSpeedCallback = callback;
}

void SkyfanZigbeeFanControl::onRemotePowerChange(void (*callback)(bool on)) {
  remotePowerCallback = callback;
}

void SkyfanZigbeeFanControl::onRemoteLevelChange(void (*callback)(uint8_t level)) {
  remoteLevelCallback = callback;
}

// Keep the On/Off and Level attributes matching the fan. The stack acts on them when a command arrives
// (a Toggle flips the stored On/Off value), so a change made with the fan's own remote or from Zigbee2MQTT
// must show up here too. The level holds the remembered speed while the fan is stopped, like a light's brightness.
void SkyfanZigbeeFanControl::syncRemoteControl(bool on, uint8_t speed) {
  uint8_t levelSpeed = on ? speed : _lastMcuSpeed;
  bool levelNeeded = (levelSpeed > 0 && levelToFanSpeed(_remoteLevel) != levelSpeed);
  if (_remoteOn == on && !levelNeeded) return;

  ZbLockGuard guard;
  if (_remoteOn != on) {
    _echoOn = on ? 1 : 0;  // set before the write: the callback can fire during it
    _echoOnAt = millis();
    esp_zb_zcl_status_t ret = esp_zb_zcl_set_attribute_val(_endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                           ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, (void *)&on, false);
    if (ret == ESP_ZB_ZCL_STATUS_SUCCESS) _remoteOn = on;
  }
  if (levelNeeded) {
    uint8_t level = fanSpeedToLevel(levelSpeed);
    _echoLevel = level;
    _echoLevelAt = millis();
    esp_zb_zcl_status_t ret = esp_zb_zcl_set_attribute_val(_endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                           ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, (void *)&level, false);
    if (ret == ESP_ZB_ZCL_STATUS_SUCCESS) _remoteLevel = level;
  }
}

void SkyfanZigbeeFanControl::setInitialPowerOn(uint8_t light, uint8_t fan) {
  _initialPowerOnLight = light;
  _initialPowerOnFan = fan;
}

void SkyfanZigbeeFanControl::onPowerOnChange(void (*callback)(uint8_t which, uint8_t value)) {
  powerOnCallback = callback;
}

// Copied from ZigbeeFanControl::setFanModeSequence
bool SkyfanZigbeeFanControl::setFanModeSequence(ZigbeeFanModeSequence sequence) {
  esp_zb_attribute_list_t *fan_control_cluster =
    esp_zb_cluster_list_get_cluster(_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_err_t ret = esp_zb_cluster_update_attr(fan_control_cluster, ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID, (void *)&sequence);
  if (ret != ESP_OK) {
    Log::error("Failed to set fan mode sequence: 0x%x", ret);
    return false;
  }
  _current_fan_mode_sequence = sequence;
  _current_fan_mode = FAN_MODE_OFF;
  // Set initial fan mode to OFF
  ret = esp_zb_cluster_update_attr(fan_control_cluster, ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, (void *)&_current_fan_mode);
  if (ret != ESP_OK) {
    Log::error("Failed to set fan mode: 0x%x", ret);
    return false;
  }
  return true;
}

bool SkyfanZigbeeFanControl::setFanMode(ZigbeeFanMode mode) {
  if (_current_fan_mode == mode) return true;

  _current_fan_mode = mode;

  // Update Zigbee attribute (works after Zigbee.begin(), fails silently before)
  esp_zb_zcl_status_t ret;
  {
    ZbLockGuard guard;
    ret = esp_zb_zcl_set_attribute_val(
      _endpoint,
      ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
      ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID,
      (void *)&mode,
      false  // Don't check value
    );
  }
  return (ret == ESP_ZB_ZCL_STATUS_SUCCESS);
}

bool SkyfanZigbeeFanControl::setFanState(bool on) {
  ZigbeeFanMode targetMode = on ? FAN_MODE_ON : FAN_MODE_OFF;
  return setFanMode(targetMode);
}

bool SkyfanZigbeeFanControl::setFanSpeed(uint8_t speed) {
  // Validate input range
  if (!isValidTuyaFanSpeed(speed)) {
    return false;
  }
  
  ZigbeeFanMode mode;
  switch (speed) {
    case TUYA_FAN_SPEED_MIN:  // 0
      mode = FAN_MODE_OFF;
      break;
    case FAN_SPEED_LOW_TUYA:  // 1
    case FAN_SPEED_LOW_TUYA + 1:  // 2
      mode = FAN_MODE_LOW;
      break;
    case FAN_SPEED_MEDIUM_TUYA:  // 3
    case FAN_SPEED_MEDIUM_TUYA + 1:  // 4
      mode = FAN_MODE_MEDIUM;
      break;
    case FAN_SPEED_HIGH_TUYA:  // 5
      mode = FAN_MODE_HIGH;
      break;
    default:
      mode = FAN_MODE_ON;  // Generic on state for unknown speeds
      break;
  }
  return setFanMode(mode);
}

bool SkyfanZigbeeFanControl::setFanDirection(uint8_t direction) {
  // Validate direction
  if (direction > static_cast<uint8_t>(FanDirection::REVERSE)) {
    return false;
  }

  if (getFanDirection() == direction) return true;

  // Update the manufacturer-specific cluster attribute
  if (customCluster && customClusterRegistered) {
    ZbLockGuard guard;
    esp_zb_zcl_status_t ret = esp_zb_zcl_set_manufacturer_attribute_val(_endpoint, VENTAIR_CUSTOM_CLUSTER_ID,
                                                                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                                                        FLS_MANUFACTURER_CODE,
                                                                        CUSTOM_ATTR_FAN_DIRECTION, 
                                                                        (void *)&direction, false);
    return (ret == ESP_ZB_ZCL_STATUS_SUCCESS);
  }
  return false;
}

uint8_t SkyfanZigbeeFanControl::getFanDirection() const {
  if (customCluster && customClusterRegistered) {
    // Get the manufacturer-specific attribute from the custom cluster
    ZbLockGuard guard;
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_manufacturer_attribute(_endpoint, VENTAIR_CUSTOM_CLUSTER_ID, 
                                                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                                                     CUSTOM_ATTR_FAN_DIRECTION,
                                                                     FLS_MANUFACTURER_CODE);
    if (attr && attr->data_p) {
      return *((uint8_t*)attr->data_p);
    }
  }
  return static_cast<uint8_t>(FanDirection::FORWARD); // Default to forward
}

bool SkyfanZigbeeFanControl::setFanSpeedAttribute(uint8_t speed) {
  if (speed > TUYA_FAN_SPEED_MAX) {
    return false;
  }

  if (!(customCluster && customClusterRegistered)) {
    return false;
  }

  ZbLockGuard guard;

  // Skip the write if the attribute already holds this value
  esp_zb_zcl_attr_t *current = esp_zb_zcl_get_manufacturer_attribute(_endpoint, VENTAIR_CUSTOM_CLUSTER_ID,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                                      CUSTOM_ATTR_FAN_SPEED,
                                                                      FLS_MANUFACTURER_CODE);
  if (current && current->data_p && *((uint8_t*)current->data_p) == speed) {
    return true;
  }

  // Update the manufacturer-specific cluster attribute
  esp_zb_zcl_status_t ret = esp_zb_zcl_set_manufacturer_attribute_val(_endpoint, VENTAIR_CUSTOM_CLUSTER_ID,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                                      FLS_MANUFACTURER_CODE,
                                                                      CUSTOM_ATTR_FAN_SPEED,
                                                                      (void *)&speed, false);
  if (ret != ESP_ZB_ZCL_STATUS_SUCCESS) {
    Log::error("Failed to set fan speed attribute (status %d)", (int)ret);
  }
  return (ret == ESP_ZB_ZCL_STATUS_SUCCESS);
}

uint8_t SkyfanZigbeeFanControl::getFanSpeedAttribute() const {
  if (customCluster && customClusterRegistered) {
    ZbLockGuard guard;
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_manufacturer_attribute(_endpoint, VENTAIR_CUSTOM_CLUSTER_ID,
                                                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                                     CUSTOM_ATTR_FAN_SPEED,
                                                                     FLS_MANUFACTURER_CODE);
    if (attr && attr->data_p) {
      return *((uint8_t*)attr->data_p);
    }
  }
  return 0;
}

bool SkyfanZigbeeFanControl::createCustomCluster() {
  if (customClusterRegistered) {
    return true; // Already registered
  }
  
  // Clean up any existing cluster first (safety check)
  if (customCluster != nullptr) {
    cleanupCustomCluster();
  }
  
  // Create custom cluster attribute list
  customCluster = esp_zb_zcl_attr_list_create(VENTAIR_CUSTOM_CLUSTER_ID);
  if (!customCluster) {
    Log::error("Failed to create custom cluster attribute list");
    return false;
  }
  
  // Add manufacturer-specific fan direction attribute with REPORTING enabled
  uint8_t default_direction = static_cast<uint8_t>(FanDirection::FORWARD);
  esp_err_t ret = esp_zb_cluster_add_manufacturer_attr(customCluster,
                                                       VENTAIR_CUSTOM_CLUSTER_ID,
                                                       CUSTOM_ATTR_FAN_DIRECTION,
                                                       FLS_MANUFACTURER_CODE,
                                                       ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                                       ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                                       &default_direction);
  
  if (ret != ESP_OK) {
    Log::error("Failed to add custom fan direction attribute (code: %d)", ret);
    // Clean up the allocated cluster on failure
    cleanupCustomCluster();
    return false;
  }

  // Add manufacturer-specific exact fan speed attribute (0 = off, 1-5) with REPORTING enabled
  uint8_t default_speed = 0;
  ret = esp_zb_cluster_add_manufacturer_attr(customCluster,
                                             VENTAIR_CUSTOM_CLUSTER_ID,
                                             CUSTOM_ATTR_FAN_SPEED,
                                             FLS_MANUFACTURER_CODE,
                                             ESP_ZB_ZCL_ATTR_TYPE_U8,
                                             ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                             &default_speed);

  if (ret != ESP_OK) {
    Log::error("Failed to add custom fan speed attribute (code: %d)", ret);
    cleanupCustomCluster();
    return false;
  }

  // Power-on behaviour settings for the light and the fan (0 default, 1 previous, 2 on, 3 off), writable from Zigbee2MQTT
  uint8_t powerOnLight = _initialPowerOnLight;
  ret = esp_zb_cluster_add_manufacturer_attr(customCluster,
                                             VENTAIR_CUSTOM_CLUSTER_ID,
                                             CUSTOM_ATTR_POWER_ON_LIGHT,
                                             FLS_MANUFACTURER_CODE,
                                             ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                             ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                             &powerOnLight);
  if (ret != ESP_OK) {
    Log::error("Failed to add custom power-on light attribute (code: %d)", ret);
    cleanupCustomCluster();
    return false;
  }

  uint8_t powerOnFan = _initialPowerOnFan;
  ret = esp_zb_cluster_add_manufacturer_attr(customCluster,
                                             VENTAIR_CUSTOM_CLUSTER_ID,
                                             CUSTOM_ATTR_POWER_ON_FAN,
                                             FLS_MANUFACTURER_CODE,
                                             ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                             ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                             &powerOnFan);
  if (ret != ESP_OK) {
    Log::error("Failed to add custom power-on fan attribute (code: %d)", ret);
    cleanupCustomCluster();
    return false;
  }
  
  // Add custom cluster to cluster list
  ret = esp_zb_cluster_list_add_custom_cluster(_cluster_list, customCluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  
  if (ret == ESP_OK) {
    Log::debug("Custom cluster created and registered successfully");
    customClusterRegistered = true;
    return true;
  } else {
    Log::error("Failed to add custom cluster to cluster list (code: %d)", ret);
    // Clean up the allocated cluster on failure
    cleanupCustomCluster();
    return false;
  }
}

void SkyfanZigbeeFanControl::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) {
  // Handle our custom cluster for fan direction
  if (message->info.cluster == VENTAIR_CUSTOM_CLUSTER_ID) {
    if (message->attribute.id == CUSTOM_ATTR_FAN_DIRECTION) {
      uint8_t direction = *(uint8_t *)message->attribute.data.value;
      handleCustomClusterAttributeChange(VENTAIR_CUSTOM_CLUSTER_ID, CUSTOM_ATTR_FAN_DIRECTION, &direction);
    } else if (message->attribute.id == CUSTOM_ATTR_FAN_SPEED) {
      uint8_t speed = *(uint8_t *)message->attribute.data.value;
      handleCustomClusterAttributeChange(VENTAIR_CUSTOM_CLUSTER_ID, CUSTOM_ATTR_FAN_SPEED, &speed);
    } else if (message->attribute.id == CUSTOM_ATTR_POWER_ON_LIGHT || message->attribute.id == CUSTOM_ATTR_POWER_ON_FAN) {
      uint8_t value = *(uint8_t *)message->attribute.data.value;
      if (powerOnCallback && value <= POWER_ON_OFF) {
        powerOnCallback(message->attribute.id == CUSTOM_ATTR_POWER_ON_LIGHT ? 0 : 1, value);
      }
    } else {
      Log::debug("Received message ignored - attribute ID %d not supported for custom cluster", message->attribute.id);
    }
  } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL) {
    // Handle standard fan control cluster
    if (message->attribute.id == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM) {
      ZigbeeFanMode mode = *(ZigbeeFanMode *)message->attribute.data.value;
      _current_fan_mode = mode;  // Track current state
      if (fanModeCallback) {
        fanModeCallback(mode);
      }
    } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID &&
               message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM) {
      _current_fan_mode_sequence = *(ZigbeeFanModeSequence *)message->attribute.data.value;
    } else {
      Log::debug("Received message ignored - attribute ID %d not supported for Fan Control", message->attribute.id);
    }
  } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
    // On, Off or Toggle from a bound switch, remote or group (the stack has already updated the attribute)
    if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
      bool on = *(bool *)message->attribute.data.value;
      _remoteOn = on;
      if (_echoOn == (on ? 1 : 0) && (millis() - _echoOnAt) < ECHO_WINDOW_MS) {
        _echoOn = -1;
        Log::debug("On/Off %s is our own sync, ignored", on ? "on" : "off");
        return;
      }
      Log::debug("Bound On/Off command: %s", on ? "on" : "off");
      if (remotePowerCallback) {
        remotePowerCallback(on);
      }
    } else {
      Log::debug("Received message ignored - attribute ID %d not supported for On/Off", message->attribute.id);
    }
  } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
    // Move/Step/Move-to-level from a bound dial or remote; the stack updates the level and tells us each change
    if (message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) {
      uint8_t level = *(uint8_t *)message->attribute.data.value;
      _remoteLevel = level;
      if (_echoLevel == level && (millis() - _echoLevelAt) < ECHO_WINDOW_MS) {
        _echoLevel = -1;
        Log::debug("Level %d is our own sync, ignored", level);
        return;
      }
      Log::debug("Bound level change: %d", level);
      if (remoteLevelCallback) {
        remoteLevelCallback(level);
      }
    } else {
      Log::debug("Received message ignored - attribute ID %d not supported for Level Control", message->attribute.id);
    }
  } else {
    Log::debug("Received message ignored - cluster ID 0x%04X not supported", message->info.cluster);
  }
}

void SkyfanZigbeeFanControl::handleCustomClusterAttributeChange(uint16_t cluster_id, uint16_t attr_id, uint8_t *data) {
  if (cluster_id == VENTAIR_CUSTOM_CLUSTER_ID && attr_id == CUSTOM_ATTR_FAN_DIRECTION && fanDirectionCallback) {
    uint8_t direction = *data;
    if (direction <= static_cast<uint8_t>(FanDirection::REVERSE)) {
      fanDirectionCallback(direction);
      // Note: Logging is done in the callback (setFanDirection) to avoid duplicates
    }
  } else if (cluster_id == VENTAIR_CUSTOM_CLUSTER_ID && attr_id == CUSTOM_ATTR_FAN_SPEED && fanSpeedCallback) {
    uint8_t speed = *data;
    if (speed <= TUYA_FAN_SPEED_MAX) {
      fanSpeedCallback(speed);
    }
  }
}

uint16_t SkyfanZigbeeFanControl::getCustomClusterId() const {
  return VENTAIR_CUSTOM_CLUSTER_ID;
}

bool SkyfanZigbeeFanControl::isCustomClusterRegistered() const {
  return customClusterRegistered;
}

void SkyfanZigbeeFanControl::cleanupCustomCluster() {
  if (customCluster != nullptr) {
    // Note: ESP32 Zigbee SDK manages cluster lifecycle internally
    // Setting to nullptr to indicate cleanup
    customCluster = nullptr;
    customClusterRegistered = false;
    Log::debug("Custom cluster resources cleaned up");
  }
}

bool SkyfanZigbeeFanControl::reportFanMode() {
  // Build raw ZCL Report Attributes frame to bypass access flag check
  // ZCL Frame: [frame_ctrl][seq_num][cmd_id][attr_id_lo][attr_id_hi][attr_type][attr_value]
  uint8_t zcl_frame[7];
  zcl_frame[0] = ZCL_FRAME_CTRL_GLOBAL_TO_CLIENT;
  zcl_frame[1] = 0x00;  // Sequence number (will be filled by stack or ignored)
  zcl_frame[2] = ZCL_CMD_REPORT_ATTRIBUTES;
  zcl_frame[3] = ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID & 0xFF;
  zcl_frame[4] = (ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID >> 8) & 0xFF;
  zcl_frame[5] = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM;
  zcl_frame[6] = static_cast<uint8_t>(_current_fan_mode);

  esp_zb_apsde_data_req_t req;
  memset(&req, 0, sizeof(req));
  req.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;  // Use binding table
  req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  req.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL;
  req.src_endpoint = _endpoint;
  req.asdu_length = sizeof(zcl_frame);
  req.asdu = zcl_frame;
  req.tx_options = 0;  // No special options
  req.radius = 0;  // Use default radius

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_aps_data_request(&req);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Log::error("Failed to report fan mode via APS: 0x%x", ret);
    return false;
  }
  Log::debug("Fan mode report sent via raw APS (mode=%d)", _current_fan_mode);
  return true;
}

bool SkyfanZigbeeFanControl::reportFanDirection() {
  // Build raw ZCL Report Attributes frame for manufacturer-specific attribute
  // ZCL Frame: [frame_ctrl][manuf_lo][manuf_hi][seq_num][cmd_id][attr_id_lo][attr_id_hi][attr_type][attr_value]
  uint8_t direction = confirmedFanDirection;
  uint8_t zcl_frame[9];
  zcl_frame[0] = ZCL_FRAME_CTRL_GLOBAL_TO_CLIENT_MANUF;
  zcl_frame[1] = FLS_MANUFACTURER_CODE & 0xFF;
  zcl_frame[2] = (FLS_MANUFACTURER_CODE >> 8) & 0xFF;
  zcl_frame[3] = 0x00;  // Sequence number
  zcl_frame[4] = ZCL_CMD_REPORT_ATTRIBUTES;
  zcl_frame[5] = CUSTOM_ATTR_FAN_DIRECTION & 0xFF;
  zcl_frame[6] = (CUSTOM_ATTR_FAN_DIRECTION >> 8) & 0xFF;
  zcl_frame[7] = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM;
  zcl_frame[8] = direction;

  esp_zb_apsde_data_req_t req;
  memset(&req, 0, sizeof(req));
  req.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;  // Use binding table
  req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  req.cluster_id = VENTAIR_CUSTOM_CLUSTER_ID;
  req.src_endpoint = _endpoint;
  req.asdu_length = sizeof(zcl_frame);
  req.asdu = zcl_frame;
  req.tx_options = 0;
  req.radius = 0;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_aps_data_request(&req);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Log::error("Failed to report fan direction via APS: 0x%x", ret);
    return false;
  }
  Log::debug("Fan direction report sent via raw APS (direction=%d)", direction);
  return true;
}

bool SkyfanZigbeeFanControl::reportFanSpeed() {
  // Raw ZCL Report Attributes frame for the manufacturer-specific speed attribute
  uint8_t speed = getFanSpeedAttribute();
  uint8_t zcl_frame[9];
  zcl_frame[0] = ZCL_FRAME_CTRL_GLOBAL_TO_CLIENT_MANUF;
  zcl_frame[1] = FLS_MANUFACTURER_CODE & 0xFF;
  zcl_frame[2] = (FLS_MANUFACTURER_CODE >> 8) & 0xFF;
  zcl_frame[3] = 0x00;  // Sequence number
  zcl_frame[4] = ZCL_CMD_REPORT_ATTRIBUTES;
  zcl_frame[5] = CUSTOM_ATTR_FAN_SPEED & 0xFF;
  zcl_frame[6] = (CUSTOM_ATTR_FAN_SPEED >> 8) & 0xFF;
  zcl_frame[7] = ESP_ZB_ZCL_ATTR_TYPE_U8;
  zcl_frame[8] = speed;

  esp_zb_apsde_data_req_t req;
  memset(&req, 0, sizeof(req));
  req.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;  // Use binding table
  req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  req.cluster_id = VENTAIR_CUSTOM_CLUSTER_ID;
  req.src_endpoint = _endpoint;
  req.asdu_length = sizeof(zcl_frame);
  req.asdu = zcl_frame;
  req.tx_options = 0;
  req.radius = 0;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_aps_data_request(&req);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Log::error("Failed to report fan speed via APS: 0x%x", ret);
    return false;
  }
  Log::debug("Fan speed report sent via raw APS (speed=%d)", speed);
  return true;
}

void SkyfanZigbeeFanControl::reportAllAttributes() {
  reportFanMode();
  reportFanDirection();
  reportFanSpeed();
}

// Confirmed state management
void SkyfanZigbeeFanControl::confirmFanMode(ZigbeeFanMode mode) {
  confirmedFanMode = mode;
}

void SkyfanZigbeeFanControl::confirmFanDirection(uint8_t direction) {
  confirmedFanDirection = direction;
}

void SkyfanZigbeeFanControl::confirmFanSpeed(uint8_t speed) {
  confirmedFanSpeed = speed;
}

uint8_t SkyfanZigbeeFanControl::getConfirmedFanSpeed() const {
  return confirmedFanSpeed;
}

ZigbeeFanMode SkyfanZigbeeFanControl::getConfirmedFanMode() const {
  return confirmedFanMode;
}

uint8_t SkyfanZigbeeFanControl::getConfirmedFanDirection() const {
  return confirmedFanDirection;
}

// Rollback to confirmed state and report
void SkyfanZigbeeFanControl::rollback() {
  rollbackFanMode();
  rollbackFanDirection();
  rollbackFanSpeed();
}

void SkyfanZigbeeFanControl::rollbackFanMode() {
  bool setOk = setFanMode(confirmedFanMode);
  bool reportOk = reportFanMode();
  Log::info("Rolled back fan mode to %d (set=%s, report=%s)",
            confirmedFanMode, setOk ? "ok" : "FAIL", reportOk ? "ok" : "FAIL");
}

void SkyfanZigbeeFanControl::rollbackFanSpeed() {
  bool setOk = setFanSpeedAttribute(confirmedFanSpeed);
  bool reportOk = reportFanSpeed();
  Log::info("Rolled back fan speed to %d (set=%s, report=%s)",
            confirmedFanSpeed, setOk ? "ok" : "FAIL", reportOk ? "ok" : "FAIL");
}

void SkyfanZigbeeFanControl::rollbackFanDirection() {
  bool setOk = setFanDirection(confirmedFanDirection);
  bool reportOk = reportFanDirection();
  Log::info("Rolled back fan direction to %d (set=%s, report=%s)",
            confirmedFanDirection, setOk ? "ok" : "FAIL", reportOk ? "ok" : "FAIL");
}

// Handle MCU status updates — validates data, updates Zigbee attributes, confirms state, reports to coordinator
void SkyfanZigbeeFanControl::handleStatusUpdate(uint8_t dpid, uint32_t value) {
  switch (dpid) {
    case DP_FAN_SWITCH: {
      bool fanOn = (value != 0);

      if (fanOn) {
        // When turning on, preserve current mode if already set to a specific speed
        // (speed status may have arrived first and set LOW/MEDIUM/HIGH)
        if (_current_fan_mode == FAN_MODE_OFF) {
          setFanMode(FAN_MODE_ON);
        }
        // Otherwise keep the existing specific mode (LOW/MEDIUM/HIGH)
      } else {
        setFanMode(FAN_MODE_OFF);
      }

      Log::debug("Read Zigbee message 'endpoint: %d, cluster: 0x%04X, attribute: 0x%04X: %lu'",
                 _endpoint, ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL, ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, (uint32_t)_current_fan_mode);

      // Keep the exact speed attribute in step with power (0 when off, last speed when on)
      _fanPowerOn = fanOn;
      {
        // Turning on from stopped: use the remembered speed. Already holding a speed (for example one just
        // written from Zigbee): keep it, the speed report that follows confirms it, and Home Assistant
        // does not see a brief flick back to the old speed.
        uint8_t held = getFanSpeedAttribute();
        uint8_t exactSpeed = fanOn ? (held != 0 ? held : _lastMcuSpeed) : 0;
        bool speedChanged = (held != exactSpeed);
        setFanSpeedAttribute(exactSpeed);
        confirmFanSpeed(exactSpeed);
        if (speedChanged) reportFanSpeed();
        syncRemoteControl(fanOn, exactSpeed);
      }

      confirmFanMode(_current_fan_mode);
      Log::info("Fan switch set to %s (%d) by Skyfan", fanOn ? "ON" : "OFF", fanOn ? 1 : 0);
      break;
    }

    case DP_FAN_SPEED: {
      uint8_t speed = static_cast<uint8_t>(value);
      if (!isValidTuyaFanSpeed(speed)) {
        Log::error("Invalid fan speed status received: %d", speed);
        return;
      }

      // setFanSpeed handles the speed-to-mode mapping and attribute update
      if (!setFanSpeed(speed)) {
        Log::error("Failed to update Zigbee fan speed status: %d", speed);
      } else if (!_fanPowerOn) {
        // The fan reports its remembered speed even while stopped; the fan-mode attribute must still say off
        setFanMode(FAN_MODE_OFF);
      } else {
        Log::debug("Read Zigbee message 'endpoint: %d, cluster: 0x%04X, attribute: 0x%04X: %lu'",
                   _endpoint, ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL, ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, (uint32_t)_current_fan_mode);
      }

      // Also publish the exact speed (0-5) on the custom attribute so all five speeds are visible
      if (speed > 0) _lastMcuSpeed = speed;
      uint8_t exactSpeed = _fanPowerOn ? speed : 0;
      bool speedChanged = (getFanSpeedAttribute() != exactSpeed);
      setFanSpeedAttribute(exactSpeed);
      confirmFanSpeed(exactSpeed);
      if (speedChanged) reportFanSpeed();
      syncRemoteControl(_fanPowerOn, exactSpeed);

      confirmFanMode(_current_fan_mode);
      Log::info("Fan speed set to %d by Skyfan", speed);
      break;
    }

    case DP_FAN_MODE: {
      uint8_t mode = static_cast<uint8_t>(value);
      if (mode <= static_cast<uint8_t>(TuyaFanMode::SLEEP)) {
        const char* modeName = (mode == static_cast<uint8_t>(TuyaFanMode::NORMAL)) ? "NORMAL" :
                               (mode == static_cast<uint8_t>(TuyaFanMode::ECO)) ? "ECO" : "SLEEP";
        Log::info("Fan MCU mode set to %s (%d) by Skyfan", modeName, mode);
      } else {
        Log::error("Invalid fan mode status received: %d", mode);
      }
      break;
    }

    case DP_FAN_DIRECTION: {
      uint8_t direction = static_cast<uint8_t>(value);
      if (direction > static_cast<uint8_t>(FanDirection::REVERSE)) {
        Log::error("Invalid fan direction status received: %d", direction);
        return;
      }

      if (!setFanDirection(direction)) {
        Log::error("Failed to update Zigbee fan direction status: %d", direction);
      } else {
        Log::debug("Read Zigbee message 'endpoint: %d, cluster: 0x%04X, attribute: 0x%04X: %lu'",
                   _endpoint, VENTAIR_CUSTOM_CLUSTER_ID, CUSTOM_ATTR_FAN_DIRECTION, (uint32_t)direction);
      }

      confirmFanDirection(direction);
      Log::info("Fan direction set to %s (%d) by Skyfan",
        (direction == static_cast<uint8_t>(FanDirection::FORWARD)) ? "FORWARD" : "REVERSE", direction);
      break;
    }

    default:
      Log::error("Unknown fan status update - DPID: %d, Value: %lu", dpid, value);
      break;
  }
}