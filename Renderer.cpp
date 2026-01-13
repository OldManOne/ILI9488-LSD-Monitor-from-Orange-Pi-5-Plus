#include "Renderer.h"
#include "ILI9488.h"
#include "Theme.h"
#include "PrinterClient.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <iomanip>
#include <unistd.h>

#include "stb_truetype.h"

// --- Sparkline Visual Zoom Parameters ---
namespace SparklineZoom {
    // NET (Mbps): zoom active at low traffic
    constexpr double NET_ZOOM_START = 20.0;
    constexpr double NET_ZOOM_END = 800.0;

    // CPU (%): zoom active at low usage
    constexpr double CPU_ZOOM_START = 5.0;
    constexpr double CPU_ZOOM_END = 60.0;

    // TEMP (°C): zoom active at low temps
    constexpr double TEMP_ZOOM_START = 30.0;
    constexpr double TEMP_ZOOM_END = 70.0;

    // Gamma range for visual zoom
    constexpr double GAMMA_MIN = 0.55;
    constexpr double GAMMA_MAX = 1.0;

    // Minimum data ranges per metric type (below this = flat line)
    constexpr double MIN_RANGE_CPU = 0.5;
    constexpr double MIN_RANGE_TEMP = 0.2;
    constexpr double MIN_RANGE_NET = 1.0;
}

// --- Fill parameters ---
// Balanced gradients: visible but not overpowering
constexpr double FILL_INTENSITY_SERIES = 0.55;   // additive fill start (series line)
constexpr double FILL_DECAY_SERIES     = 1.4;    // decay speed series
constexpr double FILL_ALPHA_SPARK      = 0.70;   // base alpha for sparkline
constexpr double FILL_DECAY_SPARK      = 1.5;    // decay speed sparkline

// --- Exponential decay lookup table for gradient fills ---
// Pre-computed exp() values to avoid expensive calculations in hot loops
constexpr int EXP_LUT_SIZE = 512;
constexpr double EXP_LUT_MAX = 8.0;  // Max decay value we'll encounter
static float exp_lut[EXP_LUT_SIZE];
static bool exp_lut_initialized = false;

// Fast exp lookup with linear interpolation
inline float fast_exp(double x) {
    if (x <= 0.0) return 1.0f;
    if (x >= EXP_LUT_MAX) return 0.0f;
    double idx_f = (x / EXP_LUT_MAX) * (EXP_LUT_SIZE - 1);
    int idx = static_cast<int>(idx_f);
    if (idx >= EXP_LUT_SIZE - 1) return exp_lut[EXP_LUT_SIZE - 1];
    float t = static_cast<float>(idx_f - idx);
    return exp_lut[idx] * (1.0f - t) + exp_lut[idx + 1] * t;
}

// Initialize lookup table
inline void init_exp_lut() {
    if (exp_lut_initialized) return;
    for (int i = 0; i < EXP_LUT_SIZE; ++i) {
        double x = (static_cast<double>(i) / (EXP_LUT_SIZE - 1)) * EXP_LUT_MAX;
        exp_lut[i] = static_cast<float>(std::exp(-x));
    }
    exp_lut_initialized = true;
}

// --- Trigonometric lookup table for arc drawing ---
// Pre-computed sin/cos values to avoid expensive trig calls in arc rendering
constexpr int TRIG_LUT_SIZE = 1024;  // 1024 entries for 360 degrees (0.35° precision)
static float sin_lut[TRIG_LUT_SIZE];
static float cos_lut[TRIG_LUT_SIZE];
static bool trig_lut_initialized = false;

// Fast sin lookup with linear interpolation
inline float fast_sin(double angle) {
    constexpr double kTwoPi = 6.283185307179586;
    // Normalize angle to [0, 2π)
    double normalized = angle - std::floor(angle / kTwoPi) * kTwoPi;
    if (normalized < 0.0) normalized += kTwoPi;

    double idx_f = (normalized / kTwoPi) * (TRIG_LUT_SIZE - 1);
    int idx = static_cast<int>(idx_f);
    if (idx >= TRIG_LUT_SIZE - 1) return sin_lut[0];  // Wrap around

    float t = static_cast<float>(idx_f - idx);
    return sin_lut[idx] * (1.0f - t) + sin_lut[idx + 1] * t;
}

// Fast cos lookup with linear interpolation
inline float fast_cos(double angle) {
    constexpr double kTwoPi = 6.283185307179586;
    double normalized = angle - std::floor(angle / kTwoPi) * kTwoPi;
    if (normalized < 0.0) normalized += kTwoPi;

    double idx_f = (normalized / kTwoPi) * (TRIG_LUT_SIZE - 1);
    int idx = static_cast<int>(idx_f);
    if (idx >= TRIG_LUT_SIZE - 1) return cos_lut[0];

    float t = static_cast<float>(idx_f - idx);
    return cos_lut[idx] * (1.0f - t) + cos_lut[idx + 1] * t;
}

// Initialize trig lookup tables
inline void init_trig_lut() {
    if (trig_lut_initialized) return;
    constexpr double kTwoPi = 6.283185307179586;
    for (int i = 0; i < TRIG_LUT_SIZE; ++i) {
        double angle = (static_cast<double>(i) / (TRIG_LUT_SIZE - 1)) * kTwoPi;
        sin_lut[i] = static_cast<float>(std::sin(angle));
        cos_lut[i] = static_cast<float>(std::cos(angle));
    }
    trig_lut_initialized = true;
}

namespace Layout {
    constexpr int HEADER_HEIGHT = 42;
    constexpr int FOOTER_HEIGHT = 0;
    constexpr int MARGIN = 12;
    constexpr int GAP = 10;
    constexpr int LEFT_PANEL_WIDTH = 310;
    constexpr int VITALS_PANEL_HEIGHT = 160;
}

// --- Helper Functions ---

// Interpolate between two RGB565 colors
static color_t interpolate_color(color_t c1, color_t c2, float t) {
    uint8_t r1 = (c1 >> 11) & 0x1F;
    uint8_t g1 = (c1 >> 5) & 0x3F;
    uint8_t b1 = (c1 >> 0) & 0x1F;

    uint8_t r2 = (c2 >> 11) & 0x1F;
    uint8_t g2 = (c2 >> 5) & 0x3F;
    uint8_t b2 = (c2 >> 0) & 0x1F;

    uint8_t r = r1 + static_cast<int>((r2 - r1) * t);
    uint8_t g = g1 + static_cast<int>((g2 - g1) * t);
    uint8_t b = b1 + static_cast<int>((b2 - b1) * t);

    return (r << 11) | (g << 5) | b;
}

static inline void rgb565_to_rgb888(color_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
    b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
}

static inline color_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (r * 31 / 255) & 0x1F;
    uint16_t gg = (g * 63 / 255) & 0x3F;
    uint16_t bb = (b * 31 / 255) & 0x1F;
    return static_cast<color_t>((rr << 11) | (gg << 5) | bb);
}

static double clamp(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static color_t scale_color(color_t c, float factor) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = (c >> 0) & 0x1F;
    r = static_cast<uint8_t>(std::clamp(static_cast<int>(r * factor), 0, 0x1F));
    g = static_cast<uint8_t>(std::clamp(static_cast<int>(g * factor), 0, 0x3F));
    b = static_cast<uint8_t>(std::clamp(static_cast<int>(b * factor), 0, 0x1F));
    return (r << 11) | (g << 5) | b;
}

// --- Renderer Implementation ---

Renderer::Renderer() {
    // Initialize exponential decay lookup table for gradient fills
    init_exp_lut();
    // Initialize trigonometric lookup tables for arc drawing
    init_trig_lut();

    idle_t_ = 0.0f;
    net1_scale_max_ = 0.0;
    net2_scale_max_ = 0.0;
    theme_name_ = getenv_string("LCD_THEME", "neutral");
    if (THEMES.count(theme_name_)) {
        current_theme_ = THEMES.at(theme_name_);
    } else {
        current_theme_ = THEMES.at("neutral");
        theme_name_ = "neutral";
    }

    grid_enabled_ = getenv_bool("LCD_GRID", false);
    band_enabled_ = getenv_bool("LCD_BAND", false);

    loadFont(getenv_string("LCD_FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"), 16.0f);

    history_size_ = (DISPLAY_WIDTH >= 400 ? 120 : 60);

    history_cpu_.clear();
    history_temp_.clear();
    history_net1_.clear();
    history_net2_.clear();

    ticker_text_ = "";
    ticker_offset_px_ = 0.0f;
    ticker_speed_px_ = 1.0f;

    net_autoscale_ = getenv_bool("LCD_NET_AUTOSCALE", net_autoscale_);
    net_autoscale_pctl_ = getenv_double("LCD_NET_AUTOSCALE_PCTL", net_autoscale_pctl_);
    net_autoscale_min_ = getenv_double("LCD_NET_AUTOSCALE_MIN", net_autoscale_min_);
    net_autoscale_max_ = getenv_double("LCD_NET_AUTOSCALE_MAX", net_autoscale_max_);
    net_autoscale_ema_ = getenv_double("LCD_NET_AUTOSCALE_EMA", net_autoscale_ema_);

    // Sparkline smoothing settings
    sparkline_smooth_ = getenv_bool("LCD_SPARKLINE_SMOOTH", sparkline_smooth_);
    sparkline_smooth_alpha_ = getenv_double("LCD_SPARKLINE_SMOOTH_ALPHA", sparkline_smooth_alpha_);
    net1_smooth_ = 0.0;
    net2_smooth_ = 0.0;
    net1_initialized_ = false;
    net2_initialized_ = false;

    // Sparkline visual enhancements
    sparkline_pulse_ = getenv_bool("LCD_SPARKLINE_PULSE", sparkline_pulse_);
    sparkline_peak_highlight_ = getenv_bool("LCD_SPARKLINE_PEAK_HIGHLIGHT", sparkline_peak_highlight_);
    sparkline_gradient_line_ = getenv_bool("LCD_SPARKLINE_GRADIENT_LINE", sparkline_gradient_line_);
    sparkline_particles_ = getenv_bool("LCD_SPARKLINE_PARTICLES", sparkline_particles_);
    sparkline_enhanced_fill_ = getenv_bool("LCD_SPARKLINE_ENHANCED_FILL", sparkline_enhanced_fill_);
    sparkline_dynamic_width_ = getenv_bool("LCD_SPARKLINE_DYNAMIC_WIDTH", sparkline_dynamic_width_);
    sparkline_baseline_shimmer_ = getenv_bool("LCD_SPARKLINE_BASELINE_SHIMMER", sparkline_baseline_shimmer_);
    sparkline_shadow_ = getenv_bool("LCD_SPARKLINE_SHADOW", sparkline_shadow_);
    sparkline_color_zones_ = getenv_bool("LCD_SPARKLINE_COLOR_ZONES", sparkline_color_zones_);
    sparkline_smooth_transitions_ = getenv_bool("LCD_SPARKLINE_SMOOTH_TRANSITIONS", sparkline_smooth_transitions_);
}

Renderer::~Renderer() {
    if (font_info_) {
        delete static_cast<stbtt_fontinfo*>(font_info_);
    }
}

void Renderer::loadFont(const std::string& font_path, float /*size*/) {
    // Освободить предыдущий шрифт если был
    if (font_info_) {
        delete static_cast<stbtt_fontinfo*>(font_info_);
        font_info_ = nullptr;
    }
    font_buffer_.clear();

    std::ifstream file(font_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open font file: " << font_path << std::endl;
        return;
    }
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    font_buffer_.resize(file_size);
    if (!file.read((char*)font_buffer_.data(), file_size)) {
        std::cerr << "Failed to read font file" << std::endl;
        return;
    }
    font_info_ = new stbtt_fontinfo();
    if (!stbtt_InitFont(static_cast<stbtt_fontinfo*>(font_info_), font_buffer_.data(), 0)) {
        std::cerr << "Failed to initialize font" << std::endl;
        delete static_cast<stbtt_fontinfo*>(font_info_);
        font_info_ = nullptr;
    }
}

int Renderer::measureTextWidth(const std::string& text, float size) {
    if (!font_info_) return 0;
    auto* info = static_cast<stbtt_fontinfo*>(font_info_);
    float scale = stbtt_ScaleForPixelHeight(info, size);
    int width = 0;
    for (char c : text) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, c, &advance, &lsb);
        width += static_cast<int>(advance * scale);
    }
    return width;
}

void Renderer::UpdateHistories(const SystemMetrics& metrics) {
    auto push = [&](std::deque<double>& dq, double v) {
        if (dq.size() >= history_size_) dq.pop_front();
        dq.push_back(v);
    };
    push(history_cpu_, metrics.cpu_usage);
    push(history_temp_, metrics.temp);

    // Apply EMA smoothing to network data for smoother sparklines
    double net1_value = metrics.net1_mbps;
    double net2_value = metrics.net2_mbps;

    if (sparkline_smooth_) {
        // Initialize on first run
        if (!net1_initialized_) {
            net1_smooth_ = net1_value;
            net1_initialized_ = true;
        } else {
            // EMA formula: smoothed = alpha * raw + (1 - alpha) * prev_smoothed
            net1_smooth_ = sparkline_smooth_alpha_ * net1_value + (1.0 - sparkline_smooth_alpha_) * net1_smooth_;
        }

        if (!net2_initialized_) {
            net2_smooth_ = net2_value;
            net2_initialized_ = true;
        } else {
            net2_smooth_ = sparkline_smooth_alpha_ * net2_value + (1.0 - sparkline_smooth_alpha_) * net2_smooth_;
        }

        net1_value = net1_smooth_;
        net2_value = net2_smooth_;
    }

    push(history_net1_, net1_value);
    push(history_net2_, net2_value);
}

void Renderer::UpdateTickerText(const SystemMetrics& metrics) {
    std::string wan = "WAN " + metrics.get_wan_status();
    std::string wg = (metrics.wg_active_peers >= 0)
                         ? "WG " + std::to_string(metrics.wg_active_peers)
                         : "WG -";
    std::string n1 = "NET1 " + formatNet(metrics.net1_mbps);
    std::string n2 = "NET2 " + formatNet(metrics.net2_mbps);
    std::string docker = (metrics.docker_running >= 0)
                             ? "Docker " + std::to_string(metrics.docker_running)
                             : "Docker -";
    std::string disk = (metrics.disk_percent >= 0)
                           ? "Disk " + std::to_string(metrics.disk_percent) + "%"
                           : "Disk -";

    ticker_text_ = wan + " | " + wg + " | " + n1 + " | " + n2 + " | " + docker + " | " + disk;
}

color_t Renderer::pickStateColor(double value, const std::string& key) const {
    auto it = THRESHOLDS.find(key);
    if (it == THRESHOLDS.end()) return current_theme_.state_low;
    color_t vivid_low = RGB(0, 255, 80);
    const auto& t = it->second;
    if (value < t[0]) return vivid_low;
    if (value < t[1]) return current_theme_.state_medium;
    return current_theme_.state_high;
}

color_t Renderer::dimColor(color_t c) const {
    return interpolate_color(c, scale_color(c, 0.6f), idle_t_);
}

std::string Renderer::formatNet(double mbps) const {
    if (mbps >= 1000.0) {
        double gb = mbps / 1000.0;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fG", gb);
        return std::string(buf);
    }
    if (mbps >= 1.0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0fM", mbps);
        return std::string(buf);
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1fM", mbps);
    return std::string(buf);
}

std::string Renderer::formatUptime(int seconds) const {
    if (seconds < 60) return std::to_string(seconds) + "s";
    int minutes = seconds / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";
    int hours = minutes / 60;
    int rem = minutes % 60;
    if (hours < 24) return std::to_string(hours) + "h " + std::to_string(rem) + "m";
    int days = hours / 24;
    int remh = hours % 24;
    return std::to_string(days) + "d " + std::to_string(remh) + "h";
}

double Renderer::computeNetScale(const std::deque<double>& history, double& smooth_max) {
    if (history.empty()) return net_autoscale_max_;
    scratch_values_.clear();
    scratch_values_.insert(scratch_values_.end(), history.begin(), history.end());
    std::sort(scratch_values_.begin(), scratch_values_.end());
    double p = clamp(net_autoscale_pctl_, 0.0, 100.0) / 100.0;
    size_t idx = static_cast<size_t>(std::round(p * (scratch_values_.size() - 1)));
    if (idx >= scratch_values_.size()) idx = scratch_values_.size() - 1;
    double raw = scratch_values_[idx];
    raw = std::max(raw, net_autoscale_min_);
    raw = std::min(raw, net_autoscale_max_);
    if (smooth_max <= 0.0) {
        smooth_max = raw;
    } else {
        smooth_max = smooth_max * (1.0 - net_autoscale_ema_) + raw * net_autoscale_ema_;
    }
    if (smooth_max < net_autoscale_min_) smooth_max = net_autoscale_min_;
    if (smooth_max > net_autoscale_max_) smooth_max = net_autoscale_max_;
    return smooth_max;
}

void Renderer::Render(const SystemMetrics& metrics,
                      const PrinterMetrics& printer,
                      AnimationEngine& animator,
                      const IdleModeController& idle_controller,
                      double time_sec,
                      std::vector<uint16_t>& buffer) {
    target_buffer_ = &buffer;
    idle_t_ = static_cast<float>(idle_controller.get_transition_progress());
    double idle_t = idle_t_;
    color_t bg_top = interpolate_color(current_theme_.bg_top_active, current_theme_.bg_top_idle, idle_t);

    // Static background (no gradient)
    if (buffer.size() != DISPLAY_WIDTH * DISPLAY_HEIGHT) {
        buffer.assign(DISPLAY_WIDTH * DISPLAY_HEIGHT, bg_top);
    } else {
        std::fill(buffer.begin(), buffer.end(), bg_top);
    }

    int header_h = Layout::HEADER_HEIGHT;
    int footer_h = Layout::FOOTER_HEIGHT;
    int margin = Layout::MARGIN;
    int gap = Layout::GAP;
    int left_w = Layout::LEFT_PANEL_WIDTH;
    int right_w = DISPLAY_WIDTH - 2 * margin - gap - left_w;

    int content_y0 = header_h + 10;
    int content_y1 = DISPLAY_HEIGHT - footer_h - 8;
    int graph_h = (content_y1 - content_y0 - gap) / 2;

    int g1_x = margin;
    int g1_y = content_y0;
    int g1_w = left_w;
    int g1_h = graph_h;

    int g2_x = margin;
    int g2_y = g1_y + g1_h + gap;
    int g2_w = left_w;
    int g2_h = graph_h;

    int r1_x = g1_x + g1_w + gap;
    int r1_y = content_y0;
    int r1_w = right_w;
    int r1_h = content_y1 - content_y0;

    double cpu = animator.get("cpu", metrics.cpu_usage);
    double temp = animator.get("temp", metrics.temp);
    double net1 = animator.get("net1", metrics.net1_mbps);
    double net2 = animator.get("net2", metrics.net2_mbps);

    // Fixed palette for series (do not depend on state)
    color_t series_net1 = dimColor(RGB(0, 210, 255));    // vivid cyan/blue
    color_t series_net2 = dimColor(RGB(255, 220, 0));    // vivid yellow
    color_t series_cpu  = dimColor(RGB(0, 255, 80));     // vivid green
    color_t series_temp = dimColor(RGB(255, 140, 80));   // warm orange

    // Determine Print Screen eligibility and toggle MAIN/PRINT with asymmetric durations
    double now = time_sec;
    bool print_active = (printer.state == "printing" || printer.state == "paused");
    bool print_eligible = false;
    if (print_active) {
        print_eligible = true;
    } else if (printer.had_job && (now - printer.last_active_ts) < 60.0) {
        print_eligible = true;
    }

    if (!print_eligible) {
        screen_mode_ = ScreenMode::MAIN;
        last_screen_switch_ts_ = now;
    } else {
        const double main_duration  = 180.0; // 3 минуты основной экран
        const double print_duration = 30.0;  // 30 секунд Print Screen

        if (!last_print_eligible_) {
            screen_mode_ = ScreenMode::MAIN;
            last_screen_switch_ts_ = now;
        } else {
            double elapsed = now - last_screen_switch_ts_;
            double current_limit = (screen_mode_ == ScreenMode::MAIN) ? main_duration : print_duration;
            if (elapsed >= current_limit) {
                screen_mode_ = (screen_mode_ == ScreenMode::MAIN) ? ScreenMode::PRINT : ScreenMode::MAIN;
                last_screen_switch_ts_ = now;
            }
        }
    }
    last_print_eligible_ = print_eligible;

    if (print_eligible && screen_mode_ == ScreenMode::PRINT) {
        drawPrintScreen(printer, animator, time_sec);
        return;
    }

    drawHeader(0, 0, DISPLAY_WIDTH, header_h, metrics);

    double net_hist_max = 2500.0;
    if (net_autoscale_) {
        double n1 = computeNetScale(history_net1_, net1_scale_max_);
        double n2 = computeNetScale(history_net2_, net2_scale_max_);
        net_hist_max = std::max(n1, n2);
    }
    std::string net_values = "N1 " + formatNet(net1) + "  N2 " + formatNet(net2);
    drawGraphPanel(g1_x, g1_y, g1_w, g1_h,
                   "Network Throughput", net_values,
                   "last 120s | auto-scale",
                   "NET1 Mbps", "NET2 Mbps",
                   history_net1_, history_net2_, 0.0, net_hist_max,
                   series_net1, series_net2,
                   MetricType::NET1, MetricType::NET2, animator, time_sec);

    std::string cpu_values = "CPU " + std::to_string(static_cast<int>(cpu)) + "%  TEMP " +
                             std::to_string(static_cast<int>(temp)) + "C";
    drawGraphPanel(g2_x, g2_y, g2_w, g2_h,
                   "CPU & TEMP", cpu_values,
                   "last 120s | 0-100",
                   "CPU %", "TEMP C",
                   history_cpu_, history_temp_, 0.0, 100.0,
                   series_cpu, series_temp,
                   MetricType::CPU, MetricType::TEMP, animator, time_sec);

    double mem = metrics.mem_percent;
    color_t mem_color = pickStateColor(mem, "ram");
    std::string wan_status = metrics.get_wan_status();
    drawVitalsPanel(r1_x, r1_y, r1_w, r1_h, cpu, temp, mem, net1, wan_status,
                    pickStateColor(cpu, "cpu"), pickStateColor(temp, "temp"), mem_color,
                    pickStateColor(net1, "net"));
    // No Services panel and no Footer ticker in simplified layout
}

void Renderer::drawText(const std::string& text, int x, int y, color_t color, float size) {
    drawTextClipped(text, x, y, color, size, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void Renderer::drawTextClipped(const std::string& text, int x, int y, color_t color, float size,
                               int clip_x, int clip_y, int clip_w, int clip_h) {
    if (!target_buffer_) return;
    if (!font_info_) return;
    auto* info = static_cast<stbtt_fontinfo*>(font_info_);
    float scale = stbtt_ScaleForPixelHeight(info, size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
    y += static_cast<int>(ascent * scale);

    for (char c : text) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, c, &advance, &lsb);
        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(info, c, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int w = c_x2 - c_x1;
        int h = c_y2 - c_y1;
        if (w > 0 && h > 0) {
            thread_local std::vector<uint8_t> bitmap;
            bitmap.resize(static_cast<size_t>(w * h));
            stbtt_MakeCodepointBitmap(info, bitmap.data(), w, h, w, scale, scale, c);
            for (int j = 0; j < h; ++j) {
                for (int i = 0; i < w; ++i) {
                    uint8_t alpha = bitmap[j * w + i];
                    if (alpha > 0) {
                        int px = x + c_x1 + i;
                        int py = y + c_y1 + j;
                        if (px >= clip_x && px < (clip_x + clip_w) &&
                            py >= clip_y && py < (clip_y + clip_h) &&
                            px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                            (*target_buffer_)[py * DISPLAY_WIDTH + px] = color;
                        }
                    }
                }
            }
        }
        x += static_cast<int>(advance * scale);
    }
}

void Renderer::drawRect(int x, int y, int w, int h, color_t color) {
    if (!target_buffer_) return;
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= DISPLAY_HEIGHT) continue;
        for (int i = x; i < x + w; ++i) {
            if (i >= 0 && i < DISPLAY_WIDTH) {
                (*target_buffer_)[j * DISPLAY_WIDTH + i] = color;
            }
        }
    }
}

void Renderer::drawLine(int x0, int y0, int x1, int y1, color_t color) {
    if (!target_buffer_) return;
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        if (x0 >= 0 && x0 < DISPLAY_WIDTH && y0 >= 0 && y0 < DISPLAY_HEIGHT) {
            (*target_buffer_)[y0 * DISPLAY_WIDTH + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Renderer::drawCircle(int cx, int cy, int r, color_t color) {
    if (!target_buffer_) return;
    int x = -r, y = 0, err = 2 - 2 * r;
    do {
        if (cx - x >= 0 && cx - x < DISPLAY_WIDTH && cy + y >= 0 && cy + y < DISPLAY_HEIGHT) (*target_buffer_)[(cy + y) * DISPLAY_WIDTH + (cx - x)] = color;
        if (cx - y >= 0 && cx - y < DISPLAY_WIDTH && cy - x >= 0 && cy - x < DISPLAY_HEIGHT) (*target_buffer_)[(cy - x) * DISPLAY_WIDTH + (cx - y)] = color;
        if (cx + x >= 0 && cx + x < DISPLAY_WIDTH && cy - y >= 0 && cy - y < DISPLAY_HEIGHT) (*target_buffer_)[(cy - y) * DISPLAY_WIDTH + (cx + x)] = color;
        if (cx + y >= 0 && cx + y < DISPLAY_WIDTH && cy + x >= 0 && cy + x < DISPLAY_HEIGHT) (*target_buffer_)[(cy + x) * DISPLAY_WIDTH + (cx + y)] = color;
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x || err > y) err += ++x * 2 + 1;
    } while (x < 0);
}

void Renderer::drawFilledCircle(int cx, int cy, int r, color_t color) {
    if (!target_buffer_) return;
    if (r <= 0) return;

    int r2 = r * r;
    for (int y = -r; y <= r; ++y) {
        int y2 = y * y;
        int dx;

        // For small radii (most common case), use integer arithmetic instead of sqrt
        // This avoids expensive floating-point sqrt call in hot loop
        if (r <= 10) {
            int max_dx2 = r2 - y2;
            if (max_dx2 < 0) {
                dx = 0;
            } else {
                // Find largest dx where dx^2 <= max_dx2 using integer operations
                dx = 0;
                while ((dx + 1) * (dx + 1) <= max_dx2) {
                    ++dx;
                }
            }
        } else {
            // For larger radii, sqrt is still efficient enough
            dx = static_cast<int>(std::sqrt(std::max(0, r2 - y2)));
        }

        int x0 = cx - dx;
        int x1 = cx + dx;
        int py = cy + y;
        if (py < 0 || py >= DISPLAY_HEIGHT) continue;
        if (x0 < 0) x0 = 0;
        if (x1 >= DISPLAY_WIDTH) x1 = DISPLAY_WIDTH - 1;
        for (int x = x0; x <= x1; ++x) {
            (*target_buffer_)[py * DISPLAY_WIDTH + x] = color;
        }
    }
}

void Renderer::drawRoundedRect(int x, int y, int w, int h, int r, color_t fill, color_t border) {
    if (w <= 0 || h <= 0) return;
    int rr = std::min({r, w / 2, h / 2});
    auto fillRounded = [&](int fx, int fy, int fw, int fh, int fr, color_t col) {
        if (fw <= 0 || fh <= 0) return;
        int frr = std::min({fr, fw / 2, fh / 2});
        if (frr <= 0) {
            drawRect(fx, fy, fw, fh, col);
            return;
        }
        drawRect(fx + frr, fy, fw - 2 * frr, fh, col);
        drawRect(fx, fy + frr, frr, fh - 2 * frr, col);
        drawRect(fx + fw - frr, fy + frr, frr, fh - 2 * frr, col);
        drawFilledCircle(fx + frr, fy + frr, frr, col);
        drawFilledCircle(fx + fw - frr - 1, fy + frr, frr, col);
        drawFilledCircle(fx + frr, fy + fh - frr - 1, frr, col);
        drawFilledCircle(fx + fw - frr - 1, fy + fh - frr - 1, frr, col);
    };

    // Outer fill (border color)
    fillRounded(x, y, w, h, rr, border);
    // Inner fill
    if (w > 2 && h > 2) {
        fillRounded(x + 1, y + 1, w - 2, h - 2, std::max(0, rr - 1), fill);
    }
}

void Renderer::drawVGradient(int x, int y, int w, int h, color_t c1, color_t c2) {
    if (!target_buffer_) return;
    if (h <= 1) {
        drawRect(x, y, w, h, c1);
        return;
    }
    for(int i = 0; i < h; ++i) {
        float t = (float)i / (h - 1);
        color_t grad_color = interpolate_color(c1, c2, t);
        for(int j = 0; j < w; ++j) {
            int px = x + j;
            int py = y + i;
            if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                (*target_buffer_)[py * DISPLAY_WIDTH + px] = grad_color;
            }
        }
    }
}

void Renderer::drawGrid(int x, int y, int w, int h, int cell, int offset_x, int offset_y, color_t color) {
    for (int gx = x + offset_x; gx < x + w; gx += cell) {
        drawLine(gx, y, gx, y + h - 1, color);
    }
    for (int gy = y + offset_y; gy < y + h; gy += cell) {
        drawLine(x, gy, x + w - 1, gy, color);
    }
}

void Renderer::drawIcon(const std::string& name, int x, int y, int size, color_t color) {
    if (size < 10) size = 10;
    const double scale = static_cast<double>(size) / 14.0;
    auto sx = [&](int v) { return x + static_cast<int>(std::round(v * scale)); };
    auto sy = [&](int v) { return y + static_cast<int>(std::round(v * scale)); };

    if (name == "cpu") {
        // Chip body
        int w = std::max(1, static_cast<int>(std::round(10 * scale)));
        int h = std::max(1, static_cast<int>(std::round(10 * scale)));
        drawRect(sx(2), sy(2), w, h, color);
        drawRect(sx(4), sy(4), std::max(1, static_cast<int>(std::round(6 * scale))),
                 std::max(1, static_cast<int>(std::round(6 * scale))), current_theme_.bar_bg);
        // Pins (top/bottom)
        for (int i = 0; i < 3; ++i) {
            int px = sx(3 + i * 3);
            drawRect(px, sy(0), std::max(1, static_cast<int>(std::round(2 * scale))),
                     std::max(1, static_cast<int>(std::round(2 * scale))), color);
            drawRect(px, sy(12), std::max(1, static_cast<int>(std::round(2 * scale))),
                     std::max(1, static_cast<int>(std::round(2 * scale))), color);
        }
        // Pins (left/right)
        for (int i = 0; i < 3; ++i) {
            int py = sy(3 + i * 3);
            drawRect(sx(0), py, std::max(1, static_cast<int>(std::round(2 * scale))),
                     std::max(1, static_cast<int>(std::round(2 * scale))), color);
            drawRect(sx(12), py, std::max(1, static_cast<int>(std::round(2 * scale))),
                     std::max(1, static_cast<int>(std::round(2 * scale))), color);
        }
    } else if (name == "temp") {
        // Thermometer
        drawLine(sx(6), sy(2), sx(6), sy(9), color);
        drawLine(sx(7), sy(2), sx(7), sy(9), color);
        drawCircle(sx(6), sy(11), std::max(2, static_cast<int>(std::round(3 * scale))), color);
    } else { // net
        // Up/down arrows
        drawLine(sx(2), sy(10), sx(2), sy(4), color);
        drawLine(sx(2), sy(4), sx(4), sy(6), color);
        drawLine(sx(2), sy(4), sx(0), sy(6), color);
        drawLine(sx(10), sy(4), sx(10), sy(10), color);
        drawLine(sx(10), sy(10), sx(8), sy(8), color);
        drawLine(sx(10), sy(10), sx(12), sy(8), color);
    }
}

void Renderer::drawSparkline(int x, int y, int w, int h,
                             const std::vector<double>& data,
                             double min_val, double max_val,
                             color_t color, color_t bg_color, int line_width,
                             MetricType metric_type,
                             AnimationEngine& animator) {
    if (data.size() < 2) return;

    // Draw background
    drawRect(x, y, w, h, bg_color);

    // Select parameters based on metric type
    double zoom_start = SparklineZoom::NET_ZOOM_START;
    double zoom_end = SparklineZoom::NET_ZOOM_END;
    double min_range = SparklineZoom::MIN_RANGE_NET;
    std::string anim_key = "net1_gamma";
    std::string pulse_key = "net1_pulse";

    switch (metric_type) {
        case MetricType::CPU:
            zoom_start = SparklineZoom::CPU_ZOOM_START;
            zoom_end = SparklineZoom::CPU_ZOOM_END;
            min_range = SparklineZoom::MIN_RANGE_CPU;
            anim_key = "cpu_gamma";
            pulse_key = "cpu_pulse";
            break;
        case MetricType::TEMP:
            zoom_start = SparklineZoom::TEMP_ZOOM_START;
            zoom_end = SparklineZoom::TEMP_ZOOM_END;
            min_range = SparklineZoom::MIN_RANGE_TEMP;
            anim_key = "temp_gamma";
            pulse_key = "temp_pulse";
            break;
        case MetricType::NET1:
            zoom_start = SparklineZoom::NET_ZOOM_START;
            zoom_end = SparklineZoom::NET_ZOOM_END;
            min_range = SparklineZoom::MIN_RANGE_NET;
            anim_key = "net1_gamma";
            pulse_key = "net1_pulse";
            break;
        case MetricType::NET2:
            zoom_start = SparklineZoom::NET_ZOOM_START;
            zoom_end = SparklineZoom::NET_ZOOM_END;
            min_range = SparklineZoom::MIN_RANGE_NET;
            anim_key = "net2_gamma";
            pulse_key = "net2_pulse";
            break;
        default:
            break;
    }

    // Calculate actual data range for flat detection
    double data_min = *std::min_element(data.begin(), data.end());
    double data_max = *std::max_element(data.begin(), data.end());
    double data_range = data_max - data_min;
    double scale_range = max_val - min_val;
    double relative_threshold = 0.03 * scale_range;
    bool is_flat = (data_range < std::max(relative_threshold, min_range * 0.2));

    // Calculate zoom factor using blended reference
    double ref = 0.7 * max_val + 0.3 * data.back();
    double t = clamp((ref - zoom_start) / (zoom_end - zoom_start + 1e-9), 0.0, 1.0);

    // Smooth gamma transition via AnimationEngine
    double target_gamma = SparklineZoom::GAMMA_MIN + t * (SparklineZoom::GAMMA_MAX - SparklineZoom::GAMMA_MIN);
    animator.set_target(anim_key, target_gamma);
    double gamma = animator.get(anim_key, 1.0);

    // Adaptive baseline
    double baseline_frac = 0.85 - t * 0.10;
    int baseline_y_pos = y + static_cast<int>(h * baseline_frac);

    // Build points with normalized values
    std::vector<std::pair<int, int>> points;
    std::vector<double> normalized_values;
    points.reserve(data.size());
    normalized_values.reserve(data.size());

    double flat_v = 0.5;
    if (is_flat) {
        double v0 = clamp((data.back() - min_val) / (max_val - min_val + 1e-9), 0.0, 1.0);
        flat_v = 0.15 + 0.7 * v0;
    }

    for (size_t i = 0; i < data.size(); ++i) {
        double v;
        if (is_flat) {
            v = flat_v;
        } else {
            v = clamp((data[i] - min_val) / (max_val - min_val + 1e-9), 0.0, 1.0);
            v = std::pow(v, gamma);
        }
        normalized_values.push_back(v);
        int px = x + 1 + static_cast<int>((static_cast<double>(i) / (data.size() - 1)) * (w - 2));
        int py = y + h - 1 - static_cast<int>(v * (h - 2));
        points.emplace_back(px, py);
    }

    // Find local peaks for highlighting
    std::vector<size_t> peak_indices;
    if (sparkline_peak_highlight_ && !is_flat && data.size() >= 5) {
        for (size_t i = 2; i < data.size() - 2; ++i) {
            if (data[i] > data[i-1] && data[i] > data[i-2] &&
                data[i] > data[i+1] && data[i] > data[i+2] &&
                normalized_values[i] > 0.6) {
                peak_indices.push_back(i);
            }
        }
    }

    // ===== EFFECT 8: Shadow/Depth =====
    if (sparkline_shadow_ && target_buffer_) {
        color_t shadow_color = scale_color(color, 0.25f);
        uint8_t sr, sg, sb;
        rgb565_to_rgb888(shadow_color, sr, sg, sb);
        uint8_t br, bg, bb;
        rgb565_to_rgb888(bg_color, br, bg, bb);

        for (size_t i = 1; i < points.size(); ++i) {
            int x0 = points[i - 1].first;
            int y0 = points[i - 1].second + 2; // Shadow offset
            int x1 = points[i].first;
            int y1 = points[i].second + 2;
            if (x0 > x1) {
                std::swap(x0, x1);
                std::swap(y0, y1);
            }
            int dx = std::max(1, x1 - x0);
            for (int xi = x0; xi <= x1; ++xi) {
                double tseg = static_cast<double>(xi - x0) / dx;
                int yi = static_cast<int>(std::round(y0 + (y1 - y0) * tseg));
                if (xi >= 0 && xi < DISPLAY_WIDTH && yi >= 0 && yi < DISPLAY_HEIGHT) {
                    size_t idx = static_cast<size_t>(yi) * DISPLAY_WIDTH + static_cast<size_t>(xi);
                    double alpha = 0.3;
                    uint8_t r = static_cast<uint8_t>(sr * alpha + br * (1.0 - alpha));
                    uint8_t g = static_cast<uint8_t>(sg * alpha + bg * (1.0 - alpha));
                    uint8_t b = static_cast<uint8_t>(sb * alpha + bb * (1.0 - alpha));
                    (*target_buffer_)[idx] = rgb888_to_rgb565(r, g, b);
                }
            }
        }
    }

    // ===== EFFECT 5: Enhanced Fill Gradient (two-stage) =====
    if (target_buffer_) {
        uint8_t fr, fg, fb;
        uint8_t br, bg, bb;
        rgb565_to_rgb888(color, fr, fg, fb);
        rgb565_to_rgb888(bg_color, br, bg, bb);
        int bottom_y = y + h - 1;

        for (size_t i = 0; i + 1 < points.size(); ++i) {
            auto [x0, y0] = points[i];
            auto [x1, y1] = points[i + 1];
            if (x0 > x1) {
                std::swap(x0, x1);
                std::swap(y0, y1);
            }
            int dx = std::max(1, x1 - x0);
            for (int xi = x0; xi <= x1; ++xi) {
                double tseg = static_cast<double>(xi - x0) / dx;
                double top_f = y0 + (y1 - y0) * tseg;
                int top = static_cast<int>(std::round(top_f));
                if (top < y) top = y;
                if (top > bottom_y) top = bottom_y;
                double denom = std::max(1.0, static_cast<double>(bottom_y - top));

                for (int py = top; py <= bottom_y; ++py) {
                    double norm = (py - top_f) / denom;
                    double alpha;

                    if (sparkline_enhanced_fill_) {
                        // Two-stage gradient: bright near line, then exponential fade
                        if (norm < 0.2) {
                            alpha = FILL_ALPHA_SPARK * (1.0 - norm * 2.0);
                        } else {
                            alpha = FILL_ALPHA_SPARK * 0.6 * fast_exp(FILL_DECAY_SPARK * (norm - 0.2));
                        }
                    } else {
                        alpha = FILL_ALPHA_SPARK * fast_exp(FILL_DECAY_SPARK * norm);
                    }

                    if (alpha < 0.001) continue;
                    if (xi < 0 || xi >= DISPLAY_WIDTH || py < 0 || py >= DISPLAY_HEIGHT) continue;

                    size_t idx = static_cast<size_t>(py) * DISPLAY_WIDTH + static_cast<size_t>(xi);

                    // EFFECT 9: Color zones - slightly shift hue based on value
                    uint8_t fill_r = fr, fill_g = fg, fill_b = fb;
                    if (sparkline_color_zones_ && i < normalized_values.size()) {
                        double val = normalized_values[i];
                        if (val < 0.33) {
                            // Low zone: cooler tones
                            fill_r = static_cast<uint8_t>(fr * 0.85);
                            fill_g = static_cast<uint8_t>(fg * 1.0);
                            fill_b = static_cast<uint8_t>(fb * 1.15);
                        } else if (val > 0.66) {
                            // High zone: warmer tones
                            fill_r = static_cast<uint8_t>(std::min(255, static_cast<int>(fr * 1.15)));
                            fill_g = static_cast<uint8_t>(fg * 0.95);
                            fill_b = static_cast<uint8_t>(fb * 0.85);
                        }
                    }

                    uint8_t r = static_cast<uint8_t>(fill_r * alpha + br * (1.0 - alpha));
                    uint8_t g = static_cast<uint8_t>(fill_g * alpha + bg * (1.0 - alpha));
                    uint8_t b = static_cast<uint8_t>(fill_b * alpha + bb * (1.0 - alpha));
                    (*target_buffer_)[idx] = rgb888_to_rgb565(r, g, b);
                }
            }
        }
    }

    // ===== EFFECT 3: Gradient Line + EFFECT 6: Dynamic Line Thickness =====
    for (size_t i = 1; i < points.size(); ++i) {
        int x0 = points[i - 1].first;
        int y0 = points[i - 1].second;
        int x1 = points[i].first;
        int y1 = points[i].second;
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }

        double val_prev = normalized_values[i - 1];
        double val_curr = (i < normalized_values.size()) ? normalized_values[i] : val_prev;

        int dx = std::max(1, x1 - x0);
        for (int xi = x0; xi <= x1; ++xi) {
            double tseg = static_cast<double>(xi - x0) / dx;
            int yi = static_cast<int>(std::round(y0 + (y1 - y0) * tseg));
            double val_interp = val_prev + (val_curr - val_prev) * tseg;

            // EFFECT 3: Gradient coloring based on value
            color_t line_color = color;
            if (sparkline_gradient_line_) {
                if (val_interp < 0.33) {
                    line_color = interpolate_color(scale_color(color, 0.7f), color, val_interp * 3.0);
                } else if (val_interp > 0.66) {
                    color_t hot = interpolate_color(color, RGB(255, 200, 100), 0.4);
                    line_color = interpolate_color(color, hot, (val_interp - 0.66) * 3.0);
                }
            }

            // EFFECT 6: Dynamic line thickness
            int lw = line_width;
            if (sparkline_dynamic_width_) {
                lw = (val_interp > 0.5) ? std::max(2, line_width + 1) : line_width;
            }

            drawLine(xi, yi, xi, yi, line_color);
            if (lw > 1) {
                drawLine(xi, yi + 1, xi, yi + 1, line_color);
            }
            if (lw > 2) {
                drawLine(xi, yi - 1, xi, yi - 1, line_color);
            }
        }
    }

    // ===== EFFECT 2: Peak Highlights with Bloom =====
    if (sparkline_peak_highlight_) {
        for (size_t pi : peak_indices) {
            if (pi >= points.size()) continue;
            int px = points[pi].first;
            int py = points[pi].second;

            // Draw bloom glow around peak
            color_t glow = interpolate_color(color, RGB(255, 255, 255), 0.5);
            drawFilledCircle(px, py, 4, scale_color(glow, 0.2f));
            drawFilledCircle(px, py, 3, scale_color(glow, 0.4f));
            drawFilledCircle(px, py, 2, glow);
        }
    }

    // ===== EFFECT 4: Particle Trails =====
    if (sparkline_particles_ && data.size() >= 3) {
        // Find rapid changes
        for (size_t i = 2; i < data.size(); ++i) {
            double change = std::abs(normalized_values[i] - normalized_values[i-1]);
            if (change > 0.15) { // Significant change
                int px = points[i].first;
                int py = points[i].second;

                // Draw small trailing particles
                int dir = (normalized_values[i] > normalized_values[i-1]) ? -1 : 1;
                for (int j = 1; j <= 3; ++j) {
                    int trail_y = py + dir * j * 3;
                    float trail_alpha = 0.6f * (1.0f - j * 0.25f);
                    color_t trail_color = scale_color(color, trail_alpha);
                    if (trail_y >= y && trail_y < y + h) {
                        drawLine(px, trail_y, px, trail_y, trail_color);
                    }
                }
            }
        }
    }

    // ===== EFFECT 7: Shimmer Effect on Baseline =====
    if (sparkline_baseline_shimmer_) {
        // Animated dashed line with shimmer
        static double shimmer_phase = 0.0;
        shimmer_phase += 0.15; // Increment for animation
        if (shimmer_phase > 20.0) shimmer_phase = 0.0;

        int dash_len = 4;
        int gap_len = 3;
        for (int xi = x + 1; xi < x + w - 1; ++xi) {
            int phase_offset = static_cast<int>(shimmer_phase);
            int pos = (xi - x + phase_offset) % (dash_len + gap_len);
            if (pos < dash_len) {
                // Shimmer intensity varies along the line
                float shimmer = 0.7f + 0.3f * std::sin((xi - x) * 0.2 + shimmer_phase);
                color_t shimmer_color = scale_color(current_theme_.bar_border, shimmer);
                drawLine(xi, baseline_y_pos, xi, baseline_y_pos, shimmer_color);
            }
        }
    } else {
        // Regular baseline
        drawLine(x + 1, baseline_y_pos, x + w - 2, baseline_y_pos, current_theme_.bar_border);
    }

    // ===== EFFECT 1: Endpoint Pulse Animation with Glow =====
    auto [px, py] = points.back();
    if (sparkline_pulse_) {
        // Animate pulse size using AnimationEngine
        static double pulse_time = 0.0;
        pulse_time += 0.08;
        if (pulse_time > 6.28) pulse_time = 0.0;

        // Pulse frequency varies with activity (higher values pulse faster)
        double activity = normalized_values.back();
        double freq = 1.0 + activity * 1.5;
        double pulse_scale = 1.0 + 0.4 * std::sin(pulse_time * freq);

        int pulse_r = static_cast<int>(2.5 * pulse_scale);
        int glow_r = pulse_r + 2;

        // Outer glow
        drawFilledCircle(px, py, glow_r, scale_color(color, 0.25f));
        // Mid glow
        drawFilledCircle(px, py, glow_r - 1, scale_color(color, 0.5f));
        // Core pulse
        drawFilledCircle(px, py, pulse_r, color);
        // Bright center
        drawFilledCircle(px, py, std::max(1, pulse_r - 1), interpolate_color(color, RGB(255, 255, 255), 0.6));
    } else {
        // Static endpoint
        drawCircle(px, py, 2, color);
    }
}

void Renderer::drawProgressBar(int x, int y, int w, int h, double value, color_t color, color_t bg) {
    int radius = std::max(1, h / 2);
    // Background rounded bar
    drawRect(x + radius, y, std::max(0, w - 2 * radius), h, bg);
    drawCircle(x + radius, y + radius, radius, bg);
    drawCircle(x + w - radius - 1, y + radius, radius, bg);

    int fill_w = static_cast<int>(w * clamp(value, 0.0, 1.0));
    if (fill_w > 0) {
        int fill_right = x + fill_w - 1;
        drawRect(x + radius, y, std::max(0, fill_w - 2 * radius), h, color);
        drawCircle(x + radius, y + radius, radius, color);
        if (fill_right > x + radius) {
            int cap_x = std::min(x + w - radius - 1, fill_right);
            drawCircle(cap_x, y + radius, radius, color);
        }
    }
}

void Renderer::drawCard(const std::string& icon,
                        const std::string& value,
                        double indicator_val,
                        double indicator_max,
                        const std::vector<double>& history,
                        double hist_min,
                        double hist_max,
                        int x, int y, int w, int h,
                        color_t accent_color,
                        color_t icon_color,
                        color_t spark_bg,
                        bool show_progress_bar,
                        MetricType metric_type,
                        AnimationEngine& animator) {
    drawRect(x, y, w, h, current_theme_.bar_bg);

    int icon_size = std::clamp(h / 6, 20, 28);
    int icon_x = x + (w - icon_size) / 2;
    int icon_y = y + 8;
    drawIcon(icon, icon_x, icon_y, icon_size, icon_color);

    float text_size = std::clamp(h / 10.0f, 14.0f, 18.0f);
    int text_w = measureTextWidth(value, text_size);
    int text_x = x + (w - text_w) / 2;
    int text_y = icon_y + icon_size + 6;
    drawText(value, text_x, text_y, current_theme_.text_value, text_size);

    if (!history.empty()) {
        int spark_x = x + 2;
        int spark_w = w - 4;
        int spark_y = text_y + static_cast<int>(text_size) + 6;
        int spark_h = h - (spark_y - y) - (show_progress_bar ? 6 : 3);
        if (spark_h < 12) {
            spark_h = 12;
            spark_y = y + h - (show_progress_bar ? 6 : 3) - spark_h;
        }
        drawSparkline(spark_x, spark_y, spark_w, spark_h, history, hist_min, hist_max,
                      accent_color, spark_bg, 2, metric_type, animator);
    }

    // Progress line only if requested
    if (show_progress_bar) {
        drawProgressBar(x + 2, y + h - 2, w - 4, 4, indicator_val / indicator_max, accent_color, spark_bg);
    }
}

void Renderer::drawStatusBar(const SystemMetrics& metrics, const IdleModeController& idle_controller) {
    int bar_h = std::clamp(DISPLAY_HEIGHT / 12, 26, 34);
    int bar_y = DISPLAY_HEIGHT - bar_h;
    drawRect(0, bar_y, DISPLAY_WIDTH, bar_h, current_theme_.bar_bg);
    drawLine(0, bar_y, DISPLAY_WIDTH, bar_y, current_theme_.bar_border);

    // WAN indicator
    color_t dot_color = current_theme_.state_low;
    std::string wan = metrics.get_wan_status();
    std::string wan_label = "OK";
    if (wan == "DOWN") {
        dot_color = current_theme_.state_high;
        wan_label = "DOWN";
    } else if (wan == "DEGRADED") {
        dot_color = current_theme_.state_medium;
        wan_label = "SLOW";
    } else if (wan == "CHECKING") {
        wan_label = "...";
    }

    int dot_r = (bar_h >= 32 ? 5 : 4);
    float text_size = (bar_h >= 32 ? 12.5f : 11.0f);
    drawCircle(10, bar_y + bar_h / 2, dot_r, dot_color);
    drawText("WAN:" + wan_label, 22, bar_y + (bar_h / 2 - 5), current_theme_.text_status, text_size);

    // Uptime on right
    std::string up = formatUptime(metrics.uptime_seconds);
    int up_w = measureTextWidth(up, text_size);
    drawText(up, DISPLAY_WIDTH - up_w - 6, bar_y + (bar_h / 2 - 5), current_theme_.text_status, text_size);

    // Ticker
    if (!ticker_text_.empty()) {
        int text_w = measureTextWidth(ticker_text_, 11.0f);
        int start_x = 90; // leave space for WAN
        int end_x = DISPLAY_WIDTH - up_w - 10;
        int zone_w = end_x - start_x;
        if (zone_w > 20) {
            float speed = ticker_speed_px_ * (idle_controller.is_idle() ? 0.4f : 1.0f);
            if (ticker_offset_px_ > zone_w + text_w + 20) {
                ticker_offset_px_ = 0;
            }
            ticker_offset_px_ += speed;
            int tx = start_x + zone_w - static_cast<int>(ticker_offset_px_);
            color_t ticker_color = interpolate_color(current_theme_.text_status,
                                                    scale_color(current_theme_.text_status, 0.5f),
                                                    idle_controller.get_transition_progress());
            drawTextClipped(ticker_text_, tx, bar_y + (bar_h / 2 - 5), ticker_color, text_size,
                            start_x, bar_y, zone_w, bar_h);
        }
    }
}

void Renderer::drawPanelFrame(int x, int y, int w, int h, const std::string& title, const std::string& subtitle) {
    color_t panel_bg = scale_color(current_theme_.bar_bg, 0.80f);
    color_t panel_border = current_theme_.bar_border;
    drawRoundedRect(x, y, w, h, 8, panel_bg, panel_border);
    if (!title.empty()) {
        drawText(title, x + 12, y + 6, dimColor(current_theme_.text_value), 14.0f);
    }
    if (!subtitle.empty()) {
        drawText(subtitle, x + 12, y + 22, dimColor(current_theme_.text_status), 11.0f);
    }
    drawLine(x + 10, y + 32, x + w - 11, y + 32, panel_border);
}

void Renderer::drawSeriesLine(const std::deque<double>& data, int x, int y, int w, int h,
                              double min_val, double max_val, color_t color,
                              color_t shadow_color, int width, MetricType metric_type,
                              AnimationEngine& animator, double time_sec) {
    if (data.size() < 2 || !target_buffer_) return;
    const int inner_w = std::max(1, w - 2);
    const int inner_h = std::max(1, h - 2);
    double range = std::max(1e-6, max_val - min_val);
    size_t n = data.size();

    // Get animation key based on metric type
    std::string pulse_key = "series_pulse";
    switch (metric_type) {
        case MetricType::CPU: pulse_key = "series_cpu_pulse"; break;
        case MetricType::TEMP: pulse_key = "series_temp_pulse"; break;
        case MetricType::NET1: pulse_key = "series_net1_pulse"; break;
        case MetricType::NET2: pulse_key = "series_net2_pulse"; break;
        default: break;
    }

    // Build points with normalized values
    std::vector<std::pair<int, int>> points;
    std::vector<double> normalized_values;
    points.reserve(n);
    normalized_values.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        double v = clamp((data[i] - min_val) / range, 0.0, 1.0);
        normalized_values.push_back(v);
        int px = x + 1 + static_cast<int>((static_cast<double>(i) / (n - 1)) * inner_w);
        int py = y + h - 1 - static_cast<int>(v * inner_h);
        points.emplace_back(px, py);
    }

    // Find local peaks for highlighting
    std::vector<size_t> peak_indices;
    if (sparkline_peak_highlight_ && data.size() >= 5) {
        for (size_t i = 2; i < data.size() - 2; ++i) {
            if (data[i] > data[i-1] && data[i] > data[i-2] &&
                data[i] > data[i+1] && data[i] > data[i+2] &&
                normalized_values[i] > 0.6) {
                peak_indices.push_back(i);
            }
        }
    }

    // ===== EFFECT 8: Shadow/Depth =====
    if (sparkline_shadow_) {
        color_t shadow_col = scale_color(color, 0.3f);
        int prev_x = points.front().first;
        int prev_y = points.front().second + 2;
        for (size_t i = 1; i < points.size(); ++i) {
            int px = points[i].first;
            int py = points[i].second + 2;
            drawLine(prev_x, prev_y, px, py, shadow_col);
            prev_x = px;
            prev_y = py;
        }
    }

    // ===== EFFECT 5: Enhanced Fill with additive blending =====
    uint8_t fr, fg, fb;
    rgb565_to_rgb888(color, fr, fg, fb);
    int bottom_y = y + h - 1;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        auto [x0, y0] = points[i];
        auto [x1, y1] = points[i + 1];
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        int dx = std::max(1, x1 - x0);
        for (int xi = x0; xi <= x1; ++xi) {
            double tseg = static_cast<double>(xi - x0) / dx;
            double top_f = y0 + (y1 - y0) * tseg;
            int top = static_cast<int>(std::round(top_f));
            if (top < y) top = y;
            if (top > bottom_y) top = bottom_y;
            double denom = std::max(1.0, static_cast<double>(bottom_y - top));

            for (int py = top; py <= bottom_y; ++py) {
                if (xi < 0 || xi >= DISPLAY_WIDTH || py < 0 || py >= DISPLAY_HEIGHT) continue;
                double norm = (py - top_f) / denom;
                double intensity;

                if (sparkline_enhanced_fill_) {
                    // Two-stage gradient for series
                    if (norm < 0.15) {
                        intensity = FILL_INTENSITY_SERIES * (1.0 - norm * 3.0);
                    } else {
                        intensity = FILL_INTENSITY_SERIES * 0.7 * fast_exp(FILL_DECAY_SERIES * (norm - 0.15));
                    }
                } else {
                    intensity = FILL_INTENSITY_SERIES * fast_exp(FILL_DECAY_SERIES * norm);
                }

                if (intensity < 0.001) continue;
                size_t idx = static_cast<size_t>(py) * DISPLAY_WIDTH + static_cast<size_t>(xi);

                // EFFECT 9: Color zones
                uint8_t fill_r = fr, fill_g = fg, fill_b = fb;
                if (sparkline_color_zones_ && i < normalized_values.size()) {
                    double val = normalized_values[i];
                    if (val < 0.33) {
                        fill_r = static_cast<uint8_t>(fr * 0.9);
                        fill_g = static_cast<uint8_t>(fg * 1.0);
                        fill_b = static_cast<uint8_t>(fb * 1.1);
                    } else if (val > 0.66) {
                        fill_r = static_cast<uint8_t>(std::min(255, static_cast<int>(fr * 1.1)));
                        fill_g = static_cast<uint8_t>(fg * 0.97);
                        fill_b = static_cast<uint8_t>(fb * 0.9);
                    }
                }

                color_t dst = (*target_buffer_)[idx];
                uint8_t dr, dg, db;
                rgb565_to_rgb888(dst, dr, dg, db);
                int nr = std::min(255, static_cast<int>(dr) + static_cast<int>(fill_r * intensity));
                int ng = std::min(255, static_cast<int>(dg) + static_cast<int>(fill_g * intensity));
                int nb = std::min(255, static_cast<int>(db) + static_cast<int>(fill_b * intensity));
                (*target_buffer_)[idx] = rgb888_to_rgb565(static_cast<uint8_t>(nr),
                                                          static_cast<uint8_t>(ng),
                                                          static_cast<uint8_t>(nb));
            }
        }
    }

    // ===== EFFECT 3: Gradient Line + EFFECT 6: Dynamic Line Thickness =====
    int prev_x = points.front().first;
    int prev_y = points.front().second;
    for (size_t i = 1; i < points.size(); ++i) {
        int px = points[i].first;
        int py = points[i].second;

        double val_prev = normalized_values[i - 1];
        double val_curr = normalized_values[i];
        double val_avg = (val_prev + val_curr) * 0.5;

        // EFFECT 3: Gradient coloring
        color_t line_color = color;
        if (sparkline_gradient_line_) {
            if (val_avg < 0.33) {
                line_color = interpolate_color(scale_color(color, 0.75f), color, val_avg * 3.0);
            } else if (val_avg > 0.66) {
                color_t hot = interpolate_color(color, RGB(255, 200, 120), 0.35);
                line_color = interpolate_color(color, hot, (val_avg - 0.66) * 3.0);
            }
        }

        // EFFECT 6: Dynamic thickness
        int lw = width;
        if (sparkline_dynamic_width_ && val_avg > 0.5) {
            lw = width + 1;
        }

        if (shadow_color != color && !sparkline_shadow_) {
            drawLine(prev_x, prev_y + 1, px, py + 1, shadow_color);
        }
        drawLine(prev_x, prev_y, px, py, line_color);
        if (lw > 1) {
            drawLine(prev_x, prev_y + 1, px, py + 1, line_color);
        }
        if (lw > 2) {
            drawLine(prev_x, prev_y - 1, px, py - 1, line_color);
        }

        prev_x = px;
        prev_y = py;
    }

    // ===== EFFECT 2: Peak Highlights with Bloom =====
    if (sparkline_peak_highlight_) {
        for (size_t pi : peak_indices) {
            if (pi >= points.size()) continue;
            int px = points[pi].first;
            int py = points[pi].second;

            color_t glow = interpolate_color(color, RGB(255, 255, 255), 0.4);
            drawFilledCircle(px, py, 5, scale_color(glow, 0.15f));
            drawFilledCircle(px, py, 4, scale_color(glow, 0.3f));
            drawFilledCircle(px, py, 3, scale_color(glow, 0.5f));
            drawFilledCircle(px, py, 2, glow);
        }
    }

    // ===== EFFECT 4: Particle Trails =====
    if (sparkline_particles_ && data.size() >= 3) {
        for (size_t i = 2; i < data.size(); ++i) {
            double change = std::abs(normalized_values[i] - normalized_values[i-1]);
            if (change > 0.12) {
                int px = points[i].first;
                int py = points[i].second;
                int dir = (normalized_values[i] > normalized_values[i-1]) ? -1 : 1;
                for (int j = 1; j <= 4; ++j) {
                    int trail_y = py + dir * j * 4;
                    float trail_alpha = 0.5f * (1.0f - j * 0.2f);
                    color_t trail_color = scale_color(color, trail_alpha);
                    if (trail_y >= y && trail_y < y + h) {
                        drawLine(px - 1, trail_y, px + 1, trail_y, trail_color);
                    }
                }
            }
        }
    }

    // ===== EFFECT 1: Endpoint Pulse Animation with Glow =====
    if (sparkline_pulse_) {
        double activity = normalized_values.back();
        double freq = 1.0 + activity * 1.2;
        double pulse_scale = 1.0 + 0.35 * std::sin(time_sec * 3.14159 * freq);

        int pulse_r = static_cast<int>(3.0 * pulse_scale);
        int glow_r = pulse_r + 3;

        drawFilledCircle(prev_x, prev_y, glow_r, scale_color(color, 0.2f));
        drawFilledCircle(prev_x, prev_y, glow_r - 1, scale_color(color, 0.4f));
        drawFilledCircle(prev_x, prev_y, pulse_r, color);
        drawFilledCircle(prev_x, prev_y, std::max(1, pulse_r - 1), interpolate_color(color, RGB(255, 255, 255), 0.5));
    } else {
        drawFilledCircle(prev_x, prev_y, 2, color);
    }
}

void Renderer::drawRingGauge(int cx, int cy, int r, int thickness, double frac,
                             color_t active, color_t inactive, int segments) {
    int segs = std::max(12, segments);
    constexpr double kPi = 3.141592653589793;
    double f = clamp(frac, 0.0, 1.0);
    int lit = static_cast<int>(std::round(f * segs));
    int inner = std::max(1, r - thickness);
    for (int i = 0; i < segs; ++i) {
        double a = (2.0 * kPi * i / segs) - kPi / 2.0;
        int x0 = cx + static_cast<int>(fast_cos(a) * inner);
        int y0 = cy + static_cast<int>(fast_sin(a) * inner);
        int x1 = cx + static_cast<int>(fast_cos(a) * r);
        int y1 = cy + static_cast<int>(fast_sin(a) * r);
        color_t col = (i < lit) ? active : inactive;
        drawLine(x0, y0, x1, y1, col);
        if (thickness > 6) {
            int x2 = cx + static_cast<int>(fast_cos(a) * (inner + 2));
            int y2 = cy + static_cast<int>(fast_sin(a) * (inner + 2));
            drawLine(x2, y2, x1, y1, col);
        }
    }
}

void Renderer::drawArcPolyline(int cx, int cy, int r, double a0, double a1, color_t color) {
    drawArcPolyline(cx, cy, r, a0, a1, color, false);
}

void Renderer::drawArcPolyline(int cx, int cy, int r, double a0, double a1, color_t color, bool invert_y) {
    double span = std::abs(a1 - a0);
    int steps = std::max(24, static_cast<int>(span * r * 1.2));
    steps = std::min(180, steps);
    double step = (a1 - a0) / static_cast<double>(steps);
    int prev_x = cx + static_cast<int>(fast_cos(a0) * r);
    int prev_y = cy + static_cast<int>((invert_y ? -fast_sin(a0) : fast_sin(a0)) * r);
    for (int i = 1; i <= steps; ++i) {
        double a = a0 + step * i;
        int x = cx + static_cast<int>(fast_cos(a) * r);
        int y = cy + static_cast<int>((invert_y ? -fast_sin(a) : fast_sin(a)) * r);
        drawLine(prev_x, prev_y, x, y, color);
        prev_x = x;
        prev_y = y;
    }
}

void Renderer::drawThickArc(int cx, int cy, int r, int thickness, double a0, double a1, color_t color) {
    drawThickArc(cx, cy, r, thickness, a0, a1, color, false);
}

void Renderer::drawThickArc(int cx, int cy, int r, int thickness, double a0, double a1, color_t color, bool invert_y) {
    int t = std::max(1, thickness);
    for (int i = 0; i < t; ++i) {
        int rr = r - i;
        if (rr <= 0) break;
        drawArcPolyline(cx, cy, rr, a0, a1, color, invert_y);
    }
}

void Renderer::drawSmoothRingGauge(int cx, int cy, int r, int thickness, double frac,
                                   color_t active, color_t inactive) {
    constexpr double kPi = 3.141592653589793;
    double f = clamp(frac, 0.0, 1.0);
    double start = -kPi / 2.0;
    double end = start + (2.0 * kPi * f);

    // Track (inactive full circle)
    drawThickArc(cx, cy, r, thickness, start, start + 2.0 * kPi, inactive);

    // Active arc
    if (f > 0.0) {
        drawThickArc(cx, cy, r, thickness, start, end, active);

        int cap_r = std::max(2, thickness / 2);
        int cap_rad = r - thickness / 2;
        int x0 = cx + static_cast<int>(fast_cos(start) * cap_rad);
        int y0 = cy + static_cast<int>(fast_sin(start) * cap_rad);
        int x1 = cx + static_cast<int>(fast_cos(end) * cap_rad);
        int y1 = cy + static_cast<int>(fast_sin(end) * cap_rad);
        drawFilledCircle(x0, y0, cap_r, active);
        drawFilledCircle(x1, y1, cap_r, active);
    }

    // Inner fill to make donut smoother
    color_t inner = scale_color(current_theme_.bar_bg, 0.70f);
    int inner_r = r - thickness - 1;
    if (inner_r > 0) {
        drawFilledCircle(cx, cy, inner_r, inner);
    }
}

void Renderer::drawSemiGauge(int cx, int cy, int r, int thickness, double frac,
                             color_t active, color_t track) {
    constexpr double kPi = 3.141592653589793;
    double f = clamp(frac, 0.0, 1.0);
    double start = kPi;   // left
    double end = 0.0;     // right
    double sweep = start - end; // PI
    double prog_end = start - sweep * f;

    // Track (full top semicircle)
    drawThickArc(cx, cy, r, thickness, start, end, track, true);

    // Active arc
    if (f > 0.0) {
        drawThickArc(cx, cy, r, thickness, start, prog_end, active, true);
        int cap_r = std::max(2, thickness / 2);
        int cap_rad = r - thickness / 2;
        int x0 = cx + static_cast<int>(fast_cos(start) * cap_rad);
        int y0 = cy + static_cast<int>(-fast_sin(start) * cap_rad);
        int x1 = cx + static_cast<int>(fast_cos(prog_end) * cap_rad);
        int y1 = cy + static_cast<int>(-fast_sin(prog_end) * cap_rad);
        drawFilledCircle(x0, y0, cap_r, active);
        drawFilledCircle(x1, y1, cap_r, active);
    }
}

void Renderer::drawGraphPanel(int x, int y, int w, int h,
                              const std::string& title,
                              const std::string& values,
                              const std::string& subtitle,
                              const std::string& label_a,
                              const std::string& label_b,
                              const std::deque<double>& series_a,
                              const std::deque<double>& series_b,
                              double min_val, double max_val,
                              color_t color_a, color_t color_b,
                              MetricType metric_type_a, MetricType metric_type_b,
                              AnimationEngine& animator, double time_sec) {
    drawPanelFrame(x, y, w, h, title, subtitle);
    if (!values.empty()) {
        int vw = measureTextWidth(values, 11.0f);
        drawText(values, x + w - vw - 12, y + 6, dimColor(current_theme_.text_status), 11.0f);
    }
    int legend_y = y + 36;
    int lx = x + 12;
    drawRect(lx, legend_y, 6, 6, color_a);
    drawText(label_a, lx + 10, legend_y - 2, dimColor(current_theme_.text_status), 11.0f);
    int lw = measureTextWidth(label_a, 11.0f);
    int lx2 = lx + 10 + lw + 14;
    drawRect(lx2, legend_y, 6, 6, color_b);
    drawText(label_b, lx2 + 10, legend_y - 2, dimColor(current_theme_.text_status), 11.0f);

    int gx = x + 10;
    int gy = y + 48;
    int gw = w - 20;
    int gh = h - (gy - y) - 10;
    color_t grid_minor = scale_color(current_theme_.bar_border, 0.25f);
    color_t grid_major = scale_color(current_theme_.bar_border, 0.45f);
    drawRect(gx, gy, gw, gh, scale_color(current_theme_.spark_bg, 0.9f));
    int cols = 12;
    int rows = 6;
    for (int c = 1; c < cols; ++c) {
        int px = gx + (c * gw) / cols;
        color_t col = (c % 3 == 0) ? grid_major : grid_minor;
        drawLine(px, gy, px, gy + gh - 1, col);
    }
    for (int r = 1; r < rows; ++r) {
        int py = gy + (r * gh) / rows;
        color_t col = (r % 2 == 0) ? grid_major : grid_minor;
        drawLine(gx, py, gx + gw - 1, py, col);
    }
    color_t shadow_a = scale_color(color_a, 0.5f);
    color_t shadow_b = scale_color(color_b, 0.5f);
    drawSeriesLine(series_a, gx, gy, gw, gh, min_val, max_val, color_a, shadow_a, 2, metric_type_a, animator, time_sec);
    drawSeriesLine(series_b, gx, gy, gw, gh, min_val, max_val, color_b, shadow_b, 2, metric_type_b, animator, time_sec);
}

void Renderer::drawVitalsPanel(int x, int y, int w, int h,
                               double cpu, double temp, double mem,
                               double net1, const std::string& wan_status,
                               color_t cpu_color, color_t temp_color, color_t mem_color, color_t net_color) {
    (void)wan_status;
    drawPanelFrame(x, y, w, h, "Vitals", "");
    int inner_y = y + 34;
    int inner_w = w - 16;
    int inner_h = h - (inner_y - y) - 8;
    int block_h = inner_h / 3;

    auto clamp_i = [](int v, int lo, int hi) {
        return std::min(hi, std::max(lo, v));
    };

    bool use_ram = mem > 0.0;
    double mid_val = use_ram ? mem : net1;
    color_t mid_color = use_ram ? mem_color : net_color;
    std::string mid_label = use_ram ? "RAM" : "NET1";
    std::string mid_text = use_ram ? (std::to_string(static_cast<int>(mem)) + "%") : formatNet(net1);

    auto drawGauge = [&](int idx, double value, double max, color_t color, const std::string& label, const std::string& val) {
        const int lift = 10; // move gauges up
        int block_y = inner_y + idx * block_h;
        int cx = x + w / 2;
        int cy = block_y + block_h - 10 - lift;
        int ring_r = clamp_i(std::min(inner_w / 2 - 6, block_h - 18), 20, 30);
        int thickness = clamp_i(ring_r / 3, 10, 12);

        color_t active = dimColor(color);
        color_t track = scale_color(active, 0.20f);
        drawSemiGauge(cx, cy, ring_r, thickness, clamp(value / max, 0.0, 1.0), active, track);

        int vw = measureTextWidth(val, 14.0f);
        int val_y = cy - static_cast<int>(ring_r * 0.45);
        drawText(val, cx - vw / 2, val_y, dimColor(current_theme_.text_value), 14.0f);
        int lw = measureTextWidth(label, 11.0f);
        int label_y = block_y + block_h - 2 - lift;
        drawText(label, cx - lw / 2, label_y, dimColor(current_theme_.text_status), 11.0f);
    };

    drawGauge(0, cpu, 100.0, cpu_color, "CPU", std::to_string(static_cast<int>(cpu)) + "%");
    drawGauge(1, mid_val, use_ram ? 100.0 : 2500.0, mid_color, mid_label, mid_text);
    drawGauge(2, temp, 100.0, temp_color, "TEMP", std::to_string(static_cast<int>(temp)) + "C");
}

void Renderer::drawPrintScreen(const PrinterMetrics& printer,
                               AnimationEngine& animator,
                               double time_sec) {
    (void)animator;
    (void)time_sec;
    int left_x = 10, left_y = 10, left_w = 310, left_h = 300;
    int right_x = 330, right_y = 10, right_w = 140, right_h = 300;

    drawPanelFrame(left_x, left_y, left_w, left_h, "Preview", "");
    drawPanelFrame(right_x, right_y, right_w, right_h, "Print", "");
    // Remove divider line under the header for the Print panel
    color_t panel_bg = scale_color(current_theme_.bar_bg, 0.80f);
    drawRect(right_x + 10, right_y + 32, right_w - 21, 1, panel_bg);

    int img_pad = 12;
    int img_x = left_x + img_pad;
    int img_y = left_y + 36;
    int img_w = left_w - img_pad * 2;
    int img_h = left_h - (img_y - left_y) - img_pad;
    drawImageRGBAFit(img_x, img_y, img_w, img_h, printer);

    double pct = printer.progress01 * 100.0f;
    pct = std::max(0.0, std::min(100.0, pct));
    std::ostringstream pct_ss;
    pct_ss.setf(std::ios::fixed);
    pct_ss << std::setprecision(3) << pct;
    std::string pct_text = pct_ss.str();
    pct_text += "%";
    float pct_size = 28.0f;
    int pct_w = measureTextWidth(pct_text, pct_size);
    drawText(pct_text, right_x + (right_w - pct_w) / 2, right_y + 36,
             dimColor(current_theme_.text_value), pct_size);

    std::string state = printer.state;
    for (char& c : state) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (state.empty()) state = "IDLE";

    color_t vivid_ok = RGB(0, 255, 80);
    color_t vivid_warn = RGB(255, 230, 0);
    color_t status_color = vivid_ok;
    if (printer.state == "paused") status_color = vivid_warn;
    if (printer.state == "error") status_color = current_theme_.state_high;
    if (printer.state == "complete" || printer.state == "standby") status_color = vivid_ok;
    status_color = dimColor(status_color);
    color_t track = scale_color(status_color, 0.20f);

    int gauge_top = right_y + 52;
    int gauge_h = 120;
    int cx = right_x + right_w / 2;
    int cy = gauge_top + gauge_h - 10;
    int ring_r = std::min(right_w / 2 - 8, gauge_h - 20);
    ring_r = std::max(24, ring_r);
    int thickness = std::max(10, std::min(12, ring_r / 3));
    drawSemiGauge(cx, cy, ring_r, thickness, clamp(printer.progress01, 0.0f, 1.0f),
                  status_color, track);

    float detail_fs = 11.0f;
    int detail_y = gauge_top + gauge_h + 6;
    drawText(state, right_x + 10, detail_y, status_color, detail_fs);
    detail_y += 14;

    std::string eta = (printer.eta_sec > 0) ? ("ETA " + formatDurationShort(printer.eta_sec)) : "ETA --";
    std::string el = "E " + formatDurationShort(printer.elapsed_sec);
    drawText(eta, right_x + 10, detail_y, dimColor(current_theme_.text_status), detail_fs);
    int el_w = measureTextWidth(el, detail_fs);
    drawText(el, right_x + right_w - el_w - 10, detail_y, dimColor(current_theme_.text_status), detail_fs);
    detail_y += 14;

    std::string fname = printer.filename.empty() ? "-" : printer.filename;
    fname = trimTextToWidth(fname, detail_fs, right_w - 20);
    drawText(fname, right_x + 10, detail_y, dimColor(current_theme_.text_value), detail_fs);
}

void Renderer::drawImageRGBAFit(int x, int y, int w, int h,
                                const PrinterMetrics& printer) {
    if (!target_buffer_) return;
    auto img = printer.thumb_rgba;
    color_t bg = scale_color(current_theme_.spark_bg, 0.85f);
    drawRect(x, y, w, h, bg);
    if (!img || img->data.empty() || img->w <= 0 || img->h <= 0) {
        drawText("NO PREVIEW", x + 8, y + h / 2 - 6, dimColor(current_theme_.text_status), 12.0f);
        return;
    }
    int iw = img->w;
    int ih = img->h;
    double scale = std::min(static_cast<double>(w) / iw, static_cast<double>(h) / ih);
    int dw = std::max(1, static_cast<int>(std::round(iw * scale)));
    int dh = std::max(1, static_cast<int>(std::round(ih * scale)));
    int dx = x + (w - dw) / 2;
    int dy = y + (h - dh) / 2;

    for (int yy = 0; yy < dh; ++yy) {
        int sy = (yy * ih) / dh;
        for (int xx = 0; xx < dw; ++xx) {
            int sx = (xx * iw) / dw;
            int src_idx = (sy * iw + sx) * 4;
            uint8_t sr = img->data[src_idx + 0];
            uint8_t sg = img->data[src_idx + 1];
            uint8_t sb = img->data[src_idx + 2];
            uint8_t sa = img->data[src_idx + 3];
            if (sa == 0) continue;
            int px = dx + xx;
            int py = dy + yy;
            if (px < 0 || py < 0 || px >= DISPLAY_WIDTH || py >= DISPLAY_HEIGHT) continue;
            size_t di = static_cast<size_t>(py) * DISPLAY_WIDTH + px;
            if (sa == 255) {
                (*target_buffer_)[di] = rgb888_to_rgb565(sr, sg, sb);
            } else {
                uint8_t dr, dg, db;
                rgb565_to_rgb888((*target_buffer_)[di], dr, dg, db);
                uint8_t rr = static_cast<uint8_t>((sr * sa + dr * (255 - sa)) / 255);
                uint8_t rg = static_cast<uint8_t>((sg * sa + dg * (255 - sa)) / 255);
                uint8_t rb = static_cast<uint8_t>((sb * sa + db * (255 - sa)) / 255);
                (*target_buffer_)[di] = rgb888_to_rgb565(rr, rg, rb);
            }
        }
    }
}

std::string Renderer::trimTextToWidth(const std::string& s, float size, int max_w) {
    if (max_w <= 0) return "";
    if (measureTextWidth(s, size) <= max_w) return s;
    const std::string ell = "...";
    if (measureTextWidth(ell, size) >= max_w) return ell;
    std::string out = s;
    while (!out.empty() && measureTextWidth(out + ell, size) > max_w) {
        out.pop_back();
    }
    return out + ell;
}

std::string Renderer::formatDurationShort(int seconds) const {
    if (seconds < 0) return "--";
    int s = seconds;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    if (h > 0) {
        return std::to_string(h) + "h " + std::to_string(m) + "m";
    }
    if (m > 0) {
        return std::to_string(m) + "m " + std::to_string(sec) + "s";
    }
    return std::to_string(sec) + "s";
}

void Renderer::drawServicesPanel(int x, int y, int w, int h,
                                 const SystemMetrics& metrics) {
    drawPanelFrame(x, y, w, h, "Services", "");
    int rows = 4;
    int row_gap = 6;
    int available = h - 36;
    int row_h = (available - (rows - 1) * row_gap) / rows;
    if (row_h < 12) row_h = 12;
    int start_y = y + 36;
    auto statusColor = [&](const std::string& label, double value, const std::string& text) -> color_t {
        if (label == "WAN") {
            if (text == "OK") return current_theme_.state_low;
            if (text == "SLOW" || text == "DEGRADED") return current_theme_.state_medium;
            if (text == "DOWN") return current_theme_.state_high;
            return dimColor(current_theme_.text_status);
        }
        if (label == "Disk" && value >= 0) return pickStateColor(value, "ram");
        if (label == "WG" && value >= 0) return (value > 0 ? current_theme_.state_low : current_theme_.state_medium);
        if (label == "Docker" && value >= 0) return (value > 0 ? current_theme_.state_low : current_theme_.state_high);
        return dimColor(current_theme_.text_status);
    };
    auto drawRow = [&](int row, const std::string& label, const std::string& value, double value_num) {
        int ry = start_y + row * (row_h + row_gap);
        if (ry > y + h - 6) return;
        color_t panel_fill = scale_color(current_theme_.bar_bg, 0.80f);
        color_t row_fill = scale_color(panel_fill, 1.08f);
        color_t row_border = scale_color(current_theme_.bar_border, 0.35f);
        drawRoundedRect(x + 8, ry, w - 16, row_h - 2, 4, row_fill, row_border);
        color_t dot = statusColor(label, value_num, value);
        drawFilledCircle(x + 14, ry + row_h / 2 - 1, 3, dot);
        float fs = (row_h <= 16 ? 10.0f : 11.5f);
        drawText(label, x + 24, ry + 4, dimColor(current_theme_.text_value), fs);
        int vw = measureTextWidth(value, fs);
        drawText(value, x + w - vw - 12, ry + 4, dimColor(current_theme_.text_value), fs);
    };
    std::string docker = (metrics.docker_running >= 0) ? (std::to_string(metrics.docker_running)) : "-";
    std::string disk = (metrics.disk_percent >= 0) ? (std::to_string(metrics.disk_percent) + "%") : "-";
    std::string wg = (metrics.wg_active_peers >= 0) ? (std::to_string(metrics.wg_active_peers)) : "-";
    std::string wan = metrics.get_wan_status();

    drawRow(0, "Docker", docker, metrics.docker_running);
    drawRow(1, "Disk", disk, metrics.disk_percent);
    drawRow(2, "WG", wg, metrics.wg_active_peers);
    drawRow(3, "WAN", wan, -1);
}

void Renderer::drawHeader(int x, int y, int w, int h, const SystemMetrics& metrics) {
    drawRect(x, y, w, h, scale_color(current_theme_.bar_bg, 0.75f));
    drawLine(x, y + h - 1, x + w - 1, y + h - 1, current_theme_.bar_border);
    static std::string title;
    if (title.empty()) {
        std::string env_title = getenv_string("LCD_TITLE", "");
        if (!env_title.empty()) {
            title = env_title;
        } else {
            char host[128] = {0};
            if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
                title = host;
            } else {
                title = "NAS Dashboard";
            }
        }
    }
    // Title intentionally hidden per user request

    std::string wan = metrics.get_wan_status();
    std::string wg = (metrics.wg_active_peers >= 0) ? std::to_string(metrics.wg_active_peers) : "-";
    std::string mc = "-";
    if (metrics.mc_online >= 0 && metrics.mc_max >= 0) {
        mc = std::to_string(metrics.mc_online) + "/" + std::to_string(metrics.mc_max);
    } else if (metrics.mc_online >= 0) {
        mc = std::to_string(metrics.mc_online);
    }
    std::string uptime = formatUptime(metrics.uptime_seconds);
    float right_fs = 22.0f;
    int ry = y + (h - static_cast<int>(right_fs)) / 2;

    color_t label_col = dimColor(current_theme_.text_status);
    color_t neutral_val = dimColor(current_theme_.text_value);
    // Brighter status colors for header values
    color_t ok_col = RGB(0, 255, 120);
    color_t warn_col = RGB(255, 230, 0);
    color_t bad_col = RGB(255, 60, 60);

    auto wan_color = [&]() -> color_t {
        if (wan == "OK") return ok_col;
        if (wan == "DEGRADED") return warn_col;
        if (wan == "DOWN") return bad_col;
        return bad_col;
    }();
    auto wg_color = [&]() -> color_t {
        if (metrics.wg_active_peers > 0) return ok_col;
        if (metrics.wg_active_peers == 0) return bad_col;
        return neutral_val;
    }();
    auto mc_color = [&]() -> color_t {
        if (metrics.mc_online > 0) return ok_col;
        if (metrics.mc_online == 0) return bad_col;
        return neutral_val;
    }();

    struct Seg { std::string t; color_t c; };
    std::vector<Seg> segs;
    segs.reserve(16);
    segs.push_back({"WAN:", label_col});
    segs.push_back({" " + wan, wan_color});
    segs.push_back({"  ", label_col});
    segs.push_back({"WG:", label_col});
    segs.push_back({" " + wg, wg_color});
    segs.push_back({"  ", label_col});
    segs.push_back({"MC:", label_col});
    if (metrics.mc_online >= 0 && metrics.mc_max >= 0) {
        segs.push_back({" " + std::to_string(metrics.mc_online), mc_color});
        segs.push_back({"/" + std::to_string(metrics.mc_max), neutral_val});
    } else {
        segs.push_back({" " + mc, mc_color});
    }
    segs.push_back({"  ", label_col});
    segs.push_back({"Uptime:", label_col});
    segs.push_back({" " + uptime, neutral_val});

    int total_w = 0;
    for (const auto& s : segs) {
        total_w += measureTextWidth(s.t, right_fs);
    }
    int rx = x + (w - total_w) / 2;
    int cx = rx;
    for (const auto& s : segs) {
        drawText(s.t, cx, ry, dimColor(s.c), right_fs);
        cx += measureTextWidth(s.t, right_fs);
    }
}

void Renderer::drawFooter(int x, int y, int w, int h,
                          const SystemMetrics& metrics, const IdleModeController& idle_controller) {
    drawRect(x, y, w, h, scale_color(current_theme_.bar_bg, 0.75f));
    drawLine(x, y, x + w - 1, y, current_theme_.bar_border);
    float footer_fs = 18.0f;
    if (!ticker_text_.empty()) {
        int start_x = x + 14;
        int end_x = x + w - 14;
        int zone_w = end_x - start_x;
        if (zone_w > 20) {
            int text_w = measureTextWidth(ticker_text_, footer_fs);
            float speed = ticker_speed_px_ * (idle_controller.is_idle() ? 0.4f : 1.0f);
            if (ticker_offset_px_ > zone_w + text_w + 20) {
                ticker_offset_px_ = 0;
            }
            ticker_offset_px_ += speed;
            int tx = start_x + zone_w - static_cast<int>(ticker_offset_px_);
            int ty = y + (h - static_cast<int>(footer_fs)) / 2;
            drawTextClipped(ticker_text_, tx, ty, dimColor(current_theme_.text_status), footer_fs,
                            start_x, y, zone_w, h);
        }
    }
}
