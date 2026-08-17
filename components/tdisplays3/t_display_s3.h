#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace esphome {
namespace tdisplays3 {

// T-Display-S3 hardware config: ST7789V, 170x320, 8-bit parallel I80
class LGFX_TDisplayS3 : public lgfx::LGFX_Device {
public:
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;
  LGFX_TDisplayS3();
};

class TDisplayS3 : public display::DisplayBuffer {
 public:
  void dump_config() override;
  void setup() override;

  void fill(Color color) override;
  int get_width_internal() override;
  int get_height_internal() override;
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_COLOR; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void update() override;
  void set_dimensions(uint16_t width, uint16_t height);

 private:
  LGFX_TDisplayS3 *lcd_{nullptr};
  lgfx::LGFX_Sprite *spr_{nullptr};
  uint16_t width_{170};
  uint16_t height_{320};
};

}  // namespace tdisplays3
}  // namespace esphome
