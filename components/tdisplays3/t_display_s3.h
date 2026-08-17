#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"

#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

namespace esphome {
namespace tdisplays3 {

class TDisplayS3 : public display::DisplayBuffer {
 public:
  void setup() override;
  void dump_config() override;
  void update() override;
  void fill(Color color) override;
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  int get_width_internal() override { return width_; }
  int get_height_internal() override { return height_; }
  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_COLOR;
  }

  void set_dimensions(uint16_t width, uint16_t height) {
    width_ = width;
    height_ = height;
  }

 protected:
  void send_command_(uint8_t cmd);
  void send_command_data_(uint8_t cmd, const uint8_t *data, size_t len);
  void push_frame_();

  esp_lcd_i80_bus_handle_t i80_bus_{nullptr};
  esp_lcd_panel_io_handle_t io_handle_{nullptr};
  uint16_t *fb_{nullptr};
  uint16_t width_{170};
  uint16_t height_{320};

  // Column offset: ST7789V controller is 240 wide, panel is 170 wide
  static constexpr int COL_OFFSET = 35;

  // Fixed hardware pins for T-Display-S3 parallel interface
  static constexpr int WR_PIN  = 8;
  static constexpr int RD_PIN  = 9;
  static constexpr int RST_PIN = 5;
  static constexpr int CS_PIN  = 6;
  static constexpr int DC_PIN  = 7;
  static constexpr int DATA_PINS[8] = {39, 40, 41, 42, 45, 46, 47, 48};
};

}  // namespace tdisplays3
}  // namespace esphome
