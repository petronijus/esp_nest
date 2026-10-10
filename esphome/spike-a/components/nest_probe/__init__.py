"""Feasibility probe: raw LVGL C API driven from an ESPHome external component."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["lvgl"]

nest_probe_ns = cg.esphome_ns.namespace("nest_probe")
NestProbe = nest_probe_ns.class_("NestProbe", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(NestProbe)}).extend(
    cv.COMPONENT_SCHEMA
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
