# LCD System Monitor для Orange Pi 5 Plus (ILI9488, 480×320)

Проект показывает системные метрики на SPI‑дисплее **ILI9488 480×320**. Оптимизирован под embedded‑сценарий (ARM64), использует частичную перерисовку, сглаживание показателей и устойчивую передачу по SPI.

## Что умеет
- Дашборд с графиками: **NET1/NET2**, **CPU/TEMP**
- Статусы сервисов (Docker/Disk/WG/WAN)
- Плавные анимации показателей
- Частичные обновления (dirty‑rect), чтобы не гонять полный кадр
- Поддержка **RCON** для вывода онлайна Minecraft (опционально)

## Аппаратные требования
- Orange Pi 5 Plus (RK3588, ARM64)
- SPI‑дисплей ILI9488, 480×320
- GPIO для DC/RST/BL

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

### ILI9488 параметры SPI
- **ILI9488_SPI_SPEED_HZ** — скорость SPI (по умолчанию 16MHz)
- **ILI9488_SPI_CHUNK** — размер чанка (по умолчанию 1024)
- **ILI9488_SPI_THROTTLE_US** — пауза между чанками

### Minecraft (RCON, опционально)
- **LCD_MC_RCON_HOST** — хост RCON
- **LCD_MC_RCON_PORT** — порт RCON
- **LCD_MC_RCON_PASS** — пароль RCON
- **LCD_MC_RCON_INTERVAL_MS** — интервал опроса

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

---
Автор: **OldManOne**
