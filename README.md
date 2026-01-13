# LCD System Monitor для Orange Pi 5 Plus (ILI9488, 480×320)

Проект показывает системные метрики на SPI‑дисплее **ILI9488 480×320**. Оптимизирован под embedded‑сценарий (ARM64), использует частичную перерисовку, сглаживание показателей и устойчивую передачу по SPI.

## Что умеет
- Дашборд с графиками: **NET1/NET2**, **CPU/TEMP**
- Плавные анимации показателей
- Частичные обновления (dirty‑rect), чтобы не гонять полный кадр
- Поддержка **RCON** для вывода онлайна Minecraft (опционально)
- Отдельный экран печати (Moonraker): превью модели + прогресс/ETA, с каруселью 10с/10с

## Аппаратные требования
- Orange Pi 5 Plus (RK3588, ARM64)
- SPI‑дисплей ILI9488, 480×320
- GPIO для DC/RST/BL

## Зависимости (для экрана печати)
Если используется интеграция с Moonraker (превью печати), нужен libcurl:
```bash
sudo apt install libcurl4-openssl-dev
```

## Сборка
```bash
make clean && make
```

## Запуск (пример)
```bash
sudo ./lcd_monitor
```

В проекте есть systemd‑unit (пример ниже), который использует переменные окружения:

```ini
[Service]
ExecStart=/home/nas/lcd_monitor_cpp/lcd_monitor
Environment=LCD_THEME=orange
Environment=LCD_FPS=20
Environment=LCD_NET_IF1=enP3p49s0
Environment=LCD_NET_IF2=enP4p65s0
```

## Переменные окружения (основные)
- **LCD_FPS** — целевой FPS
- **LCD_IDLE_FPS** — FPS в idle
- **LCD_DIRTY_TILE** — размер тайла dirty‑rect
- **LCD_FULL_FRAME_THRESHOLD** — порог полного кадра
- **LCD_THEME** — имя темы (`neutral`, `orange`, ...)
- **LCD_FONT** — путь к TTF‑шрифту (по умолчанию `/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf`)
- **LCD_NET_IF1 / LCD_NET_IF2** — интерфейсы сети
- **LCD_NET_AUTOSCALE** — авто‑масштаб графика

### Визуальные эффекты спарклайнов
**Все эффекты включены по умолчанию.** Для отключения установите значение `false` или `0`.

- **LCD_SPARKLINE_PULSE** — анимация пульсации конечной точки с glow-эффектом (по умолчанию `true`)
  - Частота пульсации зависит от активности (выше значение = быстрее пульсация)
  - Создает эффект "живого" графика

- **LCD_SPARKLINE_PEAK_HIGHLIGHT** — подсветка локальных пиков с bloom-эффектом (по умолчанию `true`)
  - Автоматически находит и подсвечивает максимумы на графике
  - Использует многослойное glow-свечение для глубины

- **LCD_SPARKLINE_GRADIENT_LINE** — градиентная окраска линии по значению (по умолчанию `true`)
  - Низкие значения: приглушенные тона
  - Высокие значения: теплые/яркие оттенки
  - Плавный переход между зонами

- **LCD_SPARKLINE_PARTICLES** — частицы-следы при резких изменениях (по умолчанию `true`)
  - Появляются при скачках значений >15%
  - Создают эффект движения данных

- **LCD_SPARKLINE_ENHANCED_FILL** — двухступенчатая градиентная заливка (по умолчанию `true`)
  - Яркая зона у линии графика
  - Плавное затухание к основанию
  - Больше глубины и объема

- **LCD_SPARKLINE_DYNAMIC_WIDTH** — динамическая толщина линии (по умолчанию `true`)
  - Линия утолщается на пиках значений
  - Создает эффект "потока"

- **LCD_SPARKLINE_BASELINE_SHIMMER** — мерцание базовой линии (по умолчанию `true`)
  - Анимированная пунктирная линия
  - Волнообразное изменение интенсивности

- **LCD_SPARKLINE_SHADOW** — тень под графиком для глубины (по умолчанию `true`)
  - Смещение на 2 пикселя вниз
  - Создает эффект приподнятости

- **LCD_SPARKLINE_COLOR_ZONES** — цветовые акценты для разных зон (по умолчанию `true`)
  - Низкие значения: холодные оттенки
  - Средние значения: нейтральные
  - Высокие значения: теплые оттенки

- **LCD_SPARKLINE_SMOOTH_TRANSITIONS** — плавные цветовые переходы idle/active (по умолчанию `true`)
  - Улучшенные переходы при изменении режима
  - Более насыщенные цвета в активном режиме

**Пример отключения всех эффектов:**
```bash
LCD_SPARKLINE_PULSE=false \
LCD_SPARKLINE_PEAK_HIGHLIGHT=false \
LCD_SPARKLINE_GRADIENT_LINE=false \
LCD_SPARKLINE_PARTICLES=false \
LCD_SPARKLINE_ENHANCED_FILL=false \
LCD_SPARKLINE_DYNAMIC_WIDTH=false \
LCD_SPARKLINE_BASELINE_SHIMMER=false \
LCD_SPARKLINE_SHADOW=false \
LCD_SPARKLINE_COLOR_ZONES=false \
LCD_SPARKLINE_SMOOTH_TRANSITIONS=false \
./lcd_monitor
```

**Пример включения только базовых эффектов (минимальная нагрузка):**
```bash
LCD_SPARKLINE_PULSE=true \
LCD_SPARKLINE_GRADIENT_LINE=true \
LCD_SPARKLINE_ENHANCED_FILL=true \
LCD_SPARKLINE_PEAK_HIGHLIGHT=false \
LCD_SPARKLINE_PARTICLES=false \
LCD_SPARKLINE_DYNAMIC_WIDTH=false \
LCD_SPARKLINE_BASELINE_SHIMMER=false \
LCD_SPARKLINE_SHADOW=false \
LCD_SPARKLINE_COLOR_ZONES=false \
./lcd_monitor
```

### ILI9488 параметры SPI
- **ILI9488_SPI_SPEED_HZ** — скорость SPI (по умолчанию 16MHz)
- **ILI9488_SPI_CHUNK** — размер чанка (по умолчанию 1024)
- **ILI9488_SPI_THROTTLE_US** — пауза между чанками

### Minecraft (RCON, опционально)
- **LCD_MC_RCON_HOST** — хост RCON
- **LCD_MC_RCON_PORT** — порт RCON
- **LCD_MC_RCON_PASS** — пароль RCON
- **LCD_MC_RCON_INTERVAL_MS** — интервал опроса

### Moonraker (Print Screen, опционально)
- **LCD_PRINTER_URL** — базовый URL Moonraker (например `http://192.168.1.103:7125`)
- **LCD_PRINTER_POLL_MS** — интервал опроса (мс), по умолчанию 5000

Экран печати появляется при `printing/paused`, чередуется с основным экраном 10с/10с.
После завершения/ошибки экран печати остаётся ещё 60 секунд, затем скрывается.

## Архитектура
- `Renderer.*` — отрисовка UI
- `ILI9488.*` — драйвер SPI‑дисплея
- `SystemMetrics.*` — сбор метрик (в фоне)
- `AnimationEngine.*` — сглаживание
- `IdleModeController.*` — idle‑режим

## Примечания
- ILI9488 работает в **RGB666** (COLMOD=0x66), SPI 16MHz стабильно.
- Полный кадр по SPI ограничен по FPS, поэтому используется частичная перерисовка.


## Установка
```bash
make clean && make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable lcd-monitor
sudo systemctl start lcd-monitor
```

## Права доступа

Для работы с SPI и GPIO нужны права:
```bash
# Добавить пользователя в группы (альтернатива запуску от root)
sudo usermod -aG spi,gpio $USER

# Или создать udev-правила для устройств
echo 'SUBSYSTEM=="spidev", MODE="0666"' | sudo tee /etc/udev/rules.d/50-spi.rules
echo 'SUBSYSTEM=="gpio", MODE="0666"' | sudo tee /etc/udev/rules.d/50-gpio.rules
sudo udevadm control --reload-rules
```

## Логи и отладка
```bash
# Просмотр логов
journalctl -u lcd-monitor -f

# Запуск с отладкой
LCD_DEBUG=1 ./lcd_monitor

# Проверка SPI
ls -la /dev/spidev*
```


## Troubleshooting

### SPI не работает
```bash
# Проверить наличие устройства
ls -la /dev/spidev*

# Проверить скорость (если артефакты — снизить)
ILI9488_SPI_SPEED_HZ=8000000 ./lcd_monitor
```

### Ошибки systemd
```bash
# Статус сервиса
systemctl status lcd-monitor

# Подробные логи
journalctl -u lcd-monitor -f --no-pager
```

### Права доступа
Если ошибка "Permission denied" на /dev/spidev* или /dev/gpiochip*:
- Либо запускать от root
- Либо настроить udev-правила (см. секцию "Права доступа")

## Пример конфигурации

Создайте `/etc/default/lcd_monitor`:
```bash
LCD_THEME=orange
LCD_FPS=10
LCD_NET_IF1=eth0
LCD_NET_IF2=eth1
LCD_FONT=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
```

---
Автор: **OldManOne**

## Changelog (кратко)
### 2026‑01
- **Визуальные эффекты спарклайнов** — добавлено 10 настраиваемых эффектов:
  - Пульсация конечной точки с glow-эффектом
  - Подсветка пиков с bloom-эффектом
  - Градиентная окраска линии по значению
  - Частицы-следы при резких изменениях
  - Двухступенчатая градиентная заливка
  - Динамическая толщина линии
  - Мерцание базовой линии
  - Тень под графиком
  - Цветовые акценты для разных зон
  - Плавные цветовые переходы
- Добавлен Print Screen для Moonraker (превью модели + прогресс/ETA)
- Логика карусели 10с/10с и "хвост" 60с после завершения печати
- Частичная перерисовка (dirty‑rect) для повышения FPS


