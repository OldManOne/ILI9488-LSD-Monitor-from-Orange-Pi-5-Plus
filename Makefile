CXX = g++
DEPFLAGS = -MMD -MP
CXXFLAGS = -std=c++17 -Wall -O2 -pthread $(DEPFLAGS)
LDFLAGS = -lgpiodcxx -pthread

TARGET = lcd_monitor
PREFIX ?= /usr/local
SYSTEMD_DIR ?= /etc/systemd/system
SRCS = main.cpp ILI9488.cpp SystemMetrics.cpp Renderer.cpp AnimationEngine.cpp IdleModeController.cpp stb_truetype_impl.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -D -m 644 lcd-monitor.service $(DESTDIR)$(SYSTEMD_DIR)/lcd-monitor.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/lcd-monitor.service
