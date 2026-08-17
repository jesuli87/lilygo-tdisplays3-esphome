#include "t_display_s3.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"

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

void TDisplayS3::push_frame_() {
  // Set address window every frame (avoids pointer drift)
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
  esp_err_t ret = esp_lcd_panel_io_tx_color(io_handle_, ST7789_RAMWR, fb_,
                                            (size_t)width_ * height_ * sizeof(uint16_t));
  if (ret != ESP_OK)
    ESP_LOGE(TAG, "tx_color failed: %s", esp_err_to_name(ret));
}

void TDisplayS3::setup() {
  size_t fb_bytes = (size_t)width_ * height_ * sizeof(uint16_t);

  // Try DMA-capable internal SRAM first
  fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (fb_) {
    ESP_LOGI(TAG, "Framebuffer: internal SRAM (%zu bytes)", fb_bytes);
  } else {
    // Fallback: PSRAM. On ESP32-S3 GDMA can read PSRAM, but flag it so we know.
    fb_ = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb_) {
      ESP_LOGW(TAG, "Framebuffer: PSRAM (%zu bytes) — DMA may not work", fb_bytes);
    }
  }
  if (!fb_) {
    ESP_LOGE(TAG, "Failed to allocate %zu-byte framebuffer", fb_bytes);
    this->mark_failed();
    return;
  }
  memset(fb_, 0, fb_bytes);

  // RD pin held HIGH (write-only parallel mode)
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

  // Attach panel IO
  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num       = CS_PIN;
  io_cfg.pclk_hz           = 20 * 1000 * 1000;
  io_cfg.trans_queue_depth  = 10;
  io_cfg.lcd_cmd_bits      = 8;
  io_cfg.lcd_param_bits    = 8;
  // Byte-swap each RGB565 word: host is little-endian, ST7789 expects big-endian
  io_cfg.flags.swap_color_bytes = 1;

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

  // ST7789V init — matches INIT_SEQUENCE_3 + CGRAM_OFFSET + INVERSION_ON
  send_command_(ST7789_SWRESET);
  vTaskDelay(pdMS_TO_TICKS(150));

  send_command_(ST7789_SLPOUT);
  vTaskDelay(pdMS_TO_TICKS(120));   // ST7789 datasheet: min 120 ms after sleep-out

  { uint8_t d[] = {0x55}; send_command_data_(ST7789_COLMOD, d, sizeof(d)); }  // 16-bit
  vTaskDelay(pdMS_TO_TICKS(10));

  { uint8_t d[] = {0x00}; send_command_data_(ST7789_MADCTL, d, sizeof(d)); }  // portrait, RGB

  // Porch control (improves reliability on many ST7789V panels)
  { uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; send_command_data_(0xB2, d, sizeof(d)); }
  // Gate control
  { uint8_t d[] = {0x35}; send_command_data_(0xB7, d, sizeof(d)); }
  // VCOM
  { uint8_t d[] = {0x19}; send_command_data_(0xBB, d, sizeof(d)); }
  // LCM control
  { uint8_t d[] = {0x2C}; send_command_data_(0xC0, d, sizeof(d)); }
  // VDV and VRH command enable
  { uint8_t d[] = {0x01}; send_command_data_(0xC2, d, sizeof(d)); }
  // VRH set
  { uint8_t d[] = {0x12}; send_command_data_(0xC3, d, sizeof(d)); }
  // VDV set
  { uint8_t d[] = {0x20}; send_command_data_(0xC4, d, sizeof(d)); }
  // Frame rate: 60 Hz
  { uint8_t d[] = {0x0F}; send_command_data_(0xC6, d, sizeof(d)); }
  // Power control
  { uint8_t d[] = {0xA4, 0xA1}; send_command_data_(0xD0, d, sizeof(d)); }

  // CASET: column 35–204 (170-pixel panel at +35 on 240-wide controller)
  {
    uint8_t d[] = {0x00, (uint8_t)COL_OFFSET,
                   0x00, (uint8_t)(COL_OFFSET + width_ - 1)};
    send_command_data_(ST7789_CASET, d, sizeof(d));
  }
  // RASET: row 0–319
  {
    uint8_t d[] = {0x00, 0x00,
                   (uint8_t)((height_ - 1) >> 8), (uint8_t)((height_ - 1) & 0xFF)};
    send_command_data_(ST7789_RASET, d, sizeof(d));
  }

  send_command_(ST7789_INVON);
  vTaskDelay(pdMS_TO_TICKS(10));

  send_command_(ST7789_NORON);
  vTaskDelay(pdMS_TO_TICKS(10));

  send_command_(ST7789_DISPON);
  vTaskDelay(pdMS_TO_TICKS(100));

  ESP_LOGI(TAG, "T-Display-S3 ready (%dx%d, col_offset=%d)", width_, height_, COL_OFFSET);

  // Hardware test: push solid green immediately after init.
  // If the display shows green on boot, the hardware path is working.
  for (size_t i = 0; i < (size_t)width_ * height_; i++)
    fb_[i] = 0x07E0;  // RGB565 green (no byte-swap needed here; swap_color_bytes handles it)
  push_frame_();
  vTaskDelay(pdMS_TO_TICKS(500));
  memset(fb_, 0, fb_bytes);
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
  push_frame_();
}

void TDisplayS3::send_command_(uint8_t cmd) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, nullptr, 0);
}

void TDisplayS3::send_command_data_(uint8_t cmd, const uint8_t *data, size_t len) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, data, len);
}

}  // namespace tdisplays3
}  // namespace esphome
