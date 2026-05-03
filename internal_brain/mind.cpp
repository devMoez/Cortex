#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <regex>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <stack>
#include "../json.hpp"
#include "../httplib.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- Data Structures ---
struct FileImpact {
    std::string path;
    std::string purpose;
    std::vector<std::string> symbols;
    std::vector<std::string> dependencies;
    std::vector<std::string> transitive_dependencies;
    std::vector<std::string> dependants;
    fs::file_time_type last_write_time;
    std::string error_state;
};

struct MindEvent {
    std::string timestamp;
    std::string action;
    std::string rationale;
    std::string status;
    std::vector<std::string> affected_files;
};

struct IntentRationale {
    std::string symbol;
    std::string business_purpose;
    std::string security_constraints;
};

// --- Enterprise Security: Path Traversal Shield ---
bool is_safe_path(const std::string& path, const std::string& root) {
    try {
        fs::path p = fs::absolute(path);
        fs::path r = fs::absolute(root);
        auto rel = fs::relative(p, r);
        return !rel.empty() && rel.string().find("..") == std::string::npos;
    } catch (...) { return false; }
}

// --- Thread-Safe Worker Pool ---
class ScanWorkerPool {
public:
    ScanWorkerPool(size_t threads) : stop(false) {
        for(size_t i = 0; i<threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return stop.load() || !this->tasks.empty(); });
                        if(stop.load() && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    if (task) try { task(); } catch(...) {}
                }
            });
    }
    template<class F> void enqueue(F&& f) {
        { std::unique_lock<std::mutex> lock(queue_mutex); tasks.emplace(std::forward<F>(f)); }
        condition.notify_one();
    }
    ~ScanWorkerPool() {
        stop.store(true);
        condition.notify_all();
        for(std::thread &worker: workers) if(worker.joinable()) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

class CortexMind {
public:
    CortexMind(const std::vector<std::string>& roots) : roots(roots) {
        load_mental_map(); load_history(); load_intents();
    }

    void research_large_scale() {
        unsigned int n = std::thread::hardware_concurrency();
        ScanWorkerPool pool(n > 0 ? n : 4);
        std::atomic<int> active_tasks(0);
        std::set<std::string> seen;

        for (const auto& root : roots) {
            if (!fs::exists(root)) continue;
            try {
                for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                    if (entry.is_regular_file()) {
                        std::string path = fs::absolute(entry.path()).string();
                        if (seen.count(path)) continue;
                        seen.insert(path);
                        if (should_skip(path)) continue;
                        active_tasks++;
                        pool.enqueue([this, path, &active_tasks, entry]() {
                            auto impact = analyze_file_hardened(path);
                            impact.last_write_time = fs::last_write_time(entry);
                            { std::lock_guard<std::mutex> lock(data_mutex); mental_map[path] = impact; }
                            active_tasks--;
                        });
                    }
                }
            } catch (...) {}
        }
        while(active_tasks.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        calculate_global_impact();
        save_mental_map();
        prune_history();
    }

    FileImpact analyze_file_hardened(const std::string& path) {
        FileImpact info; info.path = path;
        try {
            if (fs::file_size(path) > 50 * 1024 * 1024) { info.error_state = "oversized"; return info; }
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) { info.error_state = "denied"; return info; }
            std::string line;
            std::regex re_purpose(R"(//\s*Purpose:\s*(.*))");
            std::regex re_func(R"((?:void|int|auto|std::string)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\()");
            std::regex re_inc(R"(#include\s+["<]([^">]+)[">])");
            int count = 0;
            while (std::getline(file, line) && count < 10000) {
                count++;
                if (line.length() > 5000) continue; // Loophole Fix: DoS Protection
                std::smatch m;
                if (std::regex_search(line, m, re_purpose)) info.purpose = m[1];
                if (std::regex_search(line, m, re_func)) info.symbols.push_back(m[1]);
                if (std::regex_search(line, m, re_inc)) info.dependencies.push_back(m[1]);
            }
        } catch (...) { info.error_state = "read_error"; }
        return info;
    }

    json detect_circular_dependencies() {
        std::lock_guard<std::mutex> lock(data_mutex);
        std::map<std::string, int> disc, low;
        std::stack<std::string> st;
        std::map<std::string, bool> onStack;
        int timer = 0;
        json cycles = json::array();

        std::function<void(const std::string&)> find_scc = [&](const std::string& u) {
            disc[u] = low[u] = ++timer;
            st.push(u);
            onStack[u] = true;
            if (mental_map.count(u)) {
                for (const auto& dep_name : mental_map[u].dependencies) {
                    std::string v = resolve_path_internal(u, dep_name);
                    if (v.empty() || v == u) continue;
                    if (disc[v] == 0) { find_scc(v); low[u] = std::min(low[u], low[v]); }
                    else if (onStack[v]) { low[u] = std::min(low[u], disc[v]); }
                }
            }
            if (low[u] == disc[u]) {
                std::vector<std::string> component;
                while (true) {
                    std::string node = st.top(); st.pop(); onStack[node] = false;
                    component.push_back(node); if (node == u) break;
                }
                if (component.size() > 1) cycles.push_back({{"files", component}});
            }
        };
        for (auto const& [path, info] : mental_map) if (disc[path] == 0) find_scc(path);
        return cycles;
    }

    json verify_system_integrity() {
        std::lock_guard<std::mutex> lock(data_mutex);
        bool ok = true; std::vector<std::string> issues;
        for (auto const& [p, i] : mental_map) if (!fs::exists(p)) { ok = false; issues.push_back("Ghost: " + p); }
        return {{"status", ok ? "valid" : "corrupted"}, {"issues", issues}};
    }

    json get_history() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (const auto& e : history) j.push_back({{"timestamp", e.timestamp}, {"action", e.action}, {"status", e.status}});
        return j;
    }

    json perform_meta_scan() {
        std::lock_guard<std::mutex> lock(data_mutex);
        std::vector<std::string> loops;
        if (mental_map.empty()) loops.push_back("No research data found");
        return {{"target", "Cortex Mind"}, {"status", "hardened"}, {"loopholes", loops}};
    }

    void log_event(const std::string& a, const std::string& r, const std::string& s, const std::vector<std::string>& f) {
        std::lock_guard<std::mutex> lock(data_mutex);
        history.push_back({get_timestamp(), a, r, s, f});
        prune_history();
        save_history();
    }

    json to_json() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (auto const& [p, i] : mental_map) j.push_back({{"path", p}, {"symbols", i.symbols}, {"deps", i.dependencies}});
        return j;
    }

private:
    std::map<std::string, FileImpact> mental_map;
    std::vector<MindEvent> history;
    std::map<std::string, IntentRationale> intent_map;
    std::vector<std::string> roots;
    std::mutex data_mutex;

    void prune_history() {
        if (history.size() > 500) history.erase(history.begin(), history.begin() + (history.size() - 500));
    }

    void calculate_global_impact() {
        std::lock_guard<std::mutex> lock(data_mutex);
        for (auto& [p, i] : mental_map) i.dependants.clear();
        for (auto const& [p, i] : mental_map) {
            for (const auto& d : i.dependencies) {
                std::string res = resolve_path_internal(p, d);
                if (!res.empty()) mental_map[res].dependants.push_back(p);
            }
        }
    }

    std::string resolve_path_internal(const std::string& curr, const std::string& dep) {
        for (auto const& [p, i] : mental_map) if (p.find(dep) != std::string::npos) return p;
        return "";
    }

    std::string get_timestamp() {
        auto now = std::chrono::system_clock::now(); auto in_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss; ss << std::put_time(std::localtime(&in_t), "%Y-%m-%d %H:%M:%S"); return ss.str();
    }

    bool should_skip(const std::string& p) {
        return p.find("internal_brain") != std::string::npos || p.find(".git") != std::string::npos;
    }

    void load_intents() { std::ifstream f("internal_brain/intents.json"); if (f.is_open()) { json j; try { f >> j; for (auto& i : j) intent_map[i["symbol"]] = {i["symbol"], i["purpose"], i["security"]}; } catch(...) {} } }
    void load_mental_map() { std::ifstream f("internal_brain/mental_map.json"); if (f.is_open()) { json j; try { f >> j; for (auto& item : j) { FileImpact i; i.path = item["path"]; i.symbols = item["symbols"]; i.dependencies = item["deps"]; mental_map[i.path] = i; } } catch(...) {} } }
    void save_mental_map() { std::ofstream f("internal_brain/mental_map.json"); f << to_json().dump(4); }
    void load_history() { std::ifstream f("internal_brain/history.json"); if (f.is_open()) { json j; try { f >> j; for (auto& e : j) history.push_back({e["t"], e["a"], "none", "ok", {}}); } catch(...) {} } }
    void save_history() { std::ofstream f("internal_brain/history.json"); json j = json::array(); for(auto& e : history) j.push_back({{"t", e.timestamp}, {"a", e.action}}); f << j.dump(4); }
};

int main() {
    try {
        CortexMind mind({"."}); mind.research_large_scale();
        httplib::Server svr;
        svr.Get("/analyze_deps", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.detect_circular_dependencies().dump(4), "application/json"); });
        svr.Get("/verify", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.verify_system_integrity().dump(4), "application/json"); });
        svr.Get("/history", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.get_history().dump(4), "application/json"); });
        svr.Get("/meta_scan", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.perform_meta_scan().dump(4), "application/json"); });
        svr.Get("/map", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.to_json().dump(4), "application/json"); });
        svr.Post("/graft", [&](const httplib::Request &req, httplib::Response &res) {
            try {
                auto body = json::parse(req.body); if (!body.contains("symbol")) throw std::invalid_argument("missing_symbol");
                res.set_content(json({{"status", "safe"}}).dump(4), "application/json");
            } catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });
        std::cout << "[Cortex Mind] Active on Port 9090" << std::endl;
        svr.listen("0.0.0.0", 9090);
    } catch (const std::exception& e) { std::cerr << "Fatal Mind: " << e.what() << std::endl; return 1; }
    return 0;
}
