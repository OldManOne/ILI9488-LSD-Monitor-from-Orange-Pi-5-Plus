#include "SystemMetrics.h"
#include "utils.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include <map>
#include <thread>
#include <mutex>
#include <regex>
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <sys/statvfs.h>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// --- Constructor / Destructor ---

SystemMetrics::SystemMetrics() {
    debug_ = getenv_bool("LCD_DEBUG", false);
    wg_active_window_s_ = getenv_int("LCD_WG_ACTIVE_SEC", wg_active_window_s_);
    net_if1_ = getenv_string("LCD_NET_IF1", net_if1_);
    net_if2_ = getenv_string("LCD_NET_IF2", net_if2_);
    mc_rcon_host_ = getenv_string("LCD_MC_RCON_HOST", mc_rcon_host_);
    mc_rcon_port_ = getenv_int("LCD_MC_RCON_PORT", mc_rcon_port_);
    mc_rcon_pass_ = getenv_string("LCD_MC_RCON_PASS", mc_rcon_pass_);
    mc_rcon_timeout_ms_ = getenv_int("LCD_MC_RCON_TIMEOUT_MS", mc_rcon_timeout_ms_);
    mc_rcon_interval_ms_ = getenv_int("LCD_MC_RCON_INTERVAL_MS", mc_rcon_interval_ms_);
}

SystemMetrics::~SystemMetrics() {
    Stop();
}

void SystemMetrics::Start() {
    if (!running_) {
        running_ = true;
        wan_worker_ = std::thread(&SystemMetrics::wan_check_worker, this);
        metrics_worker_ = std::thread(&SystemMetrics::metrics_worker_func, this);
    }
}

void SystemMetrics::Stop() {
    if (running_) {
        running_ = false;
        if (wan_worker_.joinable()) {
            wan_worker_.join();
        }
        if (metrics_worker_.joinable()) {
            metrics_worker_.join();
        }
    }
}

// --- Public Methods ---

bool SystemMetrics::Update() {
    if (!metrics_pending_) return false;
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    cpu_usage = pending_snapshot_.cpu_usage;
    mem_percent = pending_snapshot_.mem_percent;
    mem_used_mb = pending_snapshot_.mem_used_mb;
    temp = pending_snapshot_.temp;
    net1_mbps = pending_snapshot_.net1_mbps;
    net2_mbps = pending_snapshot_.net2_mbps;
    docker_running = pending_snapshot_.docker_running;
    disk_percent = pending_snapshot_.disk_percent;
    wg_active_peers = pending_snapshot_.wg_active_peers;
    uptime_seconds = pending_snapshot_.uptime_seconds;
    mc_online = pending_snapshot_.mc_online;
    mc_max = pending_snapshot_.mc_max;
    metrics_pending_ = false;
    return true;
}

// --- Private Helper Methods ---

std::string SystemMetrics::exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

bool SystemMetrics::exec_with_timeout(const char* cmd, int timeout_sec, std::string& output) {
    output.clear();
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // child
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }

    // parent
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    int elapsed_ms = 0;
    const int step_ms = 50;
    bool finished = false;
    while (elapsed_ms < timeout_sec * 1000) {
        char buf[256];
        ssize_t r = read(pipefd[0], buf, sizeof(buf));
        while (r > 0) {
            output.append(buf, static_cast<size_t>(r));
            r = read(pipefd[0], buf, sizeof(buf));
        }

        int status = 0;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            finished = true;
            break;
        }
        usleep(step_ms * 1000);
        elapsed_ms += step_ms;
    }

    if (!finished) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    close(pipefd[0]);
    return finished;
}

std::string SystemMetrics::get_wan_status() const {
    std::lock_guard<std::mutex> lock(wan_mutex_);
    return wan_status;
}

// --- Metric Gathering ---

double SystemMetrics::getCPUUsage() {
    std::ifstream stat_file("/proc/stat");
    std::string line;
    std::getline(stat_file, line);
    std::stringstream ss(line);

    std::string cpu_label;
    ss >> cpu_label;
    if (cpu_label != "cpu") return 0.0;

    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    uint64_t current_idle = idle + iowait;
    uint64_t current_total = user + nice + system + current_idle + irq + softirq + steal;

    double usage = 0.0;
    if (prev_cpu_total_ > 0) {
        double total_delta = static_cast<double>(current_total - prev_cpu_total_);
        double idle_delta = static_cast<double>(current_idle - prev_cpu_idle_);
        if (total_delta > 0) {
            usage = 100.0 * (1.0 - idle_delta / total_delta);
        }
    }

    prev_cpu_total_ = current_total;
    prev_cpu_idle_ = current_idle;
    return usage;
}

void SystemMetrics::getMemoryUsage(double& percent, int& used_mb) {
    std::ifstream meminfo_file("/proc/meminfo");
    std::string line;
    uint64_t mem_total = 0, mem_available = 0;

    while (std::getline(meminfo_file, line)) {
        std::stringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "MemTotal:") {
            ss >> mem_total;
        } else if (key == "MemAvailable:") {
            ss >> mem_available;
        }
    }

    if (mem_total > 0) {
        uint64_t mem_used = mem_total - mem_available;
        percent = (static_cast<double>(mem_used) / mem_total) * 100.0;
        used_mb = static_cast<int>(mem_used / 1024);
    }
}

double SystemMetrics::getCPUTemp() {
    for (int i = 0; i < 5; ++i) {
        std::string path = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
        std::ifstream temp_file(path);
        if (temp_file.good()) {
            int temp_milli_c;
            temp_file >> temp_milli_c;
            double temp_c = static_cast<double>(temp_milli_c) / 1000.0;
            if (temp_c > 20 && temp_c < 120) {
                return temp_c;
            }
        }
    }
    return 0.0;
}

void SystemMetrics::getNetworkSpeed(const std::string& interface_name, double& speed) {
    uint64_t current_bytes = 0;
    bool success = false;

    // Try fast path first: read from /sys/class/net (instant, no shell exec)
    try {
        std::ifstream rx_file("/sys/class/net/" + interface_name + "/statistics/rx_bytes");
        std::ifstream tx_file("/sys/class/net/" + interface_name + "/statistics/tx_bytes");
        if (rx_file.is_open() && tx_file.is_open()) {
            uint64_t rx, tx;
            rx_file >> rx;
            tx_file >> tx;
            current_bytes = rx + tx;
            success = true;
        }
    } catch (...) {
        success = false;
    }

    // Fallback to ethtool only if sysfs failed (rare case)
    if (!success) {
        std::string cmd = "ethtool -S " + interface_name;
        std::string output;
        if (exec_with_timeout(cmd.c_str(), 5, output)) {
            std::stringstream ss(output);
            std::string line;
            uint64_t rx_octets = 0, tx_octets = 0;
            while (std::getline(ss, line)) {
                if (line.find("rx_octets:") != std::string::npos) {
                    sscanf(line.c_str(), "     rx_octets: %lu", &rx_octets);
                }
                if (line.find("tx_octets:") != std::string::npos) {
                    sscanf(line.c_str(), "     tx_octets: %lu", &tx_octets);
                }
            }
            current_bytes = rx_octets + tx_octets;
            success = true;
        }
    }

    if (!success) {
        speed = 0.0;
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (prev_net_stats_.count(interface_name)) {
        auto& prev = prev_net_stats_[interface_name];
        double time_delta = std::chrono::duration<double>(now - prev.time).count();
        if (time_delta > 0) {
            uint64_t bytes_delta = (current_bytes > prev.bytes) ? (current_bytes - prev.bytes) : 0;
            speed = (static_cast<double>(bytes_delta) * 8) / (time_delta * 1000000.0);
        }
    }

    prev_net_stats_[interface_name] = {current_bytes, now};
}

int SystemMetrics::getUptime() {
    std::ifstream uptime_file("/proc/uptime");
    double uptime;
    uptime_file >> uptime;
    return static_cast<int>(uptime);
}

int SystemMetrics::updateDockerInfo() {
    try {
        std::string output;
        if (!exec_with_timeout("docker ps -q 2>/dev/null", 5, output)) {
            return -1;
        }
        int count = 0;
        std::stringstream ss(output);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty()) count++;
        }
        return count;
    } catch (...) {
        return -1;
    }
}

int SystemMetrics::updateDiskUsage() {
    try {
        std::ifstream statf("/proc/self/mounts");
        std::string line;
        while (std::getline(statf, line)) {
            if (line.find(" / ") != std::string::npos) {
                struct statvfs vfs;
                if (statvfs("/", &vfs) == 0) {
                    unsigned long long total = vfs.f_blocks * vfs.f_frsize;
                    unsigned long long free = vfs.f_bavail * vfs.f_frsize;
                    unsigned long long used = total - free;
                    if (total > 0) {
                        return static_cast<int>((used * 100ULL) / total);
                    }
                }
                return -1;
            }
        }
        // fallback
        struct statvfs vfs;
        if (statvfs("/", &vfs) == 0) {
            unsigned long long total = vfs.f_blocks * vfs.f_frsize;
            unsigned long long free = vfs.f_bavail * vfs.f_frsize;
            unsigned long long used = total - free;
            if (total > 0) {
                return static_cast<int>((used * 100ULL) / total);
            }
        }
    } catch (...) {
    }
    return -1;
}

int SystemMetrics::updateWireGuardPeers() {
    try {
        // Получаем актуальный список разрешённых peer'ов из wg-easy.db
        // чтобы отображение на дисплее совпадало с UI wg-easy.
        std::unordered_set<std::string> enabled_pubkeys;
        {
            std::string db_out;
            const char* cmd = R"(sqlite3 /etc/wireguard/wg-easy.db "select public_key from clients_table where enabled=1;")";
            if (exec_with_timeout(cmd, 5, db_out) && !db_out.empty()) {
                std::stringstream dss(db_out);
                std::string pk;
                while (std::getline(dss, pk)) {
                    if (!pk.empty()) enabled_pubkeys.insert(pk);
                }
            }
        }

        std::string output;
        if (!exec_with_timeout("wg show wg0 latest-handshakes 2>/dev/null", 5, output)) {
            return -1;
        }
        if (output.empty()) {
            return 0;
        }
        std::stringstream ss(output);
        std::string line;
        int count = 0;
        auto now = std::chrono::system_clock::now();
        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            std::stringstream ls(line);
            std::string pubkey;
            long long ts = 0;
            ls >> pubkey >> ts;
            if (ts <= 0) continue;
            if (!enabled_pubkeys.empty() && enabled_pubkeys.find(pubkey) == enabled_pubkeys.end()) {
                continue; // пропускаем peer'ов, которых нет в базе wg-easy или выключены
            }
            if ((now_s - ts) <= wg_active_window_s_) {
                count++;
            }
        }
        return count;
    } catch (...) {
        return -1;
    }
}

std::pair<int, int> SystemMetrics::updateMinecraftPlayers() {
    if (mc_rcon_pass_.empty()) {
        return {-1, -1};
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - mc_last_poll_).count();
    if (elapsed < mc_rcon_interval_ms_) return {mc_cached_online_, mc_cached_max_};
    mc_last_poll_ = now;

    auto read_full = [&](int fd, void* buf, size_t len, int timeout_ms) -> bool {
        size_t off = 0;
        while (off < len) {
            pollfd pfd{fd, POLLIN, 0};
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;
            ssize_t r = read(fd, static_cast<char*>(buf) + off, len - off);
            if (r <= 0) return false;
            off += static_cast<size_t>(r);
        }
        return true;
    };

    auto write_full = [&](int fd, const void* buf, size_t len) -> bool {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, static_cast<const char*>(buf) + off, len - off);
            if (w <= 0) return false;
            off += static_cast<size_t>(w);
        }
        return true;
    };

    auto send_packet = [&](int fd, int32_t id, int32_t type, const std::string& payload) -> bool {
        int32_t len = static_cast<int32_t>(4 + 4 + payload.size() + 2);
        std::vector<uint8_t> buf(static_cast<size_t>(4 + len));
        std::memcpy(buf.data(), &len, 4);
        std::memcpy(buf.data() + 4, &id, 4);
        std::memcpy(buf.data() + 8, &type, 4);
        std::memcpy(buf.data() + 12, payload.data(), payload.size());
        buf[12 + payload.size()] = 0;
        buf[12 + payload.size() + 1] = 0;
        return write_full(fd, buf.data(), buf.size());
    };

    auto recv_packet = [&](int fd, int32_t& id, int32_t& type, std::string& payload) -> bool {
        int32_t len = 0;
        if (!read_full(fd, &len, 4, mc_rcon_timeout_ms_)) return false;
        if (len < 10 || len > 4096) return false;
        std::vector<char> buf(static_cast<size_t>(len));
        if (!read_full(fd, buf.data(), buf.size(), mc_rcon_timeout_ms_)) return false;
        std::memcpy(&id, buf.data(), 4);
        std::memcpy(&type, buf.data() + 4, 4);
        if (len > 10) {
            payload.assign(buf.data() + 8, buf.data() + len - 2);
        } else {
            payload.clear();
        }
        return true;
    };

    int sock = -1;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(mc_rcon_host_.c_str(), std::to_string(mc_rcon_port_).c_str(), &hints, &res) != 0) {
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }
    for (addrinfo* p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }

    int32_t id = 1;
    if (!send_packet(sock, id, 3, mc_rcon_pass_)) {
        close(sock);
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }
    int32_t rid = 0, rtype = 0;
    std::string rpayload;
    if (!recv_packet(sock, rid, rtype, rpayload) || rid == -1) {
        close(sock);
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }

    id = 2;
    if (!send_packet(sock, id, 2, "list")) {
        close(sock);
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }
    if (!recv_packet(sock, rid, rtype, rpayload)) {
        close(sock);
        mc_cached_online_ = -1;
        mc_cached_max_ = -1;
        return {mc_cached_online_, mc_cached_max_};
    }
    close(sock);

    static const std::regex re(R"(There are (\d+) of a max of (\d+) players online)");
    std::smatch m;
    if (std::regex_search(rpayload, m, re) && m.size() >= 3) {
        try {
            mc_cached_online_ = std::stoi(m[1].str());
            mc_cached_max_ = std::stoi(m[2].str());
            return {mc_cached_online_, mc_cached_max_};
        } catch (...) {
            mc_cached_online_ = -1;
            mc_cached_max_ = -1;
            return {mc_cached_online_, mc_cached_max_};
        }
    }
    mc_cached_online_ = -1;
    mc_cached_max_ = -1;
    return {mc_cached_online_, mc_cached_max_};
}

void SystemMetrics::metrics_worker_func() {
    using namespace std::chrono_literals;
    const int interval_ms = 100; // faster sampling for smoother sparklines
    while (running_) {
        MetricsSnapshot snap;

        snap.cpu_usage = getCPUUsage();
        getMemoryUsage(snap.mem_percent, snap.mem_used_mb);
        snap.temp = getCPUTemp();
        getNetworkSpeed(net_if1_, snap.net1_mbps);
        getNetworkSpeed(net_if2_, snap.net2_mbps);
        snap.uptime_seconds = getUptime();
        snap.docker_running = updateDockerInfo();
        snap.disk_percent = updateDiskUsage();
        snap.wg_active_peers = updateWireGuardPeers();
        auto mc = updateMinecraftPlayers();
        snap.mc_online = mc.first;
        snap.mc_max = mc.second;

        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            pending_snapshot_ = snap;
            metrics_pending_ = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

// --- WAN Monitoring ---

double SystemMetrics::ping(const std::string& host, double timeout_s) {
    try {
        std::string cmd = "ping -c 1 -W " + std::to_string((int)timeout_s) + " " + host;
        std::string output = exec(cmd.c_str());
        
        std::regex time_regex("time=([0-9]+\\.?[0-9]*) ms");
        std::smatch match;
        if (std::regex_search(output, match, time_regex) && match.size() > 1) {
            return std::stod(match[1].str());
        }
        return -1.0; // Indicate failure
    } catch (...) {
        return -1.0;
    }
}

void SystemMetrics::update_wan_status_from_history(const std::string& new_state) {
    std::lock_guard<std::mutex> lock(wan_mutex_);
    
    wan_status_history_.push_back(new_state);
    if (wan_status_history_.size() > wan_history_size_) {
        wan_status_history_.pop_front();
    }

    // Stabilization: DOWN has priority
    bool down_found = false;
    for(const auto& state : wan_status_history_) {
        if (state == "DOWN") {
            down_found = true;
            break;
        }
    }

    if (down_found) {
        wan_status = "DOWN";
    } else if (wan_status_history_.size() >= 2) {
        // Count occurrences and find the most common
        std::map<std::string, int> counts;
        for(const auto& state : wan_status_history_) {
            counts[state]++;
        }
        
        auto max_it = std::max_element(counts.begin(), counts.end(), 
            [](const auto& a, const auto& b){ return a.second < b.second; });
        
        if (max_it != counts.end()) {
            wan_status = max_it->first;
        }
    } else {
        wan_status = new_state;
    }
}


void SystemMetrics::wan_check_worker() {
    const double rtt_threshold_ms = 200.0;
    const std::vector<std::string> targets = {"1.1.1.1", "8.8.8.8"};

    while (running_) {
        try {
            std::string current_status = "DOWN";

            std::string route_output = exec("ip route show default");
            if (route_output.find("default via") != std::string::npos) {
                 for (const auto& target : targets) {
                    double rtt = ping(target, 2.0);
                    if (rtt >= 0) {
                        current_status = (rtt > rtt_threshold_ms) ? "DEGRADED" : "OK";
                        break; // Exit on first successful ping
                    }
                }
            }
            
            update_wan_status_from_history(current_status);
        } catch (const std::exception& e) {
            if (debug_) {
                std::cerr << "WAN worker error: " << e.what() << std::endl << std::flush;
            }
        } catch (...) {
            if (debug_) {
                std::cerr << "WAN worker error: unknown exception" << std::endl << std::flush;
            }
        }

        // Sleep for 5 seconds before the next check
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
