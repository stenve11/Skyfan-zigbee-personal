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

#ifndef SKYFAN_ZIGBEE_H
#define SKYFAN_ZIGBEE_H

#include <Arduino.h>
#include "Zigbee.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "aps/esp_zigbee_aps.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "SkyfanConfig.h"

// Custom fan control class that replicates ZigbeeFanControl functionality
// but adds raw APS reporting (to bypass access flag checks) and custom cluster support
class SkyfanZigbeeFanControl : public ZigbeeEP {
private:
  void (*fanDirectionCallback)(uint8_t direction) = nullptr;
  void (*fanModeCallback)(ZigbeeFanMode mode) = nullptr;
  void (*fanSpeedCallback)(uint8_t speed) = nullptr;
  void (*powerOnCallback)(uint8_t which, uint8_t value) = nullptr;  // which: 0 = light, 1 = fan
  void (*remotePowerCallback)(bool on) = nullptr;       // On/Off command from a bound switch, remote or group
  void (*remoteLevelCallback)(uint8_t level) = nullptr; // Level Control change (0-254) from a bound dial/remote
  uint8_t _initialPowerOnLight = 0;
  uint8_t _initialPowerOnFan = 0;
  esp_zb_attribute_list_t *customCluster = nullptr;
  bool customClusterRegistered = false;

  // Fan state (replicated from ZigbeeFanControl since we don't inherit from it)
  ZigbeeFanMode _current_fan_mode = FAN_MODE_OFF;
  ZigbeeFanModeSequence _current_fan_mode_sequence = FAN_MODE_SEQUENCE_LOW_MED_HIGH;

  // Confirmed state (last value acknowledged by MCU)
  ZigbeeFanMode confirmedFanMode = FAN_MODE_OFF;
  uint8_t confirmedFanDirection = static_cast<uint8_t>(FanDirection::FORWARD);
  uint8_t confirmedFanSpeed = 0;
  bool _fanPowerOn = false;       // last power state reported by the MCU
  uint8_t _lastMcuSpeed = 1;      // last non-zero speed reported by the MCU

  // On/Off and Level Control attributes on the fan endpoint, for bound switches, remotes and groups.
  // The Zigbee stack changes them itself when a command arrives (a Toggle works from the stored value),
  // so they are kept in step with the fan's real state. Local copies avoid reading them back.
  bool _remoteOn = false;
  uint8_t _remoteLevel = 255;

public:
  SkyfanZigbeeFanControl(uint8_t endpoint);
  
  // Destructor to clean up allocated resources
  ~SkyfanZigbeeFanControl();
  
  // Set callback for fan direction changes from Zigbee
  void onFanDirectionChange(void (*callback)(uint8_t direction));

  // Set callback for fan mode changes from Zigbee
  void onFanModeChange(void (*callback)(ZigbeeFanMode mode));

  // Set callback for exact fan speed (0-5) changes from Zigbee
  void onFanSpeedChange(void (*callback)(uint8_t speed));

  // Power-on behaviour settings (0 default, 1 previous, 2 on, 3 off): starting values, and a callback for changes from Zigbee
  void setInitialPowerOn(uint8_t light, uint8_t fan);   // call before createCustomCluster()
  void onPowerOnChange(void (*callback)(uint8_t which, uint8_t value));

  // Bound-controller support: callbacks for On/Off commands and Level Control changes on the fan endpoint
  void onRemotePowerChange(void (*callback)(bool on));
  void onRemoteLevelChange(void (*callback)(uint8_t level));

  // Last non-zero speed the fan reported; what an On command resumes at
  uint8_t getLastSpeed() const { return _lastMcuSpeed; }

  // Mirror the fan's real state into the On/Off and Level attributes (call after an MCU status update)
  void syncRemoteControl(bool on, uint8_t speed);

  // Get current fan mode
  ZigbeeFanMode getFanMode() const { return _current_fan_mode; }

  // Get current fan mode sequence
  ZigbeeFanModeSequence getFanModeSequence() const { return _current_fan_mode_sequence; }

  // Set the fan mode sequence value
  bool setFanModeSequence(ZigbeeFanModeSequence sequence);

  // Public setter methods for bidirectional status updates
  bool setFanMode(ZigbeeFanMode mode);
  bool setFanState(bool on);
  bool setFanSpeed(uint8_t speed);
  
  // Custom cluster attribute methods for fan direction
  bool setFanDirection(uint8_t direction);
  uint8_t getFanDirection() const;

  // Custom cluster attribute methods for exact fan speed (0 = off, 1-5)
  bool setFanSpeedAttribute(uint8_t speed);
  uint8_t getFanSpeedAttribute() const;
  
  // Create and register manufacturer-specific cluster for fan direction
  bool createCustomCluster();
  
  // Handle custom cluster attribute changes (called internally by zbAttributeSet)
  void handleCustomClusterAttributeChange(uint16_t cluster_id, uint16_t attr_id, uint8_t *data);

  // Override zbAttributeSet to handle custom cluster writes
  void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override;

  // Get custom cluster ID for external reference
  uint16_t getCustomClusterId() const;
  
  // Check if custom cluster is registered
  bool isCustomClusterRegistered() const;

  // Clean up custom cluster resources
  void cleanupCustomCluster();

  // Report attribute values to coordinator
  bool reportFanMode();
  bool reportFanDirection();
  bool reportFanSpeed();
  void reportAllAttributes();

  // Confirmed state management (called when MCU confirms via status report)
  void confirmFanMode(ZigbeeFanMode mode);
  void confirmFanDirection(uint8_t direction);
  ZigbeeFanMode getConfirmedFanMode() const;
  uint8_t getConfirmedFanDirection() const;
  void confirmFanSpeed(uint8_t speed);
  uint8_t getConfirmedFanSpeed() const;

  // Rollback to confirmed state and report to coordinator
  void rollback();          // Rolls back all attributes (fan mode + direction)
  void rollbackFanMode();   // Rolls back fan mode only
  void rollbackFanDirection();  // Rolls back fan direction only
  void rollbackFanSpeed();      // Rolls back exact fan speed only

  // Handle MCU status updates (validates, updates Zigbee attributes, confirms state, reports)
  void handleStatusUpdate(uint8_t dpid, uint32_t value);

};

#endif // SKYFAN_ZIGBEE_H