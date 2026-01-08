#ifndef THEME_H
#define THEME_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// RGB565 Color representation
using color_t = uint16_t;

// Helper to convert RGB888 to RGB565
constexpr color_t RGB(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

struct Theme {
    color_t bg_top_active;
    color_t bg_bottom_active;
    color_t bg_top_idle;
    color_t bg_bottom_idle;
    color_t icon_normal;
    color_t icon_dim;
    color_t text_value;
    color_t text_status;
    color_t state_low;
    color_t state_medium;
    color_t state_high;
    color_t accent_info;
    color_t accent_time;
    color_t bar_bg;
    color_t bar_border;
    color_t spark_bg;
    color_t grid_color;
    color_t band_color;
};

const std::map<std::string, Theme> THEMES = {
    {"neutral", {
        RGB(8, 8, 16), RGB(2, 2, 6), RGB(4, 4, 8), RGB(1, 1, 4),
        RGB(200, 200, 200), RGB(80, 80, 80), RGB(220, 220, 220), RGB(140, 140, 140),
        RGB(60, 180, 120), RGB(220, 180, 60), RGB(220, 80, 60),
        RGB(80, 160, 200), RGB(180, 140, 100), RGB(10, 10, 18), RGB(30, 30, 40), RGB(10, 10, 15),
        RGB(30, 30, 40), RGB(0, 0, 0)
    }},
    {"neon", {
        RGB(10, 6, 20), RGB(3, 2, 8), RGB(6, 4, 12), RGB(2, 1, 5),
        RGB(210, 240, 255), RGB(70, 90, 110), RGB(220, 240, 255), RGB(150, 170, 200),
        RGB(0, 220, 200), RGB(255, 120, 180), RGB(255, 110, 60),
        RGB(120, 190, 255), RGB(255, 160, 90), RGB(14, 10, 24), RGB(40, 35, 60), RGB(14, 12, 24),
        RGB(40, 35, 60), RGB(0, 0, 0)
    }},
    {"orange", {
        RGB(16, 10, 18), RGB(7, 4, 9), RGB(10, 7, 12), RGB(4, 3, 6),
        RGB(235, 220, 200), RGB(110, 90, 80), RGB(240, 225, 210), RGB(170, 145, 120),
        RGB(80, 200, 140), RGB(245, 150, 60), RGB(255, 100, 50),
        RGB(245, 130, 60), RGB(255, 150, 70), RGB(18, 12, 16), RGB(40, 30, 25), RGB(14, 10, 12),
        RGB(245, 130, 60), RGB(0, 0, 0)
    }}
};

const std::map<std::string, std::vector<double>> THRESHOLDS = {
    {"cpu", {40, 70, 90}},
    {"ram", {60, 80, 95}},
    {"temp", {50, 65, 80}},
    {"net", {800, 1800, 2500}}
};


#endif // THEME_H
