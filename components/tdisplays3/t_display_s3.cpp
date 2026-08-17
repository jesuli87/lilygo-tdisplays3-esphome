#include "t_display_s3.h"

#include "esphome/components/display/display_color_utils.h"
#include "esphome/core/log.h"
#include "driver/gpio.h"

namespace esphome {
namespace tdisplays3 {

static const char *const TAG = "TDisplayS3";

LGFX_TDisplayS3::LGFX_TDisplayS3() {
  // 8-bit parallel (I80) bus via LCD_CAM peripheral
  {
    auto cfg = _bus.config();
    cfg.port       = 0;
    cfg.freq_write = 16000000;  // 16 MHz — same as LilyGo factory
    cfg.pin_wr     =  8;        // WR
    cfg.pin_rd     =  9;        // RD
    cfg.pin_rs     =  7;        // DC (data/command)
    cfg.pin_d0     = 39;
    cfg.pin_d1     = 40;
    cfg.pin_d2     = 41;
    cfg.pin_d3     = 42;
    cfg.pin_d4     = 45;
    cfg.pin_d5     = 46;
    cfg.pin_d6     = 47;
    cfg.pin_d7     = 48;
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }

  // ST7789V panel: 240-wide controller, 170-wide glass, 35-column offset
  {
    auto cfg = _panel.config();
    cfg.pin_cs       =  6;
    cfg.pin_rst      =  5;
    cfg.pin_busy     = -1;
    cfg.panel_width  = 170;
    cfg.panel_height = 320;
    cfg.offset_x     =  35;   // ST7789V column offset
    cfg.offset_y     =   0;
    cfg.offset_rotation = 0;
    cfg.dummy_read_pixel =  8;
    cfg.dummy_read_bits  =  1;
    cfg.readable   = false;
    cfg.invert     = true;    // INVON required for T-Display-S3
    cfg.rgb_order  = false;
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    _panel.config(cfg);
  }

  setPanel(&_panel);
}

void TDisplayS3::setup() {
  // GPIO15 switches the display VDD rail; must be HIGH before init
  gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_15, 1);

  this->lcd_ = new LGFX_TDisplayS3();
  this->lcd_->init();
  this->lcd_->fillScreen(TFT_BLACK);

  this->spr_ = new lgfx::LGFX_Sprite(this->lcd_);
  this->spr_->setColorDepth(16);
  if (!this->spr_->createSprite(this->get_width_internal(), this->get_height_internal())) {
    ESP_LOGE(TAG, "Sprite allocation failed");
    this->mark_failed();
  }
}

void TDisplayS3::dump_config() {
  LOG_DISPLAY("", "T-Display S3 (ST7789, LovyanGFX)", this);
  LOG_UPDATE_INTERVAL(this);
}

void TDisplayS3::fill(Color color) {
  this->spr_->fillScreen(display::ColorUtil::color_to_565(color));
}

void TDisplayS3::draw_absolute_pixel_internal(int x, int y, Color color) {
  this->spr_->drawPixel(x, y, display::ColorUtil::color_to_565(color));
}

int TDisplayS3::get_width_internal() {
  if (this->lcd_) return this->lcd_->width();
  return this->width_;
}

int TDisplayS3::get_height_internal() {
  if (this->lcd_) return this->lcd_->height();
  return this->height_;
}

void TDisplayS3::update() {
  this->do_update_();
  this->spr_->pushSprite(0, 0);
}

void TDisplayS3::set_dimensions(uint16_t width, uint16_t height) {
  this->width_  = width;
  this->height_ = height;
}

}  // namespace tdisplays3
}  // namespace esphome
