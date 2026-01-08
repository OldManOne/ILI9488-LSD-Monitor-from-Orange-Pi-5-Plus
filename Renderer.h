#ifndef RENDERER_H
#define RENDERER_H

#include "Theme.h"
#include "SystemMetrics.h"
#include "AnimationEngine.h"
#include "IdleModeController.h"
#include <vector>
#include <string>
#include <cstdint>
#include <deque>

enum class MetricType {
    CPU,
    TEMP,
    NET1,
    NET2
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    void Render(const SystemMetrics& metrics,
                AnimationEngine& animator,
                const IdleModeController& idle_controller,
                double time_sec,
                std::vector<uint16_t>& buffer);

    void UpdateHistories(const SystemMetrics& metrics);
    void UpdateTickerText(const SystemMetrics& metrics);

private:
    void loadFont(const std::string& font_path, float size);
    
    void drawText(const std::string& text, int x, int y, color_t color, float size);
    void drawTextClipped(const std::string& text, int x, int y, color_t color, float size,
                         int clip_x, int clip_y, int clip_w, int clip_h);
    int measureTextWidth(const std::string& text, float size);
    void drawRect(int x, int y, int w, int h, color_t color);
    void drawLine(int x0, int y0, int x1, int y1, color_t color);
    void drawCircle(int cx, int cy, int r, color_t color);
    void drawFilledCircle(int cx, int cy, int r, color_t color);
    void drawRoundedRect(int x, int y, int w, int h, int r, color_t fill, color_t border);
    void drawVGradient(int x, int y, int w, int h, color_t c1, color_t c2);
    void drawGrid(int x, int y, int w, int h, int cell, int offset_x, int offset_y, color_t color);
    
    void drawIcon(const std::string& name, int x, int y, int size, color_t color);
    void drawSparkline(int x, int y, int w, int h, const std::vector<double>& data,
                       double min_val, double max_val, color_t color, color_t bg_color, int line_width,
                       MetricType metric_type, AnimationEngine& animator);
    void drawProgressBar(int x, int y, int w, int h, double value, color_t color, color_t bg);
    void drawCard(const std::string& icon,
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
                  AnimationEngine& animator);
    void drawStatusBar(const SystemMetrics& metrics, const IdleModeController& idle_controller);

    void drawPanelFrame(int x, int y, int w, int h, const std::string& title, const std::string& subtitle);
    void drawSeriesLine(const std::deque<double>& data, int x, int y, int w, int h,
                        double min_val, double max_val, color_t color,
                        color_t shadow_color, int width);
    void drawRingGauge(int cx, int cy, int r, int thickness, double frac,
                       color_t active, color_t inactive, int segments);
    void drawGraphPanel(int x, int y, int w, int h,
                        const std::string& title,
                        const std::string& values,
                        const std::string& subtitle,
                        const std::string& label_a,
                        const std::string& label_b,
                        const std::deque<double>& series_a,
                        const std::deque<double>& series_b,
                        double min_val, double max_val,
                        color_t color_a, color_t color_b);
    void drawVitalsPanel(int x, int y, int w, int h,
                         double cpu, double temp, double mem,
                         double net1, const std::string& wan_status,
                         color_t cpu_color, color_t temp_color, color_t mem_color, color_t net_color);
    void drawServicesPanel(int x, int y, int w, int h,
                           const SystemMetrics& metrics);
    void drawHeader(int x, int y, int w, int h, const SystemMetrics& metrics);
    void drawFooter(int x, int y, int w, int h, const SystemMetrics& metrics, const IdleModeController& idle_controller);

    color_t pickStateColor(double value, const std::string& key) const;
    std::string formatNet(double mbps) const;
    std::string formatUptime(int seconds) const;
    double computeNetScale(const std::deque<double>& history, double& smooth_max);
    color_t dimColor(color_t c) const;

    std::vector<uint16_t>* target_buffer_ = nullptr;
    Theme current_theme_;
    std::string theme_name_;
    bool grid_enabled_;
    bool band_enabled_;
    int grid_offset_x_ = 0;
    int grid_offset_y_ = 0;

    // Font rendering
    std::vector<uint8_t> font_buffer_;
    void* font_info_ = nullptr; // stbtt_fontinfo

    // Histories for sparklines
    std::deque<double> history_cpu_;
    std::deque<double> history_temp_;
    std::deque<double> history_net1_;
    std::deque<double> history_net2_;
    size_t history_size_ = 51;

    // Ticker
    std::string ticker_text_;
    float ticker_offset_px_ = 0.0f;
    float ticker_speed_px_ = 1.0f;

    // Net autoscale
    bool net_autoscale_ = false;
    double net_autoscale_pctl_ = 95.0;
    double net_autoscale_min_ = 5.0;
    double net_autoscale_max_ = 2500.0;
    double net_autoscale_ema_ = 0.15;
    double net1_scale_max_ = 0.0;
    double net2_scale_max_ = 0.0;
    mutable std::vector<double> scratch_values_;
    float idle_t_ = 0.0f;
};

#endif // RENDERER_H
