#ifndef ILI9488_H
#define ILI9488_H

#include <gpiod.hpp>
#include <string>
#include <vector>
#include <cstdint>

// Display Configuration (landscape)
const int DISPLAY_WIDTH = 480;
const int DISPLAY_HEIGHT = 320;
const int OFFSET_X = 0;
const int OFFSET_Y = 0;

// ILI9488 Commands
const uint8_t ILI9488_SWRESET = 0x01;
const uint8_t ILI9488_SLPOUT  = 0x11;
const uint8_t ILI9488_COLMOD  = 0x3A;
const uint8_t ILI9488_MADCTL  = 0x36;
const uint8_t ILI9488_CASET   = 0x2A;
const uint8_t ILI9488_RASET   = 0x2B;
const uint8_t ILI9488_RAMWR   = 0x2C;
const uint8_t ILI9488_DISPON  = 0x29;

class ILI9488 {
public:
    ILI9488(const std::string& spi_device,
            const std::string& dc_chip_path, int dc_pin,
            const std::string& rst_chip_path, int rst_pin,
            const std::string& bl_chip_path, int bl_pin);
    ~ILI9488();

    bool Init();
    void SendCommand(uint8_t cmd, const std::vector<uint8_t>& data = {});
    void SendData(const std::vector<uint8_t>& data);
    void Display(const std::vector<uint16_t>& buffer);
    void UpdateRect(int x, int y, int w, int h, const uint16_t* rgb565, int stride_pixels);
    void Clear(uint16_t color);
    void SetBacklight(bool on);

private:
    void Reset();
    void SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

    std::string spi_device_;
    int spi_fd_ = -1;
    uint32_t spi_speed_hz_ = 16000000;
    size_t chunk_size_bytes_ = 1024;
    unsigned int throttle_us_ = 0;
    std::vector<uint8_t> tx_buf_;

    gpiod::chip dc_chip_;
    gpiod::line dc_line_;
    gpiod::chip rst_chip_;
    gpiod::line rst_line_;
    gpiod::chip bl_chip_;
    gpiod::line bl_line_;

    bool is_initialized_ = false;
    bool gpio_ready_ = false;
};

#endif // ILI9488_H
