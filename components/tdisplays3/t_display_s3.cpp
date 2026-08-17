#include "t_display_s3.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ST7789 command set
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT  0x11
#define ST7789_COLMOD  0x3A
#define ST7789_MADCTL  0x36
#define ST7789_CASET   0x2A
#define ST7789_RASET   0x2B
#define ST7789_INVON   0x21
#define ST7789_NORON   0x13
#define ST7789_DISPON  0x29
#define ST7789_RAMWR   0x2C

namespace esphome {
namespace tdisplays3 {

static const char *const TAG = "TDisplayS3";

constexpr int TDisplayS3::DATA_PINS[8];

void TDisplayS3::setup() {
  // Allocate DMA-capable framebuffer (portrait: width_ × height_ × 2 bytes)
  size_t fb_bytes = (size_t)width_ * height_ * sizeof(uint16_t);
  fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!fb_) {
    // Internal SRAM full — fall back to PSRAM (DMA-accessible on ESP32-S3)
    fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!fb_) {
    ESP_LOGE(TAG, "Failed to allocate %zu-byte framebuffer", fb_bytes);
    this->mark_failed();
    return;
  }
  memset(fb_, 0, fb_bytes);

  // RD pin must be held HIGH (write-only parallel mode)
  gpio_config_t rd_cfg = {};
  rd_cfg.pin_bit_mask = 1ULL << RD_PIN;
  rd_cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rd_cfg);
  gpio_set_level((gpio_num_t)RD_PIN, 1);

  // Create Intel 8080 bus
  esp_lcd_i80_bus_config_t bus_cfg = {};
  bus_cfg.dc_gpio_num  = DC_PIN;
  bus_cfg.wr_gpio_num  = WR_PIN;
  bus_cfg.clk_src      = LCD_CLK_SRC_DEFAULT;
  bus_cfg.bus_width    = 8;
  bus_cfg.max_transfer_bytes = fb_bytes + 64;
  for (int i = 0; i < 8; i++)
    bus_cfg.data_gpio_nums[i] = DATA_PINS[i];

  esp_err_t err = esp_lcd_new_i80_bus(&bus_cfg, &i80_bus_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I80 bus create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Attach panel IO to bus
  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num       = CS_PIN;
  io_cfg.pclk_hz           = 20 * 1000 * 1000;  // 20 MHz parallel clock
  io_cfg.trans_queue_depth  = 10;
  io_cfg.lcd_cmd_bits      = 8;
  io_cfg.lcd_param_bits    = 8;
  io_cfg.flags.swap_color_bytes = 1;  // host is little-endian; ST7789 wants big-endian RGB565

  err = esp_lcd_new_panel_io_i80(i80_bus_, &io_cfg, &io_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Panel IO create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Hardware reset
  gpio_config_t rst_cfg = {};
  rst_cfg.pin_bit_mask = 1ULL << RST_PIN;
  rst_cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rst_cfg);
  gpio_set_level((gpio_num_t)RST_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level((gpio_num_t)RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(150));

  // ST7789 init sequence (matches INIT_SEQUENCE_3 + CGRAM_OFFSET + INVERSION_ON)
  send_command_(ST7789_SWRESET);
  vTaskDelay(pdMS_TO_TICKS(150));

  send_command_(ST7789_SLPOUT);
  vTaskDelay(pdMS_TO_TICKS(10));

  { uint8_t d[] = {0x55}; send_command_data_(ST7789_COLMOD, d, sizeof(d)); }  // 16-bit color
  { uint8_t d[] = {0x00}; send_command_data_(ST7789_MADCTL, d, sizeof(d)); }  // portrait, RGB

  // CASET: column 35 to 204  (170-pixel panel at +35 offset on 240-wide controller)
  {
    uint8_t d[] = {0x00, (uint8_t)COL_OFFSET,
                   0x00, (uint8_t)(COL_OFFSET + width_ - 1)};
    send_command_data_(ST7789_CASET, d, sizeof(d));
  }
  // RASET: row 0 to 319
  {
    uint8_t d[] = {0x00, 0x00,
                   (uint8_t)((height_ - 1) >> 8), (uint8_t)((height_ - 1) & 0xFF)};
    send_command_data_(ST7789_RASET, d, sizeof(d));
  }

  send_command_(ST7789_INVON);   // colour inversion ON (matches TFT_INVERSION_ON)
  vTaskDelay(pdMS_TO_TICKS(10));

  send_command_(ST7789_NORON);
  vTaskDelay(pdMS_TO_TICKS(10));

  send_command_(ST7789_DISPON);
  vTaskDelay(pdMS_TO_TICKS(255));

  ESP_LOGI(TAG, "T-Display-S3 ready (%dx%d, col_offset=%d)", width_, height_, COL_OFFSET);
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
  if (!io_handle_)
    return;

  // Reset address window before each frame so the pointer wraps correctly
  {
    uint8_t d[] = {0x00, (uint8_t)COL_OFFSET,
                   0x00, (uint8_t)(COL_OFFSET + width_ - 1)};
    send_command_data_(ST7789_CASET, d, sizeof(d));
  }
  {
    uint8_t d[] = {0x00, 0x00,
                   (uint8_t)((height_ - 1) >> 8), (uint8_t)((height_ - 1) & 0xFF)};
    send_command_data_(ST7789_RASET, d, sizeof(d));
  }

  // Push full framebuffer via DMA (RAMWR = write to GRAM)
  esp_lcd_panel_io_tx_color(io_handle_, ST7789_RAMWR, fb_,
                            (size_t)width_ * height_ * sizeof(uint16_t));
}

void TDisplayS3::send_command_(uint8_t cmd) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, nullptr, 0);
}

void TDisplayS3::send_command_data_(uint8_t cmd, const uint8_t *data, size_t len) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, data, len);
}

}  // namespace tdisplays3
}  // namespace esphome
