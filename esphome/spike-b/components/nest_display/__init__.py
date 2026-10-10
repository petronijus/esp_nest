"""Display pipeline + LVGL owned by ESP Nest code, hosted inside ESPHome."""

import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "psram"]

CONF_HW_MIRROR_X = "hw_mirror_x"

nest_display_ns = cg.esphome_ns.namespace("nest_display")
NestDisplay = nest_display_ns.class_("NestDisplay", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NestDisplay),
        cv.Optional(CONF_HW_MIRROR_X, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)

LVGL_VERSION = "9.6.0"

# LVGL is configured through its own Kconfig (the IDF component reads
# CONFIG_LV_*), independent of ESPHome's lvgl component, which is not used.
LVGL_SDKCONFIG = {
    "CONFIG_LV_COLOR_DEPTH_16": True,
    "CONFIG_LV_OS_FREERTOS": True,
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": 2,
    "CONFIG_LV_DEF_REFR_PERIOD": 15,
    "CONFIG_LV_USE_CLIB_MALLOC": True,
    "CONFIG_LV_USE_CLIB_STRING": True,
    "CONFIG_LV_USE_CLIB_SPRINTF": True,
    "CONFIG_LV_OBJ_STYLE_CACHE": True,
    "CONFIG_LV_USE_LOG": True,
    "CONFIG_LV_LOG_LEVEL_WARN": True,
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_hw_mirror_x(config[CONF_HW_MIRROR_X]))

    for name in ("esp_lcd", "esp_driver_ledc", "esp_driver_pcnt"):
        esp32.include_builtin_idf_component(name)
    esp32.add_idf_component(name="lvgl/lvgl", ref=LVGL_VERSION)
    for key, value in LVGL_SDKCONFIG.items():
        esp32.add_idf_sdkconfig_option(key, value)
