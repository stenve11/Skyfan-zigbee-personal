import {access, presets} from "zigbee-herdsman-converters/lib/exposes";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";
import * as reporting from "zigbee-herdsman-converters/lib/reporting";
import {Zcl} from "zigbee-herdsman";

const e = presets;

// Front Left Speaker manufacturer-specific constants
const FLS_MANUFACTURER_CODE = 0x1818;
const CUSTOM_CLUSTER_ID = 0xFC00;
const FAN_DIRECTION_ATTR_ID = 0x0001;
const FAN_SPEED_ATTR_ID = 0x0002;
const POWER_ON_LIGHT_ATTR_ID = 0x0003;
const POWER_ON_FAN_ATTR_ID = 0x0004;

// Power-on behaviour choices, in the order the firmware numbers them (0..3)
const POWER_ON_VALUES = ["default", "previous", "on", "off"];

const manufacturerOptions = {manufacturerCode: FLS_MANUFACTURER_CODE};

// Custom fromZigbee converters for fan clusters.
// The fan's state comes from the exact speed attribute (0 = off, 1-5), so reports on the standard
// hvacFanCtrl cluster (which only has low/medium/high) are deliberately ignored.
const fzLocal = {
    fan_speed: {
        cluster: "fanExtensions",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg, publish, options, meta) => {
            if (Object.hasOwn(msg.data, "fanSpeed")) {
                const speed = msg.data.fanSpeed;
                return {
                    fan_speed: speed,
                    fan_mode: speed === 0 ? "off" : String(speed),
                    fan_state: speed === 0 ? "OFF" : "ON",
                };
            }
        },
    },
    fan_direction: {
        cluster: "fanExtensions",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg, publish, options, meta) => {
            if (Object.hasOwn(msg.data, "fanDirection")) {
                const direction = msg.data.fanDirection === 0 ? "forward" : "reverse";
                return {fan_direction: direction};
            }
        },
    },
    power_on: {
        cluster: "fanExtensions",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            if (Object.hasOwn(msg.data, "powerOnLight")) {
                result.power_on_light = POWER_ON_VALUES[msg.data.powerOnLight] ?? "default";
            }
            if (Object.hasOwn(msg.data, "powerOnFan")) {
                result.power_on_fan = POWER_ON_VALUES[msg.data.powerOnFan] ?? "default";
            }
            if (Object.keys(result).length > 0) {
                return result;
            }
        },
    },
};

const tzLocal = {
    // Handles fan_mode ("off", "1".."5", "on") and fan_state ("ON"/"OFF", used by Home Assistant)
    fan_mode: {
        key: ["fan_mode", "fan_state"],
        convertSet: async (entity, key, value, meta) => {
            let target = String(value);
            if (key === "fan_state") {
                target = target.toUpperCase() === "OFF" ? "off" : "on";
            }
            if (target === "off" || target === "on") {
                await entity.write("hvacFanCtrl", {fanMode: target === "off" ? 0 : 4});
                return target === "off" ? {state: {fan_mode: "off", fan_state: "OFF", fan_speed: 0}} : {state: {fan_state: "ON"}};
            }
            const speed = Number(target);
            if (!Number.isInteger(speed) || speed < 1 || speed > 5) {
                throw new Error(`Unsupported fan mode '${value}'`);
            }
            await entity.write("fanExtensions", {fanSpeed: speed}, manufacturerOptions);
            return {state: {fan_mode: String(speed), fan_state: "ON", fan_speed: speed}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read("fanExtensions", ["fanSpeed"], manufacturerOptions);
        },
    },
    fan_speed: {
        key: ["fan_speed"],
        convertSet: async (entity, key, value, meta) => {
            const speed = Math.max(0, Math.min(5, Math.round(Number(value))));
            if (speed === 0) {
                await entity.write("hvacFanCtrl", {fanMode: 0});
            } else {
                await entity.write("fanExtensions", {fanSpeed: speed}, manufacturerOptions);
            }
            return {state: {fan_speed: speed, fan_mode: speed === 0 ? "off" : String(speed), fan_state: speed === 0 ? "OFF" : "ON"}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read("fanExtensions", ["fanSpeed"], manufacturerOptions);
        },
    },
    fan_direction: {
        key: ["fan_direction"],
        convertSet: async (entity, key, value, meta) => {
            const directionValue = value === "forward" ? 0 : 1;
            await entity.write("fanExtensions", {fanDirection: directionValue}, manufacturerOptions);
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read("fanExtensions", ["fanDirection"], manufacturerOptions);
        },
    },
    power_on_light: {
        key: ["power_on_light"],
        convertSet: async (entity, key, value, meta) => {
            const index = POWER_ON_VALUES.indexOf(value);
            if (index < 0) {
                throw new Error(`Unsupported power-on setting '${value}'`);
            }
            await entity.write("fanExtensions", {powerOnLight: index}, manufacturerOptions);
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read("fanExtensions", ["powerOnLight"], manufacturerOptions);
        },
    },
    power_on_fan: {
        key: ["power_on_fan"],
        convertSet: async (entity, key, value, meta) => {
            const index = POWER_ON_VALUES.indexOf(value);
            if (index < 0) {
                throw new Error(`Unsupported power-on setting '${value}'`);
            }
            await entity.write("fanExtensions", {powerOnFan: index}, manufacturerOptions);
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read("fanExtensions", ["powerOnFan"], manufacturerOptions);
        },
    },
};

// Shared custom cluster extension for fan direction
// Zigbee2MQTT needs the cluster and every attribute to carry their own "name" as well as being the object key;
// without them incoming reports show up as cluster 'undefined' with no converter, and writes are refused.
const fanExtensionsCluster = m.deviceAddCustomCluster("fanExtensions", {
    name: "fanExtensions",
    ID: CUSTOM_CLUSTER_ID,
    manufacturerCode: FLS_MANUFACTURER_CODE,
    attributes: {
        fanDirection: {
            name: "fanDirection",
            ID: FAN_DIRECTION_ATTR_ID,
            type: Zcl.DataType.ENUM8,
            write: true,
        },
        fanSpeed: {
            name: "fanSpeed",
            ID: FAN_SPEED_ATTR_ID,
            type: Zcl.DataType.UINT8,
            write: true,
        },
        powerOnLight: {
            name: "powerOnLight",
            ID: POWER_ON_LIGHT_ATTR_ID,
            type: Zcl.DataType.ENUM8,
            write: true,
        },
        powerOnFan: {
            name: "powerOnFan",
            ID: POWER_ON_FAN_ATTR_ID,
            type: Zcl.DataType.ENUM8,
            write: true,
        },
    },
    commands: {},
    commandsResponse: {},
});

// Home Assistant builds its fan entity from this expose, so the feature name stays "mode".
// Only the label and description shown in the Zigbee2MQTT UI are renamed.
const fanExpose = e.fan().withModes(["off", "1", "2", "3", "4", "5", "on"]);
fanExpose.features.find((f) => f.name === "mode").withLabel("Fan speed").withDescription("Fan speed: off, 1 to 5, or on");

const fanExposes = [
    fanExpose,
    e.enum("fan_direction", access.ALL, ["forward", "reverse"]).withDescription("Fan rotation direction"),
];

const powerOnLightExpose = e
    .enum("power_on_light", access.ALL, POWER_ON_VALUES)
    .withLabel("Power-on light")
    .withDescription(
        "What the light does when power returns after a power cut. previous: as it was before (including staying off); on: on at the previous brightness and colour; off: off; default: the fan's own power-up state (light on at its default brightness and colour).",
    )
    .withCategory("config");
const powerOnFanExpose = e
    .enum("power_on_fan", access.ALL, POWER_ON_VALUES)
    .withLabel("Power-on fan")
    .withDescription(
        "What the fan does when power returns after a power cut. default: stays stopped; previous: back to its previous speed and direction (stopped if it was off); on: runs at its previous speed; off: stopped.",
    )
    .withCategory("config");

const fanFromZigbee = [fzLocal.fan_speed, fzLocal.fan_direction, fzLocal.power_on];
const fanToZigbee = [tzLocal.fan_mode, tzLocal.fan_speed, tzLocal.fan_direction, tzLocal.power_on_light, tzLocal.power_on_fan];

async function configureFan(device, coordinatorEndpoint) {
    const fanEndpoint = device.getEndpoint(1);

    // fanExtensions carries the fan speed and direction reports, so bind it first
    await reporting.bind(fanEndpoint, coordinatorEndpoint, ["fanExtensions"]);

    try {
        // hvacFanCtrl is only used for writes (on/off); a failed bind here must not stop the rest of the setup
        await reporting.bind(fanEndpoint, coordinatorEndpoint, ["hvacFanCtrl"]);
        await reporting.fanMode(fanEndpoint);
    } catch {
        // Fan mode reporting not supported, using binding only
    }

    try {
        await fanEndpoint.configureReporting("fanExtensions", [
            {
                attribute: "fanDirection",
                minimumReportInterval: 1,
                maximumReportInterval: 3600,
                reportableChange: 1,
            },
        ]);
    } catch {
        // Fan direction reporting not supported, using binding only
    }

    try {
        await fanEndpoint.configureReporting("fanExtensions", [
            {
                attribute: "fanSpeed",
                minimumReportInterval: 1,
                maximumReportInterval: 3600,
                reportableChange: 1,
            },
        ]);
    } catch {
        // Fan speed reporting not supported, using binding only
    }

    try {
        // Read the current values once, so speed, direction and the power-on settings are known straight after pairing
        await fanEndpoint.read("fanExtensions", ["fanSpeed", "fanDirection", "powerOnLight", "powerOnFan"], manufacturerOptions);
    } catch {
        // Initial read failed; values will appear on the next change
    }
}

// The fan's light has three colour temperature settings, not a continuous range. Zigbee2MQTT's stock presets
// (coolest 154, cool 250, warmest 333) don't match the middle setting (238 mired), so replace them.
// The colour-temperature-at-power-on, power-on behaviour and effect controls are switched off because the
// firmware does not implement them.
const lightExtend = m.light({
    colorTemp: {range: [154, 333], startup: false},
    endpointNames: ["light"],
    effect: false,
    powerOnBehavior: false,
});
for (const expose of lightExtend.exposes) {
    const colorTemp = expose.features?.find((f) => f.name === "color_temp");
    if (colorTemp) {
        colorTemp.presets = [
            {name: "cool", value: 154, description: "Cool setting (about 6500 K)"},
            {name: "neutral", value: 238, description: "Neutral setting (about 4200 K)"},
            {name: "warm", value: 333, description: "Warm setting (about 3000 K)"},
        ];
    }
}

// Home Assistant discovery tweaks for the fan entity, applied automatically to every Skyfan.
//  - Direction: adds the direction control to the fan entity (Zigbee2MQTT does not do this itself).
//  - Smooth slider: Home Assistant shows buttons for fans with 5 speeds or fewer and a slider for more.
//    Reporting a 1-100 range makes it show a slider; the templates snap the slider to the five real speeds.
//    Set HA_FAN_SMOOTH_SLIDER to false if you prefer Home Assistant's 1-2-3-4-5 buttons.
const HA_FAN_SMOOTH_SLIDER = true;

function overrideFanDiscovery(payload) {
    // Only touch the fan entity; every other entity of the device is left alone
    if (!payload.percentage_command_topic?.endsWith("/set/fan_mode")) {
        return;
    }

    // Direction control (topics derive from the entity's own topics, so renaming a device just works)
    payload.direction_command_topic = `${payload.percentage_command_topic.slice(0, -"fan_mode".length)}fan_direction`;
    payload.direction_state_topic = payload.percentage_state_topic ?? payload.state_topic;
    payload.direction_value_template = "{{ value_json.fan_direction }}";

    if (HA_FAN_SMOOTH_SLIDER) {
        payload.speed_range_min = 1;
        payload.speed_range_max = 100;
        payload.percentage_value_template =
            "{{ {'off':0,'1':20,'2':40,'3':60,'4':80,'5':100}[value_json.fan_mode] | default('None') }}";
        payload.percentage_command_template = "{{ 'off' if (value | int) == 0 else (((value | int) + 19) // 20) | string }}";
    }
}

// OTA updates from GitHub releases
// User must add the following to their Zigbee2MQTT configuration.yaml:
// ota:
//   zigbee_ota_override_index_location: https://raw.githubusercontent.com/rhysfred/skyfan-zigbee/main/zigbee2mqtt/ota-index.json

export default [
    {
        zigbeeModel: ["Ventair Skyfan/Light ZB Adaptor"],
        model: "Ventair Skyfan/Light ZB Adaptor",
        vendor: "Front Left Speaker",
        description: "Ventair Skyfan ceiling fan with integrated light controller",
        extend: [
            fanExtensionsCluster,
            m.deviceEndpoints({endpoints: {fan: 1, light: 2}}),
            lightExtend,
        ],
        exposes: [...fanExposes, powerOnLightExpose, powerOnFanExpose],
        fromZigbee: fanFromZigbee,
        toZigbee: fanToZigbee,
        configure: async (device, coordinatorEndpoint) => {
            await configureFan(device, coordinatorEndpoint);

            const lightEndpoint = device.getEndpoint(2);
            await reporting.bind(lightEndpoint, coordinatorEndpoint, ["genOnOff", "genLevelCtrl", "lightingColorCtrl"]);

            try {
                await lightEndpoint.configureReporting("genOnOff", [
                    {attribute: "onOff", minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 1},
                ]);
            } catch {
                // genOnOff reporting not supported, using binding only
            }

            try {
                await lightEndpoint.configureReporting("genLevelCtrl", [
                    {attribute: "currentLevel", minimumReportInterval: 1, maximumReportInterval: 3600, reportableChange: 1},
                ]);
            } catch {
                // genLevelCtrl reporting not supported, using binding only
            }

            try {
                await lightEndpoint.configureReporting("lightingColorCtrl", [
                    {attribute: "colorTemperature", minimumReportInterval: 1, maximumReportInterval: 3600, reportableChange: 1},
                ]);
            } catch {
                // lightingColorCtrl reporting not supported, using binding only
            }
        },
        ota: true,
        meta: {overrideHaDiscoveryPayload: overrideFanDiscovery},
    },
    {
        zigbeeModel: ["Ventair Skyfan ZB Adaptor"],
        model: "Ventair Skyfan ZB Adaptor",
        vendor: "Front Left Speaker",
        description: "Ventair Skyfan ceiling fan controller (fan only)",
        extend: [
            fanExtensionsCluster,
            m.deviceEndpoints({endpoints: {fan: 1}}),
        ],
        exposes: [...fanExposes, powerOnFanExpose],
        fromZigbee: fanFromZigbee,
        toZigbee: fanToZigbee,
        configure: async (device, coordinatorEndpoint) => {
            await configureFan(device, coordinatorEndpoint);
        },
        ota: true,
        meta: {overrideHaDiscoveryPayload: overrideFanDiscovery},
    },
];
