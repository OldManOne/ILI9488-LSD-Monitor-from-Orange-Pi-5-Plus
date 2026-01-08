#ifndef IDLE_MODE_CONTROLLER_H
#define IDLE_MODE_CONTROLLER_H

#include "SystemMetrics.h"
#include <chrono>

class IdleModeController {
public:
    IdleModeController();

    // Обновить состояние idle режима
    void update(const SystemMetrics& metrics, double dt);

    bool is_idle() const;
    double get_transition_progress() const;

private:
    const double idle_threshold_seconds;
    std::chrono::steady_clock::time_point idle_start_time;
    bool _is_idle;
    bool idle_timer_running;
    double transition_progress; // 0.0 = active, 1.0 = full idle
};

#endif // IDLE_MODE_CONTROLLER_H
