#include "t_display_s3.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace tdisplays3 {

static const char *const TAG = "TDisplayS3";

constexpr int TDisplayS3::DATA_PINS[8];

void TDisplayS3::setup() {
  size_t fb_bytes = (size_t)width_ * height_ * sizeof(uint16_t);

  fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (fb_) {
    ESP_LOGI(TAG, "Framebuffer: internal SRAM (%zu bytes)", fb_bytes);
  } else {
    fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb_)
      ESP_LOGW(TAG, "Framebuffer: PSRAM (%zu bytes) — DMA may not work", fb_bytes);
  }
  if (!fb_) {
    ESP_LOGE(TAG, "Framebuffer allocation failed (%zu bytes)", fb_bytes);
    this->mark_failed();
    return;
  }
  memset(fb_, 0, fb_bytes);

  // RD pin held HIGH (parallel write-only mode)
  gpio_config_t rd_cfg = {};
  rd_cfg.pin_bit_mask = 1ULL << RD_PIN;
  rd_cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rd_cfg);
  gpio_set_level((gpio_num_t)RD_PIN, 1);

  // I80 bus
  esp_lcd_i80_bus_config_t bus_cfg = {};
  bus_cfg.dc_gpio_num        = DC_PIN;
  bus_cfg.wr_gpio_num        = WR_PIN;
  bus_cfg.clk_src            = LCD_CLK_SRC_DEFAULT;
  bus_cfg.bus_width          = 8;
  bus_cfg.max_transfer_bytes = fb_bytes + 64;
  bus_cfg.dma_burst_size     = 64;
  for (int i = 0; i < 8; i++)
    bus_cfg.data_gpio_nums[i] = DATA_PINS[i];
  // Mark unused upper-byte data pins as unused (-1)
  for (int i = 8; i < SOC_LCD_I80_BUS_WIDTH; i++)
    bus_cfg.data_gpio_nums[i] = -1;

  esp_err_t err = esp_lcd_new_i80_bus(&bus_cfg, &i80_bus_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I80 bus create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Panel IO
  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num              = CS_PIN;
  io_cfg.pclk_hz                  = 10 * 1000 * 1000;  // 10 MHz (within ST7789V 66ns cycle spec)
  io_cfg.trans_queue_depth        = 10;
  io_cfg.lcd_cmd_bits             = 8;
  io_cfg.lcd_param_bits           = 8;
  io_cfg.flags.swap_color_bytes   = 1;  // RGB565: host little-endian → display big-endian

  err = esp_lcd_new_panel_io_i80(i80_bus_, &io_cfg, &io_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Panel IO create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // ST7789 vendor panel driver — handles reset timing, SWRESET/SLPOUT/COLMOD/MADCTL/DISPON
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num  = RST_PIN;
  panel_cfg.rgb_endian      = LCD_RGB_ENDIAN_RGB;  // matches TFT_RGB_ORDER=TFT_RGB
  panel_cfg.bits_per_pixel  = 16;

  err = esp_lcd_new_panel_st7789(io_handle_, &panel_cfg, &panel_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Panel create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Bring up the display
  esp_lcd_panel_reset(panel_handle_);   // RST LOW→HIGH + stabilisation
  esp_lcd_panel_init(panel_handle_);    // SWRESET→SLPOUT→COLMOD→MADCTL→DISPON (with proper delays)

  esp_lcd_panel_invert_color(panel_handle_, true);      // matches TFT_INVERSION_ON
  esp_lcd_panel_set_gap(panel_handle_, COL_OFFSET, 0);  // CGRAM_OFFSET: shift columns +35

  ESP_LOGI(TAG, "T-Display-S3 ready (%dx%d, col_offset=%d)", width_, height_, COL_OFFSET);

  // Boot test: green frame for 500ms. If visible, the full path works.
  for (size_t i = 0; i < (size_t)width_ * height_; i++)
    fb_[i] = 0x07E0;
  push_frame_();
  vTaskDelay(pdMS_TO_TICKS(500));
  memset(fb_, 0, fb_bytes);
}

void TDisplayS3::push_frame_() {
  // draw_bitmap applies the gap offset (COL_OFFSET) automatically via set_gap()
  esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle_, 0, 0, (int)width_, (int)height_, fb_);
  if (ret != ESP_OK)
    ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(ret));
}

void TDisplayS3::dump_config() {
  LOG_DISPLAY("", "T-Display-S3 ST7789 (8-bit parallel, ESP-IDF)", this);
  ESP_LOGCONFIG(TAG, "  RST: GPIO%d  CS: GPIO%d  DC: GPIO%d  WR: GPIO%d  RD: GPIO%d",
                RST_PIN, CS_PIN, DC_PIN, WR_PIN, RD_PIN);
  ESP_LOGCONFIG(TAG, "  Data: GPIO%d-%d (D0-D3), GPIO%d-%d (D4-D7)",
                DATA_PINS[0], DATA_PINS[3], DATA_PINS[4], DATA_PINS[7]);
  LOG_UPDATE_INTERVAL(this);
}

void TDisplayS3::fill(Color color) {
  uint16_t c = display::ColorUtil::color_to_565(color);
  uint32_t count = (uint32_t)width_ * height_;
  for (uint32_t i = 0; i < count; i++)
    fb_[i] = c;
}

void TDisplayS3::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= (int)width_ || y < 0 || y >= (int)height_)
    return;
  fb_[y * width_ + x] = display::ColorUtil::color_to_565(color);
}

void TDisplayS3::update() {
  this->do_update_();
  if (!panel_handle_)
    return;
  push_frame_();
}

}  // namespace tdisplays3
}  // namespace esphome
