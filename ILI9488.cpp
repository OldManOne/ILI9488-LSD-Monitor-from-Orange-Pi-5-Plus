#include "ILI9488.h"
#include <iostream>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace {
constexpr uint8_t ILI9488_PIXFMT_18BPP = 0x66; // RGB666
constexpr uint8_t ILI9488_MADCTL_LANDSCAPE = 0x28; // MV|BGR
constexpr uint32_t SPI_SPEED_HZ_DEFAULT = 16000000;
constexpr uint32_t SPI_SPEED_HZ_MAX = 24000000;
constexpr size_t CHUNK_SIZE_DEFAULT = 1024;

static uint32_t getenv_u32(const char* name, uint32_t def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    char* end = nullptr;
    unsigned long val = std::strtoul(v, &end, 10);
    if (!end || *end != '\0') return def;
    return static_cast<uint32_t>(val);
}

static size_t getenv_szt(const char* name, size_t def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    char* end = nullptr;
    unsigned long val = std::strtoul(v, &end, 10);
    if (!end || *end != '\0') return def;
    return static_cast<size_t>(val);
}

static unsigned int getenv_uint(const char* name, unsigned int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    char* end = nullptr;
    unsigned long val = std::strtoul(v, &end, 10);
    if (!end || *end != '\0') return def;
    return static_cast<unsigned int>(val);
}
}

ILI9488::ILI9488(const std::string& spi_device,
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
        dc_line_.request({"ili9488-dc", gpiod::line_request::DIRECTION_OUTPUT, 0});
        rst_line_.request({"ili9488-rst", gpiod::line_request::DIRECTION_OUTPUT, 0});
        bl_line_.request({"ili9488-bl", gpiod::line_request::DIRECTION_OUTPUT, 0});
        std::cout << "  [GPIO] Lines requested." << std::endl << std::flush;
        gpio_ready_ = true;
    } catch (const std::exception& e) {
        std::cerr << "  [GPIO] Init failed: " << e.what() << std::endl << std::flush;
        gpio_ready_ = false;
    }
}

ILI9488::~ILI9488() {
    if (is_initialized_) {
        SetBacklight(false);
        if (spi_fd_ != -1) {
            close(spi_fd_);
        }
    }
}

bool ILI9488::Init() {
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
    spi_speed_hz_ = getenv_u32("ILI9488_SPI_SPEED_HZ", SPI_SPEED_HZ_DEFAULT);
    if (spi_speed_hz_ > SPI_SPEED_HZ_MAX) spi_speed_hz_ = SPI_SPEED_HZ_MAX;
    chunk_size_bytes_ = getenv_szt("ILI9488_SPI_CHUNK", CHUNK_SIZE_DEFAULT);
    if (chunk_size_bytes_ < 3) chunk_size_bytes_ = 3;
    if (chunk_size_bytes_ % 3 != 0) {
        chunk_size_bytes_ -= (chunk_size_bytes_ % 3);
        if (chunk_size_bytes_ < 3) chunk_size_bytes_ = 3;
    }
    throttle_us_ = getenv_uint("ILI9488_SPI_THROTTLE_US", 0);
    uint32_t speed = spi_speed_hz_;

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
    std::cout << "  ILI9488: speed=" << spi_speed_hz_ << "Hz"
              << " chunk=" << chunk_size_bytes_ << "B"
              << " throttle=" << throttle_us_ << "us"
              << " COLMOD=0x66"
              << " MADCTL=0x28"
              << " size=" << DISPLAY_WIDTH << "x" << DISPLAY_HEIGHT
              << std::endl << std::flush;

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
    SendCommand(ILI9488_SWRESET);
    usleep(150000);

    std::cout << "  Sending SLPOUT..." << std::endl << std::flush;
    SendCommand(ILI9488_SLPOUT);
    usleep(120000);

    std::cout << "  Sending COLMOD (RGB666)..." << std::endl << std::flush;
    SendCommand(ILI9488_COLMOD, {ILI9488_PIXFMT_18BPP});
    usleep(10000);

    std::cout << "  Sending MADCTL (landscape)..." << std::endl << std::flush;
    SendCommand(ILI9488_MADCTL, {ILI9488_MADCTL_LANDSCAPE});
    usleep(10000);

    std::cout << "  Sending DISPON..." << std::endl << std::flush;
    SendCommand(ILI9488_DISPON);
    usleep(100000);

    std::cout << "  Enabling backlight..." << std::endl << std::flush;
    SetBacklight(true);
    std::cout << "  Backlight enabled." << std::endl << std::flush;

    is_initialized_ = true;
    return true;
}

void ILI9488::SendCommand(uint8_t cmd, const std::vector<uint8_t>& data) {
    dc_line_.set_value(0);
    if (write(spi_fd_, &cmd, 1) != 1) {
        std::cerr << "Failed to send SPI command" << std::endl << std::flush;
    }
    if (!data.empty()) {
        SendData(data);
    }
}

void ILI9488::SendData(const std::vector<uint8_t>& data) {
    dc_line_.set_value(1);
    if (write(spi_fd_, data.data(), data.size()) != (ssize_t)data.size()) {
        std::cerr << "Failed to send SPI data" << std::endl << std::flush;
    }
}

void ILI9488::Display(const std::vector<uint16_t>& buffer) {
    if (!is_initialized_) return;
    UpdateRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, buffer.data(), DISPLAY_WIDTH);
}

void ILI9488::UpdateRect(int x, int y, int w, int h, const uint16_t* rgb565, int stride_pixels) {
    if (!is_initialized_) return;
    if (!rgb565 || w <= 0 || h <= 0) return;

    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(DISPLAY_WIDTH, x + w);
    int y1 = std::min(DISPLAY_HEIGHT, y + h);
    if (x1 <= x0 || y1 <= y0) return;

    int rw = x1 - x0;
    int rh = y1 - y0;

    SetWindow(static_cast<uint16_t>(x0),
              static_cast<uint16_t>(y0),
              static_cast<uint16_t>(x1 - 1),
              static_cast<uint16_t>(y1 - 1));

    dc_line_.set_value(1);

    const size_t chunk_pixels = std::max<size_t>(1, chunk_size_bytes_ / 3);
    if (tx_buf_.capacity() < chunk_size_bytes_) {
        tx_buf_.reserve(chunk_size_bytes_);
    }

    size_t sent_bytes_total = 0;
    size_t chunk_index = 0;
    for (int row = 0; row < rh; ++row) {
        const uint16_t* src = rgb565 + (y0 + row) * stride_pixels + x0;
        int remaining = rw;
        while (remaining > 0) {
            size_t this_pixels = std::min(chunk_pixels, static_cast<size_t>(remaining));
            size_t this_bytes = this_pixels * 3;
            tx_buf_.resize(this_bytes);
            uint8_t* out = tx_buf_.data();
            for (size_t i = 0; i < this_pixels; ++i) {
                uint16_t px = src[i];
                uint8_t r5 = (px >> 11) & 0x1F;
                uint8_t g6 = (px >> 5) & 0x3F;
                uint8_t b5 = px & 0x1F;
                out[i * 3 + 0] = static_cast<uint8_t>(r5 << 3);
                out[i * 3 + 1] = static_cast<uint8_t>(g6 << 2);
                out[i * 3 + 2] = static_cast<uint8_t>(b5 << 3);
            }

            struct spi_ioc_transfer tr = {};
            tr.tx_buf = (unsigned long)(tx_buf_.data());
            tr.len = this_bytes;
            tr.speed_hz = spi_speed_hz_;
            tr.bits_per_word = 8;

            if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 1) {
                std::cerr << "Failed to send SPI message chunk: " << std::strerror(errno)
                          << " chunk=" << chunk_index
                          << " bytes=" << sent_bytes_total << "+" << this_bytes
                          << " speed=" << spi_speed_hz_
                          << " chunk_size=" << chunk_size_bytes_
                          << std::endl << std::flush;
                return;
            }

            src += this_pixels;
            remaining -= static_cast<int>(this_pixels);
            sent_bytes_total += this_bytes;
            ++chunk_index;
            if (throttle_us_ > 0) {
                usleep(throttle_us_);
            }
        }
    }
}

void ILI9488::Clear(uint16_t color) {
    std::vector<uint16_t> buffer(DISPLAY_WIDTH * DISPLAY_HEIGHT, color);
    Display(buffer);
}

void ILI9488::SetBacklight(bool on) {
    bl_line_.set_value(on ? 1 : 0);
}

void ILI9488::Reset() {
    rst_line_.set_value(1);
    usleep(10000);
    rst_line_.set_value(0);
    usleep(20000);
    rst_line_.set_value(1);
    usleep(120000);
}

void ILI9488::SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += OFFSET_X;
    x1 += OFFSET_X;
    y0 += OFFSET_Y;
    y1 += OFFSET_Y;

    SendCommand(ILI9488_CASET, {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    });

    SendCommand(ILI9488_RASET, {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
    });

    SendCommand(ILI9488_RAMWR);
}
