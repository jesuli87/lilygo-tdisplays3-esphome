import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.components import display
from esphome.const import (
    CONF_HEIGHT,
    CONF_ID,
    CONF_LAMBDA,
    CONF_WIDTH,
)
from . import tdisplays3_ns

AUTO_LOAD = ["psram"]
DEPENDENCIES = ["esp32"]

TDISPLAYS3 = tdisplays3_ns.class_(
    "TDisplayS3", cg.PollingComponent, display.Display
)

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(TDISPLAYS3),
            cv.Optional(CONF_HEIGHT, default=320): cv.uint16_t,
            cv.Optional(CONF_WIDTH, default=170): cv.uint16_t,
        }
    ).extend(cv.polling_component_schema("5s")),
)


async def to_code(config):
    cg.add_library("lovyan03/LovyanGFX", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    cg.add(var.set_dimensions(config[CONF_WIDTH], config[CONF_HEIGHT]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
