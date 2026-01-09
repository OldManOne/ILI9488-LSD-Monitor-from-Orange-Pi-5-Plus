#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

struct ImageRGBA {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> data; // RGBA
};

struct PrinterMetrics {
    std::string state;
    std::string filename;
    float progress01 = 0.0f;
    int elapsed_sec = 0;
    int eta_sec = -1;
    bool active = false;
    bool had_job = false;
    double last_active_ts = 0.0;
    std::string thumb_relpath;
    std::shared_ptr<ImageRGBA> thumb_rgba;
};

class PrinterClient {
public:
    explicit PrinterClient(const std::string& base_url);
    ~PrinterClient();

    void Start();
    void Stop();

    PrinterMetrics GetSnapshot() const;

private:
    void worker();
    bool httpGet(const std::string& url, std::string& out) const;
    bool httpGetBinary(const std::string& url, std::vector<unsigned char>& out) const;

    std::string base_url_;
    int poll_ms_ = 5000;

    mutable std::mutex mutex_;
    PrinterMetrics metrics_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    std::string last_filename_;
    std::string last_thumb_relpath_;
};
