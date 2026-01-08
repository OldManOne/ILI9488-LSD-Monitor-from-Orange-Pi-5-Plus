#ifndef ANIMATION_ENGINE_H
#define ANIMATION_ENGINE_H

#include <map>
#include <string>

class AnimationEngine {
public:
    AnimationEngine();

    // Установить целевое значение для плавной анимации
    void set_target(const std::string& key, double target_value);

    // Выполнить шаг интерполяции, dt - время в долях секунды с прошлого кадра
    void step(double dt);

    // Получить текущее плавное значение
    double get(const std::string& key, double default_value = 0.0);

private:
    struct AnimatedValue {
        double current;
        double target;
    };

    std::map<std::string, AnimatedValue> values;
    double interpolation_speed;
};

#endif // ANIMATION_ENGINE_H
