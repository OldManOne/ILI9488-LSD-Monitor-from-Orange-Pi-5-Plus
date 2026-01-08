#ifndef ST7789_H
#define ST7789_H

#include <gpiod.hpp>
#include <string>
#include <vector>
#include <cstdint>

// Display Configuration
const int DISPLAY_WIDTH = 240;
const int DISPLAY_HEIGHT = 135;
const int OFFSET_X = 40;
const int OFFSET_Y = 52;

// ST7789 Commands
const uint8_t ST7789_SWRESET = 0x01;
const uint8_t ST7789_SLPOUT = 0x11;
const uint8_t ST7789_COLMOD = 0x3A;
const uint8_t ST7789_MADCTL = 0x36;
const uint8_t ST7789_CASET = 0x2A;
const uint8_t ST7789_RASET = 0x2B;
const uint8_t ST7789_RAMWR = 0x2C;
const uint8_t ST7789_INVON = 0x21;
const uint8_t ST7789_DISPON = 0x29;

class ST7789 {
public:
    ST7789(const std::string& spi_device,
           const std::string& dc_chip_path, int dc_pin,
           const std::string& rst_chip_path, int rst_pin,
           const std::string& bl_chip_path, int bl_pin);
    ~ST7789();

    bool Init();
    void SendCommand(uint8_t cmd, const std::vector<uint8_t>& data = {});
    void SendData(const std::vector<uint8_t>& data);
    void Display(const std::vector<uint16_t>& buffer);
    void Clear(uint16_t color);
    void SetBacklight(bool on);

private:
    void Reset();
    void SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

    std::string spi_device_;
    int spi_fd_ = -1;

    gpiod::chip dc_chip_;
    gpiod::line dc_line_;
    gpiod::chip rst_chip_;
    gpiod::line rst_line_;
    gpiod::chip bl_chip_;
    gpiod::line bl_line_;

    bool is_initialized_ = false;
    bool gpio_ready_ = false;
};

#endif // ST7789_H
