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
    # TFT_eSPI build flags for T-Display-S3 (ST7789V, 170x320, 8-bit parallel)
    cg.add_build_flag("-DUSER_SETUP_LOADED")
    cg.add_build_flag("-DST7789_DRIVER")
    cg.add_build_flag("-DINIT_SEQUENCE_3")
    cg.add_build_flag("-DCGRAM_OFFSET")
    cg.add_build_flag("-DTFT_RGB_ORDER=TFT_RGB")
    cg.add_build_flag("-DTFT_INVERSION_ON")
    cg.add_build_flag("-DTFT_PARALLEL_8_BIT")
    cg.add_build_flag(f"-DTFT_WIDTH={config[CONF_WIDTH]}")
    cg.add_build_flag(f"-DTFT_HEIGHT={config[CONF_HEIGHT]}")
    cg.add_build_flag("-DTFT_RST=5")
    cg.add_build_flag("-DTFT_CS=6")
    cg.add_build_flag("-DTFT_DC=7")
    cg.add_build_flag("-DTFT_WR=8")
    cg.add_build_flag("-DTFT_RD=9")
    cg.add_build_flag("-DTFT_D0=39")
    cg.add_build_flag("-DTFT_D1=40")
    cg.add_build_flag("-DTFT_D2=41")
    cg.add_build_flag("-DTFT_D3=42")
    cg.add_build_flag("-DTFT_D4=45")
    cg.add_build_flag("-DTFT_D5=46")
    cg.add_build_flag("-DTFT_D6=47")
    cg.add_build_flag("-DTFT_D7=48")
    cg.add_build_flag("-DDISABLE_ALL_LIBRARY_WARNINGS")

    cg.add_library("TFT_eSPI", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    cg.add(var.set_dimensions(config[CONF_WIDTH], config[CONF_HEIGHT]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
