"""ESP Nest: display pipeline, UI and device logic, hosted inside ESPHome.

ESPHome owns the platform (WiFi, native API, OTA, safe mode, web server,
diagnostic entities). This component owns everything device-specific: the
ST7701S panel, LVGL 9.6 and the UI in its own `nest_ui` FreeRTOS task, the
knob (encoder + button), the backlight and the LED ring (`nest_led` task). See
docs/esphome-migration-plan.md for the architecture.

Threading: LVGL / LEDC only on `nest_ui`, ESPHome APIs only on loopTask.
"""

import hashlib
from pathlib import Path
import re
from typing import NamedTuple

import esphome.codegen as cg
from esphome.components import esp32, mqtt, number, switch
from esphome.components.esp32 import CONF_PARTITIONS
from esphome.components.esp32.const import VARIANT_ESP32S3
import esphome.config_validation as cv
from esphome.const import (
    CONF_BROKER,
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_NAME,
    CONF_PROJECT,
    CONF_RESTORE_VALUE,
    CONF_VERSION,
    ENTITY_CATEGORY_CONFIG,
    PLATFORM_ESP32,
)
from esphome.core import CORE, EsphomeError
import esphome.final_validate as fv

DEPENDENCIES = ["esp32", "psram", "mqtt", "wifi"]
AUTO_LOAD = ["number", "switch"]

CONF_DISPLAY = "display"
CONF_HW_MIRROR_X = "hw_mirror_x"
CONF_ESPHOME = "esphome"
CONF_BLINDS = "blinds"
CONF_MQTT_ID = "mqtt_id"
CONF_TOPICS = "topics"
CONF_LIGHTS = "lights"
CONF_FAN = "fan"
CONF_STEREO_VOLUME = "stereo_volume"
CONF_PREFIX = "prefix"
CONF_TOPIC_ID = "topic_id"
CONF_GROUP_A = "group_a"
CONF_GROUP_B = "group_b"
CONF_LABEL = "label"
CONF_SCREEN_BRIGHTNESS = "screen_brightness"
CONF_DIM_BRIGHTNESS = "dim_brightness"
CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_LED_RING = "led_ring"

esp_nest_ns = cg.esphome_ns.namespace("esp_nest")
EspNest = esp_nest_ns.class_("EspNest", cg.Component)
SettingId = esp_nest_ns.enum("SettingId", is_class=True)
SettingNumber = esp_nest_ns.class_("SettingNumber", number.Number)
LedRingSwitch = esp_nest_ns.class_("LedRingSwitch", switch.Switch)

# ---------------------------------------------------------------------------
# Repository anchors
# ---------------------------------------------------------------------------

# esphome/esp-nest-components/esp_nest/__init__.py -> repository root. Holds
# both in the working tree and in a remote-package / external-component
# clone, which contains the whole repository.
REPO_ROOT = Path(__file__).resolve().parents[3]
VERSION_FILE = REPO_ROOT / "version.txt"

# sha256 of the repository's partitions.csv (LF line endings). The device
# keeps the partition table written by the native firmware's USB flash; OTA
# cannot change it, so every ESPHome image must be built against exactly this
# table. A constant (not the file in the repo) is the correct reference: it
# is what is on the device. The guard hashes the CSV with CRLF normalised to
# LF (line endings do not change the generated table; .gitattributes also
# pins the file to LF); the byte-exact check is the generated
# partition-table.bin sha256 gate in scripts/check.sh.
PARTITIONS_CSV_SHA256 = (
    "0a9ba0537d395e1c42f7540a69a1d3b0af9cbc854c0a063b834b54452961528a"
)

# ---------------------------------------------------------------------------
# LVGL
# ---------------------------------------------------------------------------

LVGL_VERSION = "9.6.0"

# Defines LV_ASSERT_HANDLER; LVGL includes it via CONFIG_LV_ASSERT_CUSTOM_INCLUDE.
LV_ASSERT_HEADER = "nest_lv_assert.h"
# The C function LV_ASSERT_HANDLER calls (nest_lv_assert.c).
LV_ASSERT_HANDLER_SYMBOL = "nest_lv_assert_failed"

# The IDF builtins this component's code uses (ESPHome excludes several of
# them by default, E/components/esp32/__init__.py DEFAULT_EXCLUDED_...).
IDF_BUILTIN_COMPONENTS = (
    "esp_lcd",  # ST7701S RGB panel (nest_panel)
    "esp_driver_spi",  # 9-bit SPI init sequence (nest_panel)
    "esp_driver_gpio",  # button (nest_input)
    "esp_driver_ledc",  # backlight PWM + fade (nest_backlight)
    "esp_driver_pcnt",  # rotary encoder (nest_input)
    "esp_driver_rmt",  # WS2812 LED ring via led_strip (nest_led_ring)
)

# The LED ring driver, pinned at the native firmware's locked version
# (dependencies.lock).
LED_STRIP_VERSION = "3.0.3"

# LVGL is configured through its own Kconfig (the IDF component reads
# CONFIG_LV_*); ESPHome's lvgl component is not used.
LVGL_SDKCONFIG = {
    # Hardware-verified spike set (esphome/README.md, variant B).
    "CONFIG_LV_COLOR_DEPTH_16": True,
    "CONFIG_LV_OS_FREERTOS": True,
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": 2,
    # Display refresh period. The encoder indev read timer is pinned to the
    # native 33 ms separately, where the indev is created.
    "CONFIG_LV_DEF_REFR_PERIOD": 15,
    "CONFIG_LV_USE_CLIB_STRING": True,
    "CONFIG_LV_USE_CLIB_SPRINTF": True,
    "CONFIG_LV_OBJ_STYLE_CACHE": True,
    # Allocator: lv_malloc_core & co. are implemented in nest_ui_task.cpp
    # (PSRAM first, internal fallback): ESPHome's psram component makes
    # malloc() internal-only (CONFIG_SPIRAM_USE_CAPS_ALLOC).
    "CONFIG_LV_USE_CUSTOM_MALLOC": True,
    "CONFIG_LV_USE_ASSERT_MALLOC": True,
    "CONFIG_LV_USE_ASSERT_NULL": True,
    # The assert handler: nest_lv_assert.h (CONFIG_LV_ASSERT_CUSTOM_INCLUDE
    # is set in to_code, it is a build-tree path). LVGL's default handler
    # spins forever on nest_ui; this one prints the location and aborts.
    "CONFIG_LV_ASSERT_USE_CUSTOM_INCLUDE": True,
    # Software draw threads at or below loopTask's priority (1), so
    # rendering can never starve ESPHome's loop (lv_freertos.c creates them
    # at tskIDLE_PRIORITY + this value). 1 = LV_THREAD_PRIO_LOW.
    "CONFIG_LV_DRAW_THREAD_PRIO": 1,
    # Logging: warnings and errors, forwarded to the ESPHome logger by the
    # print callback registered in nest_ui_task.cpp.
    "CONFIG_LV_USE_LOG": True,
    "CONFIG_LV_LOG_LEVEL_WARN": True,
    # What the UI uses, pinned so a changed LVGL default cannot drop it.
    "CONFIG_LV_FONT_MONTSERRAT_12": True,
    "CONFIG_LV_USE_ARC": True,
    "CONFIG_LV_USE_LABEL": True,
    "CONFIG_LV_USE_IMAGE": True,
    "CONFIG_LV_DRAW_SW_SUPPORT_RGB565": True,
    "CONFIG_LV_DRAW_SW_SUPPORT_A8": True,
    "CONFIG_LV_DRAW_SW_COMPLEX": True,
    "CONFIG_LV_USE_THEME_DEFAULT": True,
    # Native carried these, the UI does not use them.
    "CONFIG_LV_FONT_MONTSERRAT_24": False,
    "CONFIG_LV_FONT_MONTSERRAT_48": False,
    "CONFIG_LV_USE_LODEPNG": False,
}

# ---------------------------------------------------------------------------
# Blinds labels and their font
# ---------------------------------------------------------------------------

# The blinds group labels render in this pre-rasterized font, which only has
# the glyphs it was generated with (assets/README.md). The glyph set is read
# from the `--range` option lv_font_conv records in the file header, so the
# validation follows a regenerated font automatically.
LABEL_FONT_FILE = Path(__file__).resolve().parent / "font_helvetica_100.c"

# Native Kconfig defaults (TOPIC_BLINDS_LABEL_A / _B).
DEFAULT_BLINDS_LABEL_A = "A"
DEFAULT_BLINDS_LABEL_B = "B"

_FONT_RANGE_RE = re.compile(r"^ \* Opts: .*?--range (\S+)", re.MULTILINE)
_label_font_glyphs: frozenset[int] | None = None


def _parse_font_range(spec: str) -> frozenset[int]:
    """Code points of an lv_font_conv --range list, e.g. 0x20-0x7E,0xC1."""
    code_points: set[int] = set()
    for item in spec.split(","):
        first, sep, last = item.partition("-")
        try:
            start = int(first, 0)
            end = int(last, 0) if sep else start
        except ValueError as err:
            raise EsphomeError(
                f"esp_nest: cannot parse the --range item {item!r} in {LABEL_FONT_FILE}"
            ) from err
        if end < start:
            raise EsphomeError(
                f"esp_nest: empty --range item {item!r} in {LABEL_FONT_FILE}"
            )
        code_points.update(range(start, end + 1))
    return frozenset(code_points)


def label_font_glyphs() -> frozenset[int]:
    """The code points font_helvetica_100.c has glyphs for (cached)."""
    global _label_font_glyphs  # pylint: disable=global-statement
    if _label_font_glyphs is None:
        try:
            header = LABEL_FONT_FILE.read_text(encoding="utf-8")[:4096]
        except OSError as err:
            raise EsphomeError(
                f"esp_nest: cannot read the label font {LABEL_FONT_FILE}: {err}"
            ) from err
        match = _FONT_RANGE_RE.search(header)
        if match is None:
            raise EsphomeError(
                f"esp_nest: no '--range' in the Opts header of {LABEL_FONT_FILE}"
            )
        _label_font_glyphs = _parse_font_range(match.group(1))
    return _label_font_glyphs


def _describe_code_points(code_points) -> str:
    return ", ".join(
        f"'{chr(cp)}' (U+{cp:04X})" if chr(cp).isprintable() else f"U+{cp:04X}"
        for cp in sorted(code_points)
    )


def blinds_label(value):
    """A blinds group label: visible text without leading or trailing
    whitespace, every glyph present in the font."""
    value = cv.string(value)
    if not value.strip():
        raise cv.Invalid(
            "A blinds label must not be empty or only whitespace: the screen "
            "would show nothing."
        )
    if value != value.strip():
        raise cv.Invalid(
            f"Blinds label '{value}' has leading or trailing whitespace, which "
            "would shift the centred label off centre. Remove it."
        )
    glyphs = label_font_glyphs()
    missing = {ord(char) for char in value if ord(char) not in glyphs}
    if missing:
        raise cv.Invalid(
            f"Blinds label '{value}' uses {_describe_code_points(missing)}, which "
            f"{LABEL_FONT_FILE.name} has no glyph for: the label would render "
            "with gaps. Use other characters, or regenerate the font with a "
            "wider --range if its source face has them (assets/README.md)."
        )
    return value


# Native Kconfig defaults (TOPIC_BLINDS_GROUP_A / _B).
DEFAULT_BLINDS_GROUP_A = "group_a"
DEFAULT_BLINDS_GROUP_B = "group_b"


def _blinds_group_schema(default_topic_id: str, default_label: str) -> cv.Schema:
    return cv.Schema(
        {
            # Group id in the blinds bridge's topics:
            # <prefix>/<topic_id>_{1..3}/position/..., <prefix>/<topic_id>_group/command
            # (native Kconfig TOPIC_BLINDS_GROUP_A / _B).
            cv.Optional(CONF_TOPIC_ID, default=default_topic_id): cv.publish_topic,
            # Shown on the blinds detail screen (native Kconfig
            # TOPIC_BLINDS_LABEL_A / _B).
            cv.Optional(CONF_LABEL, default=default_label): blinds_label,
        }
    )


BLINDS_SCHEMA = cv.Schema(
    {
        # Topic prefix of the ESPHome RF cover bridge (native TOPIC_BLINDS_PREFIX).
        cv.Optional(CONF_PREFIX, default="blinds/cover"): cv.publish_topic,
        cv.Optional(CONF_GROUP_A, default={}): _blinds_group_schema(
            DEFAULT_BLINDS_GROUP_A, DEFAULT_BLINDS_LABEL_A
        ),
        cv.Optional(CONF_GROUP_B, default={}): _blinds_group_schema(
            DEFAULT_BLINDS_GROUP_B, DEFAULT_BLINDS_LABEL_B
        ),
    }
)

# ---------------------------------------------------------------------------
# External-device MQTT topics
# ---------------------------------------------------------------------------

# Defaults are the native Kconfig defaults (generic; the real values live in
# the private overlay / the HA device YAML as substitutions). No wildcards:
# ESPHome's MQTT client subscribes to exact topics only.
TOPICS_SCHEMA = cv.Schema(
    {
        # zigbee2mqtt light group: <lights> (state), <lights>/set, <lights>/get.
        cv.Optional(CONF_LIGHTS, default="zigbee2mqtt/lights"): cv.publish_topic,
        # Fan state, retained text Off / low / medium / high.
        cv.Optional(CONF_FAN, default="home/fan"): cv.publish_topic,
        # Stereo volume base: <stereo_volume>/set takes UP / DOWN pulses.
        cv.Optional(CONF_STEREO_VOLUME, default="home/stereo/volume"): cv.publish_topic,
    }
)


def _unique_topics(pairs: list[tuple[str, str]]) -> dict[str, str]:
    """{topic: description}; a topic listed twice is a configuration error."""
    result: dict[str, str] = {}
    for topic, what in pairs:
        if topic in result:
            raise cv.Invalid(
                f"MQTT topic '{topic}' is used for both {result[topic]} and {what}. "
                "Every topic the knob subscribes to or publishes to must be distinct "
                "(each message must reach exactly one handler, and a published topic "
                "must never come back as a subscribed one)."
            )
        result[topic] = what
    return result


def _bridge_topics(config) -> tuple[dict[str, str], dict[str, str]]:
    """The topics the MQTT bridge subscribes to and publishes to, each
    mapped to a description (mirrors MqttBridge::setup in nest_mqtt_bridge.cpp)."""
    topics = config[CONF_TOPICS]
    blinds = config[CONF_BLINDS]
    prefix = blinds[CONF_PREFIX]
    subscribed: list[tuple[str, str]] = [
        (topics[CONF_FAN], "the fan state topic (topics: fan)"),
        (topics[CONF_LIGHTS], "the lights state topic (topics: lights)"),
    ]
    published: list[tuple[str, str]] = [
        (f"{topics[CONF_LIGHTS]}/set", "the lights /set topic (topics: lights + /set)"),
        (f"{topics[CONF_LIGHTS]}/get", "the lights /get topic (topics: lights + /get)"),
        (
            f"{topics[CONF_STEREO_VOLUME]}/set",
            "the stereo volume /set topic (topics: stereo_volume + /set)",
        ),
    ]
    for key in (CONF_GROUP_A, CONF_GROUP_B):
        group = f"{prefix}/{blinds[key][CONF_TOPIC_ID]}"
        published.append(
            (f"{group}_group/command", f"the {key} group command topic")
        )
        for member in (1, 2, 3):
            subscribed.append(
                (f"{group}_{member}/position/state", f"the {key} member {member} state topic")
            )
            published.append(
                (
                    f"{group}_{member}/position/command",
                    f"the {key} member {member} command topic",
                )
            )
    return _unique_topics(subscribed), _unique_topics(published)


def validate_distinct_topics(config):
    """Reject configurations whose topics collide.

    The native firmware routed every message to exactly one handler (if/else
    on the topic); ESPHome calls every matching subscription, so two equal
    subscription topics would drive two handlers. A published topic equal to
    a subscribed one would feed our own publishes back as state. The one
    deliberate overlap is the fan topic, which is both (retained state).
    """
    subscribed, published = _bridge_topics(config)
    fan = config[CONF_TOPICS][CONF_FAN]
    for topic, what in published.items():
        if topic in subscribed and topic != fan:
            raise cv.Invalid(
                f"MQTT topic '{topic}' is published to as {what} but also subscribed "
                f"to as {subscribed[topic]}: our own publishes would come back as "
                "state. Every topic must be distinct."
            )
    if fan in published:
        raise cv.Invalid(
            f"The fan topic '{fan}' is also {published[fan]}; it must be distinct "
            "from every other topic."
        )
    return config


# ---------------------------------------------------------------------------
# Settings entities
# ---------------------------------------------------------------------------


class NumberSetting(NamedTuple):
    """One settings number, as the native firmware advertised it to HA
    (main/ha_mqtt.c discovery: name, range, step, unit, icon)."""

    conf: str
    setting_id: str  # SettingId member (nest_settings.h)
    setter: str  # EspNest method taking the entity
    name: str
    icon: str
    unit: str
    minimum: int
    maximum: int
    step: int
    initial: int  # native boot default (settings_word.h)


SETTINGS_HEADER = Path(__file__).resolve().parent / "settings_word.h"
_SETTINGS_DEFINE = re.compile(r"^#define\s+(SETTINGS_[A-Z_]+)\s+(\d+)\s*$", re.MULTILINE)


def _settings_constants() -> dict[str, int]:
    """The SETTINGS_* defines of settings_word.h: the single source of the
    entities' ranges, steps and defaults, shared with the C++ hand-off and
    backlight_policy (a static_assert there ties it to the latter)."""
    return {
        name: int(value)
        for name, value in _SETTINGS_DEFINE.findall(SETTINGS_HEADER.read_text())
    }


def _number_settings() -> tuple[NumberSetting, ...]:
    constants = _settings_constants()

    def spec(
        conf: str,
        setting_id: str,
        setter: str,
        name: str,
        icon: str,
        unit: str,
        prefix: str,
    ) -> NumberSetting:
        try:
            minimum, maximum, step, initial = (
                constants[f"SETTINGS_{prefix}_{field}"]
                for field in ("MIN", "MAX", "STEP", "DEFAULT")
            )
        except KeyError as err:
            raise EsphomeError(
                f"esp_nest: {SETTINGS_HEADER.name} lacks #define {err.args[0]}"
            ) from err
        return NumberSetting(
            conf, setting_id, setter, name, icon, unit, minimum, maximum, step, initial
        )

    return (
        spec(
            CONF_SCREEN_BRIGHTNESS,
            "BACKLIGHT_PCT",
            "set_screen_brightness",
            "Screen Brightness",
            "mdi:brightness-6",
            "%",
            "SCREEN_BRIGHTNESS",
        ),
        spec(
            CONF_DIM_BRIGHTNESS,
            "DIM_PCT",
            "set_dim_brightness",
            "Dim Brightness",
            "mdi:brightness-4",
            "%",
            "DIM_BRIGHTNESS",
        ),
        spec(
            CONF_IDLE_TIMEOUT,
            "IDLE_TIMEOUT_S",
            "set_idle_timeout",
            "Idle Timeout",
            "mdi:timer-sand",
            "s",
            "IDLE_TIMEOUT",
        ),
    )


# The ranges, steps and defaults are fixed, read from settings_word.h (they
# are the UI's and backlight_policy's limits); the entity names, the initial
# value and restore_value can be overridden in YAML.
NUMBER_SETTINGS = _number_settings()

LED_RING_NAME = "LED Ring"
LED_RING_ICON = "mdi:led-on"


def _default_name(name: str):
    """Validator: fills in the entity name when the YAML gives none, so the
    entities exist (parity with native) without any configuration."""

    def validator(value):
        value = dict(value)
        value.setdefault(CONF_NAME, name)
        return value

    return validator


def _number_setting_schema(spec: NumberSetting):
    return cv.All(
        _default_name(spec.name),
        number.number_schema(
            SettingNumber,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon=spec.icon,
            unit_of_measurement=spec.unit,
        ).extend(
            {
                # Persistence is a YAML switch: the native firmware kept
                # these in RAM only (plan decision D2).
                cv.Optional(CONF_INITIAL_VALUE, default=spec.initial): cv.int_range(
                    spec.minimum, spec.maximum
                ),
                cv.Optional(CONF_RESTORE_VALUE, default=False): cv.boolean,
            }
        ),
    )


LED_RING_SCHEMA = cv.All(
    _default_name(LED_RING_NAME),
    switch.switch_schema(
        LedRingSwitch,
        block_inverted=True,
        # Native: the ring is on at every boot.
        default_restore_mode="ALWAYS_ON",
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon=LED_RING_ICON,
    ),
)

# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

DISPLAY_SCHEMA = cv.Schema(
    {
        # true: the ST7701S mirrors X (SDIR) and flush only reverses the
        # row order (35 fps on hardware); false: per-pixel 180° copy like
        # the native firmware (33.6 fps).
        cv.Optional(CONF_HW_MIRROR_X, default=True): cv.boolean,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EspNest),
            # ESPHome's MQTT client (`mqtt:`), configured silent in the
            # package: no topic prefix, no discovery.
            cv.GenerateID(CONF_MQTT_ID): cv.use_id(mqtt.MQTTClientComponent),
            cv.Optional(CONF_DISPLAY, default={}): DISPLAY_SCHEMA,
            cv.Optional(CONF_TOPICS, default={}): TOPICS_SCHEMA,
            cv.Optional(CONF_BLINDS, default={}): BLINDS_SCHEMA,
            **{
                cv.Optional(spec.conf, default={}): _number_setting_schema(spec)
                for spec in NUMBER_SETTINGS
            },
            cv.Optional(CONF_LED_RING, default={}): LED_RING_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_distinct_topics,
    cv.only_with_framework("esp-idf"),
    esp32.only_on_variant(supported=[VARIANT_ESP32S3]),
)


# ---------------------------------------------------------------------------
# Final validation guards (hard errors)
# ---------------------------------------------------------------------------


def _csv_sha256(path: Path) -> str:
    """sha256 of a CSV file with CRLF line endings normalised to LF."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def _validate_partitions(full_config) -> None:
    """esp32.partitions must be the repository's partitions.csv.

    Catches the silent shadowing of the package's file by a
    /config/partitions.csv in the HA Device Builder, and any edit of the
    table: an image built against another table cannot boot from the
    device's existing (OTA-immutable) partition table.
    """
    partitions = full_config[PLATFORM_ESP32].get(CONF_PARTITIONS)
    if partitions is None:
        raise cv.Invalid(
            "esp_nest requires 'esp32: partitions:' pointing at the repository's "
            "partitions.csv: the device keeps the native firmware's partition table, "
            "which OTA cannot change."
        )
    if isinstance(partitions, list):
        raise cv.Invalid(
            "esp_nest requires 'esp32: partitions:' to be the repository's "
            "partitions.csv file, not a list of custom partitions."
        )
    path = CORE.relative_config_path(partitions)
    digest = _csv_sha256(path)
    if digest != PARTITIONS_CSV_SHA256:
        raise cv.Invalid(
            f"esp32.partitions '{path}' has sha256 {digest} (line endings "
            f"normalised to LF), expected "
            f"{PARTITIONS_CSV_SHA256} (the repository's partitions.csv). The partition "
            "table on the device cannot change over OTA; check that no other "
            "partitions.csv (e.g. /config/partitions.csv) shadows the package's file."
        )


def _validate_project_version(full_config) -> None:
    """esphome.project.version must equal version.txt (the single source)."""
    project = full_config[CONF_ESPHOME].get(CONF_PROJECT)
    if project is None:
        raise cv.Invalid(
            "esp_nest requires 'esphome: project:' with 'version: !include "
            "../version.txt'."
        )
    try:
        expected = VERSION_FILE.read_text(encoding="utf-8").strip()
    except OSError as err:
        raise cv.Invalid(
            f"esp_nest cannot read the repository version file {VERSION_FILE}: {err}"
        ) from err
    version = project[CONF_VERSION]
    if version != expected:
        raise cv.Invalid(
            f"esphome.project.version is '{version}' but {VERSION_FILE} says "
            f"'{expected}'; set 'version: !include ../version.txt' in the package."
        )


def _validate_broker(full_config) -> None:
    """The broker must be a bare host name or IP address.

    The native Kconfig MQTT_BROKER_URI is a full mqtt://host:port URI, so a
    value copied from there would pass config validation and only fail at
    runtime, when the name never resolves. ESPHome's mqtt component takes the
    host alone and has a separate 'port' option.
    """
    broker = str(full_config["mqtt"][CONF_BROKER])
    # A single colon is a ':port' suffix; more than one is a bare IPv6 address.
    if "://" in broker or broker.count(":") == 1:
        raise cv.Invalid(
            f"mqtt.broker is '{broker}', but must be a host name or IP address "
            "only, without a scheme (mqtt://) or a ':port' suffix; set the "
            "port with 'mqtt: port:'."
        )


def _final_validate(config):
    full_config = fv.full_config.get()
    _validate_broker(full_config)
    _validate_partitions(full_config)
    _validate_project_version(full_config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_hw_mirror_x(config[CONF_DISPLAY][CONF_HW_MIRROR_X]))
    blinds = config[CONF_BLINDS]
    cg.add(
        var.set_blinds_labels(
            blinds[CONF_GROUP_A][CONF_LABEL], blinds[CONF_GROUP_B][CONF_LABEL]
        )
    )

    mqtt_client = await cg.get_variable(config[CONF_MQTT_ID])
    topics = config[CONF_TOPICS]
    cg.add(
        var.set_mqtt(
            mqtt_client,
            topics[CONF_LIGHTS],
            topics[CONF_FAN],
            topics[CONF_STEREO_VOLUME],
            blinds[CONF_PREFIX],
            blinds[CONF_GROUP_A][CONF_TOPIC_ID],
            blinds[CONF_GROUP_B][CONF_TOPIC_ID],
        )
    )

    for spec in NUMBER_SETTINGS:
        conf = config[spec.conf]
        entity = await number.new_number(
            conf,
            getattr(SettingId, spec.setting_id),
            min_value=spec.minimum,
            max_value=spec.maximum,
            step=spec.step,
        )
        cg.add(entity.set_initial_value(conf[CONF_INITIAL_VALUE]))
        cg.add(entity.set_restore_value(conf[CONF_RESTORE_VALUE]))
        cg.add(getattr(var, spec.setter)(entity))
    led_ring = await switch.new_switch(config[CONF_LED_RING])
    cg.add(var.set_led_ring(led_ring))

    for name in IDF_BUILTIN_COMPONENTS:
        esp32.include_builtin_idf_component(name)
    esp32.add_idf_component(name="lvgl/lvgl", ref=LVGL_VERSION)
    esp32.add_idf_component(name="espressif/led_strip", ref=LED_STRIP_VERSION)
    for key, value in LVGL_SDKCONFIG.items():
        esp32.add_idf_sdkconfig_option(key, value)
    esp32.add_idf_sdkconfig_option(
        "CONFIG_LV_ASSERT_CUSTOM_INCLUDE", _lv_assert_include_path()
    )
    # Only LVGL's archive references the handler, and the linker scans it
    # after the archive that defines it: force the definition in (the IDF
    # idiom for such hooks, cf. ESPHome's own --wrap=esp_panic_handler).
    cg.add_build_flag(f"-Wl,-u,{LV_ASSERT_HANDLER_SYMBOL}")


def _lv_assert_include_path() -> str:
    """Absolute path of nest_lv_assert.h in the build tree, for LVGL's Kconfig.

    LVGL does `#include LV_ASSERT_CUSTOM_INCLUDE` from its own sources, whose
    include path does not contain this component, so the header is named by
    absolute path: the copy ESPHome writes into the build's src tree (all
    top-level component sources are copied there). Forward slashes keep the
    Kconfig string and the #include valid on Windows too.
    """
    path = CORE.relative_src_path(
        "esphome", "components", "esp_nest", LV_ASSERT_HEADER
    ).resolve()
    text = path.as_posix()
    if '"' in text or "\\" in text:
        raise EsphomeError(
            f"esp_nest: the build path {text!r} contains a quote or backslash, "
            "which cannot be passed to LVGL as CONFIG_LV_ASSERT_CUSTOM_INCLUDE"
        )
    return text
