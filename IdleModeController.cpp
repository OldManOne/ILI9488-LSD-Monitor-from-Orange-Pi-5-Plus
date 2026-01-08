#include "IdleModeController.h"
#include <cmath>

IdleModeController::IdleModeController()
    : idle_threshold_seconds(30.0),
      _is_idle(false),
      idle_timer_running(false),
      transition_progress(0.0) {}

bool IdleModeController::is_idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return _is_idle;
}

double IdleModeController::get_transition_progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transition_progress;
}

void IdleModeController::update(const SystemMetrics& metrics, double dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool system_is_idle = (metrics.cpu_usage < 10.0 &&
                           metrics.temp < 50.0 &&
                           metrics.net1_mbps < 10.0 &&
                           metrics.net2_mbps < 10.0);

    if (system_is_idle) {
        if (!idle_timer_running) {
            idle_start_time = std::chrono::steady_clock::now();
            idle_timer_running = true;
        } else {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - idle_start_time).count();
            if (elapsed > idle_threshold_seconds) {
                _is_idle = true;
            }
        }
    } else {
        idle_timer_running = false;
        _is_idle = false;
    }

    // Smooth transition
    double target_progress = _is_idle ? 1.0 : 0.0;
    // Frame-rate independent smoothing: tau = 0.3 seconds
    const double tau = 0.3;
    double alpha = 1.0 - std::exp(-dt / tau);
    transition_progress += (target_progress - transition_progress) * alpha;

    // Clamp to [0, 1] for safety
    if (transition_progress < 0.0) transition_progress = 0.0;
    if (transition_progress > 1.0) transition_progress = 1.0;
}
