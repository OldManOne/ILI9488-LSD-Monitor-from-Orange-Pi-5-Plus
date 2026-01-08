CXX = g++
DEPFLAGS = -MMD -MP
CXXFLAGS = -std=c++17 -Wall -O2 -pthread $(DEPFLAGS)
LDFLAGS = -lgpiodcxx -pthread

TARGET = lcd_monitor
SRCS = main.cpp ILI9488.cpp SystemMetrics.cpp Renderer.cpp AnimationEngine.cpp IdleModeController.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)
