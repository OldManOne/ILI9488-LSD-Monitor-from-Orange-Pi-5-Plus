#include "ILI9488.h"
#include "SystemMetrics.h"
#include "Renderer.h"
#include "AnimationEngine.h"
#include "IdleModeController.h"
#include <iostream>
#include <vector>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <vector>

static volatile sig_atomic_t running = 1;
static void signal_handler(int) { running = 0; }

static int getenv_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static double getenv_double(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

struct Rect {
    int x;
    int y;
    int w;
    int h;
};

static void compute_dirty_tiles(const uint16_t* cur,
                                const uint16_t* prev,
                                int width,
                                int height,
                                int tile,
                                std::vector<uint8_t>& dirty,
                                int& dirty_tiles) {
    int tiles_x = (width + tile - 1) / tile;
    int tiles_y = (height + tile - 1) / tile;
    dirty.assign(static_cast<size_t>(tiles_x * tiles_y), 0);
    dirty_tiles = 0;
    for (int ty = 0; ty < tiles_y; ++ty) {
        int y0 = ty * tile;
        int y1 = std::min(y0 + tile, height);
        for (int tx = 0; tx < tiles_x; ++tx) {
            int x0 = tx * tile;
            int x1 = std::min(x0 + tile, width);
            bool diff = false;
            for (int y = y0; y < y1 && !diff; ++y) {
                const uint16_t* a = cur + y * width + x0;
                const uint16_t* b = prev + y * width + x0;
                size_t bytes = static_cast<size_t>(x1 - x0) * sizeof(uint16_t);
                if (std::memcmp(a, b, bytes) != 0) {
                    diff = true;
                }
            }
            if (diff) {
                dirty[ty * tiles_x + tx] = 1;
                ++dirty_tiles;
            }
        }
    }
}

static void build_rects_from_tiles(int width,
                                   int height,
                                   int tile,
                                   const std::vector<uint8_t>& dirty,
                                   std::vector<Rect>& rects) {
    int tiles_x = (width + tile - 1) / tile;
    int tiles_y = (height + tile - 1) / tile;
    std::vector<uint8_t> visited(dirty.size(), 0);
    rects.clear();

    std::vector<int> stack;
    stack.reserve(dirty.size());

    auto push = [&](int idx) {
        visited[idx] = 1;
        stack.push_back(idx);
    };

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            int idx = ty * tiles_x + tx;
            if (!dirty[idx] || visited[idx]) continue;

            int min_tx = tx, max_tx = tx;
            int min_ty = ty, max_ty = ty;
            push(idx);

            while (!stack.empty()) {
                int cur_idx = stack.back();
                stack.pop_back();
                int cx = cur_idx % tiles_x;
                int cy = cur_idx / tiles_x;
                min_tx = std::min(min_tx, cx);
                max_tx = std::max(max_tx, cx);
                min_ty = std::min(min_ty, cy);
                max_ty = std::max(max_ty, cy);

                const int nx[4] = {1, -1, 0, 0};
                const int ny[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int xx = cx + nx[k];
                    int yy = cy + ny[k];
                    if (xx < 0 || yy < 0 || xx >= tiles_x || yy >= tiles_y) continue;
                    int nidx = yy * tiles_x + xx;
                    if (dirty[nidx] && !visited[nidx]) {
                        push(nidx);
                    }
                }
            }

            Rect r;
            r.x = min_tx * tile;
            r.y = min_ty * tile;
            r.w = (max_tx - min_tx + 1) * tile;
            r.h = (max_ty - min_ty + 1) * tile;
            if (r.x + r.w > width) r.w = width - r.x;
            if (r.y + r.h > height) r.h = height - r.y;
            rects.push_back(r);
        }
    }
}

const int TARGET_FPS = getenv_int("LCD_FPS", 5);
const int IDLE_FPS = getenv_int("LCD_IDLE_FPS", 3);
const int BURST_FRAMES = getenv_int("LCD_ANIM_BURST_FRAMES", 5);
const int TILE_SIZE = getenv_int("LCD_DIRTY_TILE", 16);
const int DIRTY_MAX_RECTS = getenv_int("LCD_DIRTY_MAX_RECTS", 8);
const double FULL_FRAME_THRESHOLD = getenv_double("LCD_FULL_FRAME_THRESHOLD", 0.6);

int main() {
    std::cout << "Starting Full LCD Monitor Test..." << std::endl << std::flush;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // GPIO mapping from Python script
    const std::string dc_chip = "/dev/gpiochip3";
    const int dc_pin = 13;
    const std::string rst_chip = "/dev/gpiochip3";
    const int rst_pin = 14;
    const std::string bl_chip = "/dev/gpiochip1";
    const int bl_pin = 2;
    const std::string spi_dev = "/dev/spidev0.0";

    ILI9488 display(spi_dev, dc_chip, dc_pin, rst_chip, rst_pin, bl_chip, bl_pin);
    if (!display.Init()) {
        std::cerr << "Failed to initialize display" << std::endl << std::flush;
        return 1;
    }

    SystemMetrics metrics;
    metrics.Start(); // Start the async worker threads

    Renderer renderer;
    AnimationEngine animator;
    IdleModeController idle_controller;
    std::vector<uint16_t> frame_a(DISPLAY_WIDTH * DISPLAY_HEIGHT);
    std::vector<uint16_t> frame_b(DISPLAY_WIDTH * DISPLAY_HEIGHT);
    std::vector<uint16_t>* cur = &frame_a;
    std::vector<uint16_t>* prev = &frame_b;
    bool first_frame = true;

    std::vector<uint8_t> dirty_tiles;
    std::vector<Rect> rects;
    int anim_burst = 0;

    auto last_log = std::chrono::steady_clock::now();
    double render_time_acc = 0.0;
    double spi_time_acc = 0.0;
    size_t bytes_sent_acc = 0;
    int render_frames = 0;
    int spi_frames = 0;
    size_t last_dirty_area = 0;
    size_t last_dirty_rects = 0;

    auto last_frame_time = std::chrono::steady_clock::now();

    while (running) {
        auto frame_start = std::chrono::steady_clock::now();
        auto dt_duration = frame_start - last_frame_time;
        double dt = std::chrono::duration_cast<std::chrono::duration<double>>(dt_duration).count();
        last_frame_time = frame_start;

        bool metrics_updated = metrics.Update();
        if (metrics_updated) {
            renderer.UpdateHistories(metrics);
            renderer.UpdateTickerText(metrics);
            anim_burst = BURST_FRAMES;
        }

        // Set animation targets
        animator.set_target("cpu", metrics.cpu_usage);
        animator.set_target("temp", metrics.temp);
        animator.set_target("net1", metrics.net1_mbps);
        animator.set_target("net2", metrics.net2_mbps);
        animator.step(dt);
        idle_controller.update(metrics, dt);

        auto render_start = std::chrono::steady_clock::now();
        double time_sec = std::chrono::duration_cast<std::chrono::duration<double>>(frame_start.time_since_epoch()).count();
        renderer.Render(metrics, animator, idle_controller, time_sec, *cur);
        auto render_end = std::chrono::steady_clock::now();
        render_time_acc += std::chrono::duration_cast<std::chrono::duration<double>>(render_end - render_start).count();
        render_frames++;

        bool send_frame = false;
        size_t dirty_area = 0;
        size_t bytes_sent = 0;

        if (first_frame) {
            send_frame = true;
            dirty_area = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
            bytes_sent = dirty_area * 3;
        } else {
            int dirty_tiles_count = 0;
            compute_dirty_tiles(cur->data(), prev->data(), DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                TILE_SIZE, dirty_tiles, dirty_tiles_count);
            if (dirty_tiles_count > 0) {
                build_rects_from_tiles(DISPLAY_WIDTH, DISPLAY_HEIGHT, TILE_SIZE, dirty_tiles, rects);
                size_t screen_area = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
                for (const auto& r : rects) {
                    dirty_area += static_cast<size_t>(r.w) * r.h;
                }
                double dirty_ratio = (screen_area > 0) ? (static_cast<double>(dirty_area) / screen_area) : 1.0;
                if (dirty_ratio > FULL_FRAME_THRESHOLD || static_cast<int>(rects.size()) > DIRTY_MAX_RECTS) {
                    send_frame = true;
                    dirty_area = screen_area;
                    bytes_sent = dirty_area * 3;
                } else {
                    send_frame = true;
                    bytes_sent = dirty_area * 3;
                }
            }
        }

        if (send_frame) {
            auto spi_start = std::chrono::steady_clock::now();
            if (first_frame || dirty_area == static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT) {
                display.Display(*cur);
                last_dirty_rects = 1;
            } else {
                for (const auto& r : rects) {
                    display.UpdateRect(r.x, r.y, r.w, r.h, cur->data(), DISPLAY_WIDTH);
                }
                last_dirty_rects = rects.size();
            }
            auto spi_end = std::chrono::steady_clock::now();
            spi_time_acc += std::chrono::duration_cast<std::chrono::duration<double>>(spi_end - spi_start).count();
            bytes_sent_acc += bytes_sent;
            spi_frames++;
            last_dirty_area = dirty_area;

            std::swap(cur, prev);
            first_frame = false;
        }

        if (anim_burst > 0) {
            anim_burst--;
        }

        auto frame_end = std::chrono::steady_clock::now();
        auto frame_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();

        int target_fps = TARGET_FPS;
        if (idle_controller.is_idle() && anim_burst == 0) {
            target_fps = std::max(1, IDLE_FPS);
        }
        int frame_time_ms = 1000 / std::max(1, target_fps);
        if (frame_duration_ms < frame_time_ms) {
            usleep((frame_time_ms - frame_duration_ms) * 1000);
        }

        auto now = std::chrono::steady_clock::now();
        auto log_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count();
        if (log_elapsed >= 5) {
            double sec = static_cast<double>(log_elapsed);
            double fps_render = render_frames / sec;
            double fps_spi = spi_frames / sec;
            double dirty_pct = 0.0;
            size_t screen_area = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
            if (screen_area > 0) {
                dirty_pct = 100.0 * static_cast<double>(last_dirty_area) / screen_area;
            }
            std::cerr << "LCD PERF: render_fps=" << fps_render
                      << " spi_fps=" << fps_spi
                      << " bytes_5s=" << bytes_sent_acc
                      << " dirty_rects=" << last_dirty_rects
                      << " dirty_pct=" << dirty_pct
                      << " render_ms=" << (render_time_acc * 1000.0)
                      << " spi_ms=" << (spi_time_acc * 1000.0)
                      << std::endl;
            render_time_acc = 0.0;
            spi_time_acc = 0.0;
            bytes_sent_acc = 0;
            render_frames = 0;
            spi_frames = 0;
            last_log = now;
        }
    }

    display.SetBacklight(false);
    metrics.Stop();
    return 0;
}
