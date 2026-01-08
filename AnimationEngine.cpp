#include "AnimationEngine.h"
#include <algorithm> // For std::min

AnimationEngine::AnimationEngine() : interpolation_speed(0.3) {}

void AnimationEngine::set_target(const std::string& key, double target_value) {
    auto it = values.find(key);
    if (it != values.end()) {
        it->second.target = target_value;
    } else {
        values[key] = {target_value, target_value};
    }
}

void AnimationEngine::step(double dt) {
    for (auto& pair : values) {
        AnimatedValue& val = pair.second;
        double interpolation_factor = std::min(1.0, interpolation_speed * dt * 10.0);
        double new_value = val.current + (val.target - val.current) * interpolation_factor;

        // Prevent negative values if the target is non-negative
        if (val.target >= 0 && new_value < 0) {
            new_value = 0.0;
        }

        val.current = new_value;
    }
}

double AnimationEngine::get(const std::string& key, double default_value) {
    auto it = values.find(key);
    if (it != values.end()) {
        return it->second.current;
    }
    return default_value;
}
