/*
 * Power-on restore - remembers the fan and light state and puts it back after a power cut
 * Copyright (C) 2025 Rhys Frederick at Front Left Speaker
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3, as
 * published by the Free Software Foundation.
 */

#ifndef POWER_ON_RESTORE_H
#define POWER_ON_RESTORE_H

#include <Arduino.h>
#include <Preferences.h>
#include "SkyfanConfig.h"
#include "TuyaProtocol.h"
#include "Logger.h"

class PowerOnRestore {
private:
  static constexpr const char* NAMESPACE = "skyfanpo";
  static constexpr const char* KEY = "snap";
  static constexpr uint8_t SNAPSHOT_VERSION = 1;

  // Datapoints seen at least once (bit per group member), so a group is only saved once all of it is known
  static constexpr uint8_t SEEN_LIGHT_ON = 1 << 0, SEEN_LIGHT_BRIGHT = 1 << 1, SEEN_LIGHT_TEMP = 1 << 2;
  static constexpr uint8_t SEEN_FAN_ON = 1 << 3, SEEN_FAN_SPEED = 1 << 4, SEEN_FAN_DIR = 1 << 5;
  static constexpr uint8_t SEEN_LIGHT_ALL = SEEN_LIGHT_ON | SEEN_LIGHT_BRIGHT | SEEN_LIGHT_TEMP;
  static constexpr uint8_t SEEN_FAN_ALL = SEEN_FAN_ON | SEEN_FAN_SPEED | SEEN_FAN_DIR;

  static constexpr const char* KEY_LIGHT_MODE = "lightMode";
  static constexpr const char* KEY_FAN_MODE = "fanMode";

  Preferences prefs;
  uint8_t _lightMode = POWER_ON_LIGHT_MODE;   // POWER_ON_DEFAULT / PREVIOUS / ON / OFF
  uint8_t _fanMode = POWER_ON_FAN_MODE;
  PowerSnapshot _saved = {};     // what is in flash
  PowerSnapshot _current = {};   // what we last saw the fan do
  uint8_t _seen = 0;
  bool _dirty = false;
  unsigned long _changedAt = 0;

  void load() {
    _saved = {};
    _lightMode = POWER_ON_LIGHT_MODE;
    _fanMode = POWER_ON_FAN_MODE;
    if (prefs.begin(NAMESPACE, true)) {
      if (prefs.getBytesLength(KEY) == sizeof(PowerSnapshot)) {
        prefs.getBytes(KEY, &_saved, sizeof(PowerSnapshot));
      }
      if (prefs.isKey(KEY_LIGHT_MODE)) {
        uint8_t m = prefs.getUChar(KEY_LIGHT_MODE, _lightMode);
        if (m <= POWER_ON_OFF) _lightMode = m;
      }
      if (prefs.isKey(KEY_FAN_MODE)) {
        uint8_t m = prefs.getUChar(KEY_FAN_MODE, _fanMode);
        if (m <= POWER_ON_OFF) _fanMode = m;
      }
      prefs.end();
    }
    if (_saved.version != SNAPSHOT_VERSION) {
      _saved = {};
    }
    _current = _saved;
    _seen = 0;
    if (_saved.valid & 1) _seen |= SEEN_LIGHT_ALL;
    if (_saved.valid & 2) _seen |= SEEN_FAN_ALL;
  }

  void markChanged() {
    _dirty = true;
    _changedAt = millis();
  }

  void setField(uint8_t& field, uint8_t value, uint8_t seenBit) {
    _seen |= seenBit;
    if (field != value) {
      field = value;
      markChanged();
    }
  }

  // Send one datapoint to the fan's controller and give it a moment to answer before the next one
  void send(TuyaProtocol& tuya, uint8_t dpid, uint8_t type, uint32_t value) {
    uint8_t data[8];
    uint16_t n = 0;
    data[n++] = dpid;
    data[n++] = type;
    data[n++] = 0x00;
    if (type == DP_TYPE_BOOL) {
      data[n++] = DP_BOOL_PAYLOAD_LENGTH;
      data[n++] = value ? 0x01 : 0x00;
    } else if (type == DP_TYPE_ENUM) {
      data[n++] = DP_ENUM_PAYLOAD_LENGTH;
      data[n++] = value & 0xFF;
    } else {  // DP_TYPE_VALUE
      data[n++] = DP_VALUE_PAYLOAD_LENGTH;
      data[n++] = (value >> 24) & 0xFF;
      data[n++] = (value >> 16) & 0xFF;
      data[n++] = (value >> 8) & 0xFF;
      data[n++] = value & 0xFF;
    }
    tuya.sendCommand(TUYA_CMD_SEND_COMMAND, data, n);
    unsigned long start = millis();
    while (millis() - start < 150) {
      tuya.processResponse();
      delay(5);
    }
  }

public:
  void begin() { load(); }

  // Power-on behaviour settings (changed from Zigbee2MQTT, remembered across power cuts)
  uint8_t getLightMode() const { return _lightMode; }
  uint8_t getFanMode() const { return _fanMode; }
  void setLightMode(uint8_t mode) {
    if (mode > POWER_ON_OFF) return;
    _lightMode = mode;
    if (prefs.begin(NAMESPACE, false)) {
      prefs.putUChar(KEY_LIGHT_MODE, mode);
      prefs.end();
    }
  }
  void setFanMode(uint8_t mode) {
    if (mode > POWER_ON_OFF) return;
    _fanMode = mode;
    if (prefs.begin(NAMESPACE, false)) {
      prefs.putUChar(KEY_FAN_MODE, mode);
      prefs.end();
    }
  }

  // Forget everything (used with a factory reset)
  void clear() {
    if (prefs.begin(NAMESPACE, false)) {
      prefs.clear();
      prefs.end();
    }
    _saved = {};
    _current = {};
    _seen = 0;
    _dirty = false;
    _lightMode = POWER_ON_LIGHT_MODE;
    _fanMode = POWER_ON_FAN_MODE;
  }

  // Call for every status report from the fan's controller
  void track(uint8_t dpid, uint32_t value) {
    switch (dpid) {
      case DP_LIGHT_SWITCH:
        setField(_current.lightOn, value ? 1 : 0, SEEN_LIGHT_ON);
        break;
      case DP_LIGHT_DIMMER:
        if (value >= 1 && value <= TUYA_BRIGHTNESS_MAX) setField(_current.lightBrightness, (uint8_t)value, SEEN_LIGHT_BRIGHT);
        break;
      case DP_LIGHT_COLOUR_TEMP:
        if (value <= 2) setField(_current.lightTemp, (uint8_t)value, SEEN_LIGHT_TEMP);
        break;
      case DP_FAN_SWITCH:
        setField(_current.fanOn, value ? 1 : 0, SEEN_FAN_ON);
        break;
      case DP_FAN_SPEED:
        if (value >= 1 && value <= TUYA_FAN_SPEED_MAX) setField(_current.fanSpeed, (uint8_t)value, SEEN_FAN_SPEED);
        break;
      case DP_FAN_DIRECTION:
        if (value <= 1) setField(_current.fanDirection, (uint8_t)value, SEEN_FAN_DIR);
        break;
      default:
        break;
    }
  }

  // Call from loop(): writes a change to flash once things have settled
  void update() {
    if (!_dirty || (millis() - _changedAt) < POWER_ON_SAVE_DELAY_MS) {
      return;
    }
    _dirty = false;
    PowerSnapshot snap = _current;
    snap.version = SNAPSHOT_VERSION;
    snap.valid = ((_seen & SEEN_LIGHT_ALL) == SEEN_LIGHT_ALL ? 1 : 0) | ((_seen & SEEN_FAN_ALL) == SEEN_FAN_ALL ? 2 : 0);
    if (snap.valid == 0 || memcmp(&snap, &_saved, sizeof(PowerSnapshot)) == 0) {
      return;
    }
    if (prefs.begin(NAMESPACE, false)) {
      prefs.putBytes(KEY, &snap, sizeof(PowerSnapshot));
      prefs.end();
      _saved = snap;
      Log::info("Power-on snapshot saved");
    } else {
      Log::error("Could not save the power-on snapshot");
    }
  }

  // Call once, after the fan's controller is connected, on a genuine power-on boot
  void restore(TuyaProtocol& tuya) {
    bool haveSnapshot = (_saved.version == SNAPSHOT_VERSION);
    bool lightFromSnapshot = (_lightMode == POWER_ON_PREVIOUS || _lightMode == POWER_ON_ON) && haveSnapshot && (_saved.valid & 1);
    bool fanFromSnapshot = (_fanMode == POWER_ON_PREVIOUS || _fanMode == POWER_ON_ON) && haveSnapshot && (_saved.valid & 2);
    bool lightOff = (_lightMode == POWER_ON_OFF);
    bool fanOff = (_fanMode == POWER_ON_OFF);

    if (!lightFromSnapshot && !fanFromSnapshot && !lightOff && !fanOff) {
      Log::info("Power-on restore: nothing to do (light mode %d, fan mode %d)", _lightMode, _fanMode);
      return;
    }

    // Give the fan's controller time to finish starting up
    while (millis() < POWER_ON_RESTORE_DELAY_MS) {
      tuya.processResponse();
      delay(10);
    }

    // Light
    if (lightFromSnapshot) {
      // Brightness and colour while the light is on (as the controller leaves it), then the on/off state last
      send(tuya, DP_LIGHT_COLOUR_TEMP, DP_TYPE_ENUM, _saved.lightTemp);
      send(tuya, DP_LIGHT_DIMMER, DP_TYPE_VALUE, _saved.lightBrightness);
      bool on = (_lightMode == POWER_ON_ON) ? true : (_saved.lightOn != 0);
      send(tuya, DP_LIGHT_SWITCH, DP_TYPE_BOOL, on ? 1 : 0);
      Log::info("Power-on restore: light %s, brightness %d, colour %d", on ? "on" : "off", _saved.lightBrightness, _saved.lightTemp);
    } else if (lightOff) {
      send(tuya, DP_LIGHT_SWITCH, DP_TYPE_BOOL, 0);
      Log::info("Power-on restore: light off");
    }

    // Fan
    if (fanFromSnapshot) {
      send(tuya, DP_FAN_DIRECTION, DP_TYPE_ENUM, _saved.fanDirection);
      bool on = (_fanMode == POWER_ON_ON) ? true : (_saved.fanOn != 0);
      if (on) {
        send(tuya, DP_FAN_SWITCH, DP_TYPE_BOOL, 1);
        send(tuya, DP_FAN_SPEED, DP_TYPE_VALUE, _saved.fanSpeed);
      }
      Log::info("Power-on restore: fan %s, speed %d, direction %d", on ? "on" : "stopped", _saved.fanSpeed, _saved.fanDirection);
    } else if (fanOff) {
      send(tuya, DP_FAN_SWITCH, DP_TYPE_BOOL, 0);
      Log::info("Power-on restore: fan stopped");
    }
  }
};

#endif  // POWER_ON_RESTORE_H
