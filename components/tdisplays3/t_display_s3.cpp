#include "t_display_s3.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// ---------------------------------------------------------------------------
// Bit-bang diagnostic: raw GPIO I80 write, no LCD_CAM peripheral involved.
// Runs once at boot before the LCD_CAM is touched. Answers: "is the
// hardware physically reachable?" If the screen shows green during the 2-s
// hold, the hardware works and the LCD_CAM config is the bug. If nothing
// appears, the hardware (FPC, panel, chip) is broken.
// ---------------------------------------------------------------------------
namespace {

static void bb_write_byte(uint8_t b) {
    gpio_set_level(GPIO_NUM_39, (b >> 0) & 1);
    gpio_set_level(GPIO_NUM_40, (b >> 1) & 1);
    gpio_set_level(GPIO_NUM_41, (b >> 2) & 1);
    gpio_set_level(GPIO_NUM_42, (b >> 3) & 1);
    gpio_set_level(GPIO_NUM_45, (b >> 4) & 1);
    gpio_set_level(GPIO_NUM_46, (b >> 5) & 1);
    gpio_set_level(GPIO_NUM_47, (b >> 6) & 1);
    gpio_set_level(GPIO_NUM_48, (b >> 7) & 1);
    gpio_set_level(GPIO_NUM_8, 0);    // WR LOW
    esp_rom_delay_us(1);
    gpio_set_level(GPIO_NUM_8, 1);    // WR HIGH
    esp_rom_delay_us(1);
}

static void bb_cmd(uint8_t cmd) {
    gpio_set_level(GPIO_NUM_6, 0);    // CS LOW
    gpio_set_level(GPIO_NUM_7, 0);    // DC LOW (command)
    bb_write_byte(cmd);
    gpio_set_level(GPIO_NUM_6, 1);    // CS HIGH
}

static void bb_data(const uint8_t *data, size_t len) {
    gpio_set_level(GPIO_NUM_6, 0);    // CS LOW
    gpio_set_level(GPIO_NUM_7, 1);    // DC HIGH (data)
    for (size_t i = 0; i < len; i++) bb_write_byte(data[i]);
    gpio_set_level(GPIO_NUM_6, 1);    // CS HIGH
}

static void bit_bang_test() {
    ESP_LOGI("bb", "Bit-bang diagnostic: plain GPIO, no LCD_CAM");

    // All I80 + control pins as plain GPIO outputs
    const gpio_num_t PINS[] = {
        GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9,
        GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42,
        GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48,
    };
    uint64_t mask = 0;
    for (gpio_num_t p : PINS) mask |= 1ULL << (int)p;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);

    gpio_set_level(GPIO_NUM_5, 1);    // RST HIGH
    gpio_set_level(GPIO_NUM_6, 1);    // CS HIGH
    gpio_set_level(GPIO_NUM_7, 1);    // DC HIGH
    gpio_set_level(GPIO_NUM_8, 1);    // WR HIGH (idle)
    gpio_set_level(GPIO_NUM_9, 1);    // RD HIGH

    // Verify GPIO output register is being set correctly.
    // gpio_get_level() on an output-mode pin reads the output register.
    // If these log 1/1/0/1, the ESP32 side is correct — problem is the physical trace.
    // If any log wrong values, there is a GPIO driver issue.
    gpio_set_level(GPIO_NUM_39, 1);
    gpio_set_level(GPIO_NUM_48, 1);
    gpio_set_level(GPIO_NUM_8,  0);  // WR LOW for contrast
    ESP_LOGI("bb", "GPIO register check: D0(GPIO39)=%d D7(GPIO48)=%d WR(GPIO8)=%d [expected 1,1,0]",
             gpio_get_level(GPIO_NUM_39),
             gpio_get_level(GPIO_NUM_48),
             gpio_get_level(GPIO_NUM_8));
    gpio_set_level(GPIO_NUM_39, 0);
    gpio_set_level(GPIO_NUM_48, 0);
    gpio_set_level(GPIO_NUM_8,  1);

    // Hardware reset
    gpio_set_level(GPIO_NUM_5, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_5, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Full ST7789V init — no INVON so green fills as true green
    bb_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));          // SWRESET
    bb_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));          // SLPOUT
    { uint8_t d[] = {0x55}; bb_cmd(0x3A); bb_data(d,1); } // COLMOD 16-bit
    vTaskDelay(pdMS_TO_TICKS(10));
    { uint8_t d[] = {0x00}; bb_cmd(0x36); bb_data(d,1); } // MADCTL
    { uint8_t d[] = {0x0C,0x0C,0x00,0x33,0x33}; bb_cmd(0xB2); bb_data(d,5); }
    { uint8_t d[] = {0x35}; bb_cmd(0xB7); bb_data(d,1); }
    { uint8_t d[] = {0x1A}; bb_cmd(0xBB); bb_data(d,1); }
    { uint8_t d[] = {0x2C}; bb_cmd(0xC0); bb_data(d,1); }
    { uint8_t d[] = {0x01}; bb_cmd(0xC2); bb_data(d,1); }
    { uint8_t d[] = {0x12}; bb_cmd(0xC3); bb_data(d,1); }
    { uint8_t d[] = {0x20}; bb_cmd(0xC4); bb_data(d,1); }
    { uint8_t d[] = {0x0F}; bb_cmd(0xC6); bb_data(d,1); }
    { uint8_t d[] = {0xA4,0xA1}; bb_cmd(0xD0); bb_data(d,2); }
    { uint8_t d[] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,
                     0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
      bb_cmd(0xE0); bb_data(d,14); }
    { uint8_t d[] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,
                     0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
      bb_cmd(0xE1); bb_data(d,14); }
    bb_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(10));           // DISPON

    // Cover ALL 240 controller columns so col_offset uncertainty doesn't hide the result
    { uint8_t d[] = {0x00,0x00,0x00,0xEF}; bb_cmd(0x2A); bb_data(d,4); } // CASET 0-239
    { uint8_t d[] = {0x00,0x00,0x01,0x3F}; bb_cmd(0x2B); bb_data(d,4); } // RASET 0-319
    bb_cmd(0x2C);  // RAMWR

    // Fill 240x320 pixels GREEN (RGB565 0x07E0 big-endian: 0x07, 0xE0)
    gpio_set_level(GPIO_NUM_6, 0);    // CS LOW
    gpio_set_level(GPIO_NUM_7, 1);    // DC HIGH
    for (int i = 0; i < 240 * 320; i++) {
        bb_write_byte(0x07);
        bb_write_byte(0xE0);
    }
    gpio_set_level(GPIO_NUM_6, 1);    // CS HIGH

    ESP_LOGI("bb", "Bit-bang fill done — holding 2s. Did you see GREEN?");
    vTaskDelay(pdMS_TO_TICKS(2000));
}

}  // namespace

namespace esphome {
namespace tdisplays3 {

static const char *const TAG = "TDisplayS3";

constexpr int TDisplayS3::DATA_PINS[8];

void TDisplayS3::send_cmd_(uint8_t cmd) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, NULL, 0);
}

void TDisplayS3::send_cmd_data_(uint8_t cmd, const uint8_t *data, size_t len) {
  esp_lcd_panel_io_tx_param(io_handle_, cmd, data, len);
}

void TDisplayS3::init_lcd_() {
  // Full ST7789V init matching TFT_eSPI's sequence for T-Display-S3.
  // The generic ESP-IDF vendor driver omits the power control and gamma
  // registers, leaving AVDD/AVEE/VGH/VGL at post-reset defaults that are
  // often insufficient for the LC drive voltage this panel requires.
  send_cmd_(0x01);                                     // SWRESET
  vTaskDelay(pdMS_TO_TICKS(150));

  send_cmd_(0x11);                                     // SLPOUT
  vTaskDelay(pdMS_TO_TICKS(120));

  { uint8_t d[] = {0x55};
    send_cmd_data_(0x3A, d, sizeof d); }               // COLMOD: 16-bit RGB565
  vTaskDelay(pdMS_TO_TICKS(10));

  { uint8_t d[] = {0x00};
    send_cmd_data_(0x36, d, sizeof d); }               // MADCTL: portrait, RGB order

  { uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    send_cmd_data_(0xB2, d, sizeof d); }               // PORCTR: frame rate 60 Hz

  { uint8_t d[] = {0x35};
    send_cmd_data_(0xB7, d, sizeof d); }               // GCTRL

  { uint8_t d[] = {0x1A};
    send_cmd_data_(0xBB, d, sizeof d); }               // VCOMS = 0.9 V

  { uint8_t d[] = {0x2C};
    send_cmd_data_(0xC0, d, sizeof d); }               // LCMCTRL

  { uint8_t d[] = {0x01};
    send_cmd_data_(0xC2, d, sizeof d); }               // VDVVRHEN

  { uint8_t d[] = {0x12};
    send_cmd_data_(0xC3, d, sizeof d); }               // VRHS = 4.45 V

  { uint8_t d[] = {0x20};
    send_cmd_data_(0xC4, d, sizeof d); }               // VDV = 0 V

  { uint8_t d[] = {0x0F};
    send_cmd_data_(0xC6, d, sizeof d); }               // FRCTRL2 = 60 Hz

  { uint8_t d[] = {0xA4, 0xA1};
    send_cmd_data_(0xD0, d, sizeof d); }               // PWCTRL1: AVDD=6.8V AVEE=-6.8V

  { uint8_t d[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                   0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    send_cmd_data_(0xE0, d, sizeof d); }               // PVGAMCTRL

  { uint8_t d[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                   0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    send_cmd_data_(0xE1, d, sizeof d); }               // NVGAMCTRL

  send_cmd_(0x21);                                     // INVON: colour inversion
  send_cmd_(0x29);                                     // DISPON
  vTaskDelay(pdMS_TO_TICKS(10));
}

void TDisplayS3::setup() {
  // Force GPIO15 (display power enable) HIGH before anything else.
  // RESTORE_DEFAULT_ON restores NVS-saved state, which may be OFF if the switch
  // was ever turned off. The ST7789V has no power when GPIO15 is LOW.
  gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_15, 1);
  vTaskDelay(pdMS_TO_TICKS(100));  // Power rail stabilisation
  ESP_LOGI(TAG, "GPIO15 (power_en) forced HIGH, waiting for power rail");

  // Diagnostic: raw GPIO bit-bang — proves hardware connectivity independent of LCD_CAM.
  // Watch for GREEN on screen during the ~2s hold. See log for "Did you see GREEN?".
  bit_bang_test();

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
  io_cfg.pclk_hz                  = 4 * 1000 * 1000;   // 4 MHz: conservative for signal integrity
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

  // Vendor panel driver: used for RST GPIO management and draw_bitmap.
  // We bypass panel_init() and send the full init sequence ourselves.
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num  = RST_PIN;
  panel_cfg.rgb_endian      = LCD_RGB_ENDIAN_RGB;
  panel_cfg.bits_per_pixel  = 16;

  err = esp_lcd_new_panel_st7789(io_handle_, &panel_cfg, &panel_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Panel create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Hardware reset via RST pin, then wait for controller to stabilise
  esp_lcd_panel_reset(panel_handle_);
  vTaskDelay(pdMS_TO_TICKS(120));

  // Full init: power control + gamma registers that the generic vendor driver omits.
  // Without PWCTRL1/VCOMS/VRHS the LC drive voltages stay at post-reset defaults
  // which are often too low for this panel to display anything.
  init_lcd_();

  esp_lcd_panel_set_gap(panel_handle_, COL_OFFSET, 0);  // CGRAM shift: 240-wide ctrl → 170-wide panel

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
