#include "PrinterClient.h"
#include "utils.h"
#include "json.hpp"
#include "stb_image.h"
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

static size_t write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<const char*>(contents), total);
    return total;
}

static size_t write_bin_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* vec = static_cast<std::vector<unsigned char>*>(userp);
    auto* c = static_cast<unsigned char*>(contents);
    vec->insert(vec->end(), c, c + total);
    return total;
}

static double steady_seconds() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::duration<double>>(now).count();
}

static std::string url_encode_query(const std::string& s) {
    std::ostringstream out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << hex[c >> 4] << hex[c & 0x0F];
        }
    }
    return out.str();
}

static std::string url_encode_path(const std::string& s) {
    std::ostringstream out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c == '/') {
            out << c;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << hex[c >> 4] << hex[c & 0x0F];
        }
    }
    return out.str();
}

PrinterClient::PrinterClient(const std::string& base_url)
    : base_url_(base_url) {
    poll_ms_ = getenv_int("LCD_PRINTER_POLL_MS", 5000);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

PrinterClient::~PrinterClient() {
    Stop();
    curl_global_cleanup();
}

void PrinterClient::Start() {
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&PrinterClient::worker, this);
}

void PrinterClient::Stop() {
    if (!running_) return;
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

PrinterMetrics PrinterClient::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

bool PrinterClient::httpGet(const std::string& url, std::string& out) const {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && code == 200);
}

bool PrinterClient::httpGetBinary(const std::string& url, std::vector<unsigned char>& out) const {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bin_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && code == 200 && !out.empty());
}

void PrinterClient::worker() {
    while (running_) {
        std::string body;
        std::string url = base_url_ + "/printer/objects/query?print_stats&virtual_sdcard";
        if (httpGet(url, body)) {
            auto j = json::parse(body, nullptr, false);
            if (!j.is_discarded()) {
                auto status = j["result"]["status"];
                auto ps = status["print_stats"];
                auto vsd = status["virtual_sdcard"];

                std::string state = ps.value("state", "");
                std::string filename = ps.value("filename", "");
                double elapsed = ps.value("print_duration", 0.0);
                double progress = vsd.value("progress", 0.0);

                bool active = (state == "printing" || state == "paused");
                double now = steady_seconds();

                int eta = -1;
                if (progress > 0.03 && elapsed > 5.0) {
                    double total = elapsed / progress;
                    double rem = total - elapsed;
                    if (rem > 0) eta = static_cast<int>(rem);
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    metrics_.state = state;
                    metrics_.filename = filename;
                    metrics_.progress01 = static_cast<float>(progress);
                    metrics_.elapsed_sec = static_cast<int>(elapsed);
                    metrics_.eta_sec = eta;
                    metrics_.active = active;
                    if (active) {
                        metrics_.had_job = true;
                        metrics_.last_active_ts = now;
                    }
                }

                if (!filename.empty() && filename != last_filename_) {
                    last_filename_ = filename;
                    std::string meta_body;
                    std::string meta_url = base_url_ + "/server/files/metadata?filename=" + url_encode_query(filename);
                    if (httpGet(meta_url, meta_body)) {
                        auto jm = json::parse(meta_body, nullptr, false);
                        if (!jm.is_discarded()) {
                            auto thumbs = jm["result"]["thumbnails"];
                            int best_area = -1;
                            std::string best_rel;
                            for (auto& th : thumbs) {
                                int w = th.value("width", 0);
                                int h = th.value("height", 0);
                                int area = w * h;
                                if (area > best_area) {
                                    best_area = area;
                                    best_rel = th.value("relative_path", "");
                                }
                            }
                            if (!best_rel.empty() && best_rel != last_thumb_relpath_) {
                                last_thumb_relpath_ = best_rel;
                                std::string thumb_url = base_url_ + "/server/files/gcodes/" + url_encode_path(best_rel);
                                std::vector<unsigned char> png;
                                if (httpGetBinary(thumb_url, png)) {
                                    int w = 0, h = 0, ch = 0;
                                    unsigned char* img = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &ch, 4);
                                    if (img && w > 0 && h > 0) {
                                        auto image = std::make_shared<ImageRGBA>();
                                        image->w = w;
                                        image->h = h;
                                        image->data.assign(img, img + (w * h * 4));
                                        stbi_image_free(img);
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        metrics_.thumb_rgba = image;
                                        metrics_.thumb_relpath = best_rel;
                                    } else if (img) {
                                        stbi_image_free(img);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
    }
}
