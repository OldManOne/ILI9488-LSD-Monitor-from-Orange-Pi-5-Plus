#ifndef SYSTEM_METRICS_H
#define SYSTEM_METRICS_H

#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <utility>

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    void Start();
    void Stop();

    bool Update();

    double cpu_usage = 0.0;
    double mem_percent = 0.0;
    int mem_used_mb = 0;
    double temp = 0.0;
    double net1_mbps = 0.0;
    double net2_mbps = 0.0;
    int uptime_seconds = 0;
    std::string wan_status = "CHECKING";
    int docker_running = -1;
    int disk_percent = -1;
    int wg_active_peers = -1;
    int mc_online = -1;
    int mc_max = -1;
    std::string get_wan_status() const;

private:
    std::string exec(const char* cmd);
    bool exec_with_timeout(const char* cmd, int timeout_sec, std::string& output);

    double getCPUUsage();
    void getMemoryUsage(double& percent, int& used_mb);
    double getCPUTemp();
    void getNetworkSpeed(const std::string& interface_name, double& speed);
    int getUptime();
    int updateDockerInfo();
    int updateDiskUsage();
    int updateWireGuardPeers();
    std::pair<int, int> updateMinecraftPlayers();
    void metrics_worker_func();
    
    // WAN Monitoring
    void wan_check_worker();
    void update_wan_status_from_history(const std::string& new_state);
    double ping(const std::string& host, double timeout_s);

    // For CPU calculation
    uint64_t prev_cpu_total_ = 0;
    uint64_t prev_cpu_idle_ = 0;

    // For network speed calculation
    struct NetStats {
        uint64_t bytes;
        std::chrono::steady_clock::time_point time;
    };
    std::map<std::string, NetStats> prev_net_stats_;
    
    // For async WAN status
    std::thread wan_worker_;
    std::thread metrics_worker_;
    mutable std::mutex wan_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> metrics_pending_{false};
    std::deque<std::string> wan_status_history_;
    const size_t wan_history_size_ = 3;

    bool debug_ = false;
    int wg_active_window_s_ = 120;
    std::string net_if1_ = "eth0";
    std::string net_if2_ = "eth1";
    std::string mc_rcon_host_ = "127.0.0.1";
    std::string mc_rcon_pass_;
    int mc_rcon_port_ = 25575;
    int mc_rcon_timeout_ms_ = 1500;
    int mc_rcon_interval_ms_ = 2000;
    std::chrono::steady_clock::time_point mc_last_poll_ = std::chrono::steady_clock::now();
    int mc_cached_online_ = -1;
    int mc_cached_max_ = -1;

    struct MetricsSnapshot {
        double cpu_usage = 0;
        double mem_percent = 0;
        int mem_used_mb = 0;
        double temp = 0;
        double net1_mbps = 0;
        double net2_mbps = 0;
        int docker_running = -1;
        int disk_percent = -1;
        int wg_active_peers = -1;
        int uptime_seconds = 0;
        int mc_online = -1;
        int mc_max = -1;
    };
    MetricsSnapshot pending_snapshot_;
    std::mutex snapshot_mutex_;
};

#endif // SYSTEM_METRICS_H
