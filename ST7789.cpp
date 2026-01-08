#include "ST7789.h"
#include <iostream>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <stdexcept>

ST7789::ST7789(const std::string& spi_device,
               const std::string& dc_chip_path, int dc_pin,
               const std::string& rst_chip_path, int rst_pin,
               const std::string& bl_chip_path, int bl_pin)
    : spi_device_(spi_device) {
    try {
        std::cout << "  [GPIO] Opening DC chip " << dc_chip_path << "..." << std::endl << std::flush;
        dc_chip_ = gpiod::chip(dc_chip_path, gpiod::chip::OPEN_BY_PATH);
        std::cout << "  [GPIO] Opening RST chip " << rst_chip_path << "..." << std::endl << std::flush;
        rst_chip_ = gpiod::chip(rst_chip_path, gpiod::chip::OPEN_BY_PATH);
        std::cout << "  [GPIO] Opening BL chip " << bl_chip_path << "..." << std::endl << std::flush;
        bl_chip_ = gpiod::chip(bl_chip_path, gpiod::chip::OPEN_BY_PATH);
        std::cout << "  [GPIO] Chips opened." << std::endl << std::flush;

        std::cout << "  [GPIO] Getting lines..." << std::endl << std::flush;
        dc_line_ = dc_chip_.get_line(dc_pin);
        rst_line_ = rst_chip_.get_line(rst_pin);
        bl_line_ = bl_chip_.get_line(bl_pin);
        std::cout << "  [GPIO] Lines acquired." << std::endl << std::flush;

        std::cout << "  [GPIO] Requesting lines..." << std::endl << std::flush;
        dc_line_.request({"st7789-dc", gpiod::line_request::DIRECTION_OUTPUT, 0});
        rst_line_.request({"st7789-rst", gpiod::line_request::DIRECTION_OUTPUT, 0});
        bl_line_.request({"st7789-bl", gpiod::line_request::DIRECTION_OUTPUT, 0});
        std::cout << "  [GPIO] Lines requested." << std::endl << std::flush;
        gpio_ready_ = true;
    } catch (const std::exception& e) {
        std::cerr << "  [GPIO] Init failed: " << e.what() << std::endl << std::flush;
        gpio_ready_ = false;
    }
}

ST7789::~ST7789() {
    if (is_initialized_) {
        SetBacklight(false);
        if (spi_fd_ != -1) {
            close(spi_fd_);
        }
    }
}

bool ST7789::Init() {
    if (!gpio_ready_) {
        std::cerr << "  GPIO not initialized, cannot init display" << std::endl << std::flush;
        return false;
    }
    std::cout << "  Opening SPI device..." << std::endl << std::flush;
    spi_fd_ = open(spi_device_.c_str(), O_RDWR);
    if (spi_fd_ < 0) {
        std::cerr << "  Failed to open SPI device: " << spi_device_ << std::endl << std::flush;
        return false;
    }
    std::cout << "  SPI device opened." << std::endl << std::flush;

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 80000000;

    std::cout << "  Setting SPI parameters..." << std::endl << std::flush;
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) == -1 ||
        ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) == -1 ||
        ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == -1) {
        std::cerr << "  Failed to set SPI parameters" << std::endl << std::flush;
        close(spi_fd_);
        spi_fd_ = -1;
        return false;
    }
    std::cout << "  SPI parameters set." << std::endl << std::flush;

    std::cout << "  Resetting display..." << std::endl << std::flush;
    try {
        Reset();
    } catch (const std::exception& e) {
        std::cerr << "  Reset failed: " << e.what() << std::endl << std::flush;
        close(spi_fd_);
        spi_fd_ = -1;
        return false;
    }
    std::cout << "  Display reset." << std::endl << std::flush;
    
    std::cout << "  Sending SWRESET..." << std::endl << std::flush;
    SendCommand(ST7789_SWRESET);
    usleep(150000);

    std::cout << "  Sending SLPOUT..." << std::endl << std::flush;
    SendCommand(ST7789_SLPOUT);
    usleep(50000);

    std::cout << "  Sending COLMOD..." << std::endl << std::flush;
    SendCommand(ST7789_COLMOD, {0x05}); // 16-bit/pixel

    std::cout << "  Sending MADCTL..." << std::endl << std::flush;
    SendCommand(ST7789_MADCTL, {0x60}); // 90-degree rotation

    std::cout << "  Sending INVON..." << std::endl << std::flush;
    SendCommand(ST7789_INVON);

    std::cout << "  Sending DISPON..." << std::endl << std::flush;
    SendCommand(ST7789_DISPON);
    usleep(10000);

    std::cout << "  Enabling backlight..." << std::endl << std::flush;
    SetBacklight(true);
    std::cout << "  Backlight enabled." << std::endl << std::flush;

    is_initialized_ = true;
    return true;
}

void ST7789::SendCommand(uint8_t cmd, const std::vector<uint8_t>& data) {
    dc_line_.set_value(0); // Command mode
    if (write(spi_fd_, &cmd, 1) != 1) {
        std::cerr << "Failed to send SPI command" << std::endl << std::flush;
    }

    if (!data.empty()) {
        SendData(data);
    }
}

void ST7789::SendData(const std::vector<uint8_t>& data) {
    dc_line_.set_value(1); // Data mode
    if (write(spi_fd_, data.data(), data.size()) != (ssize_t)data.size()) {
        std::cerr << "Failed to send SPI data" << std::endl << std::flush;
    }
}

void ST7789::Display(const std::vector<uint16_t>& buffer) {
    if (!is_initialized_) return;
    
    SetWindow(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    
    std::vector<uint8_t> tx_buffer(buffer.size() * 2);
    for (size_t i = 0; i < buffer.size(); ++i) {
        tx_buffer[i * 2] = (buffer[i] >> 8) & 0xFF;
        tx_buffer[i * 2 + 1] = buffer[i] & 0xFF;
    }
    
    dc_line_.set_value(1); // Data mode

    const size_t chunkSize = 4096;
    for (size_t i = 0; i < tx_buffer.size(); i += chunkSize) {
        size_t end = i + chunkSize;
        if (end > tx_buffer.size()) {
            end = tx_buffer.size();
        }
        
        struct spi_ioc_transfer tr = {};
        tr.tx_buf = (unsigned long)(tx_buffer.data() + i);
        tr.len = end - i;
        tr.speed_hz = 80000000;
        tr.bits_per_word = 8;

        if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 1) {
            std::cerr << "Failed to send SPI message chunk" << std::endl << std::flush;
            return; // Exit on first failed chunk
        }
    }
}


void ST7789::Clear(uint16_t color) {
    std::vector<uint16_t> buffer(DISPLAY_WIDTH * DISPLAY_HEIGHT, color);
    Display(buffer);
}

void ST7789::SetBacklight(bool on) {
    bl_line_.set_value(on ? 1 : 0);
}

void ST7789::Reset() {
    rst_line_.set_value(1);
    usleep(10000);
    rst_line_.set_value(0);
    usleep(10000);
    rst_line_.set_value(1);
    usleep(120000);
}

void ST7789::SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += OFFSET_X;
    x1 += OFFSET_X;
    y0 += OFFSET_Y;
    y1 += OFFSET_Y;
    
    SendCommand(ST7789_CASET, {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    });

    SendCommand(ST7789_RASET, {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
    });

    SendCommand(ST7789_RAMWR);
}
