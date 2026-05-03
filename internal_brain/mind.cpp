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
#include "../json.hpp"
#include "../httplib.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- Enterprise Security & Safety ---
bool is_safe_path(const std::string& path, const std::string& root) {
    try {
        fs::path p = fs::absolute(path);
        fs::path r = fs::absolute(root);
        auto rel = fs::relative(p, r);
        return !rel.empty() && rel.string().find("..") == std::string::npos;
    } catch (...) { return false; }
}

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
                    try { if (task) task(); } catch (...) {}
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
        load_mental_map(); load_history(); load_session(); load_intents();
    }

    void research_large_scale() {
        unsigned int n = std::thread::hardware_concurrency();
        ScanWorkerPool pool(n > 0 ? n : 4);
        std::atomic<int> active_tasks(0);
        std::set<std::string> seen_paths;

        for (const auto& root : roots) {
            if (!fs::exists(root)) continue;
            try {
                for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                    if (entry.is_regular_file()) {
                        std::string path = fs::absolute(entry.path()).string();
                        if (seen_paths.count(path)) continue;
                        seen_paths.insert(path);
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
    }

    FileImpact analyze_file_hardened(const std::string& path) {
        FileImpact info; info.path = path;
        try {
            if (fs::file_size(path) > 50 * 1024 * 1024) { info.error_state = "oversized"; return info; }
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) { info.error_state = "denied"; return info; }
            std::string line;
            std::regex re_purpose(R"(//\s*Purpose:\s*(.*))");
            std::regex re_func(R"((?:void|int|auto|std::string|char|float|double)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\()");
            std::regex re_include(R"(#include\s+["<]([^">]+)[">])");
            int count = 0;
            while (std::getline(file, line) && count < 10000) {
                count++;
                if (line.length() > 5000) continue;
                std::smatch match;
                if (std::regex_search(line, match, re_purpose)) info.purpose = match[1];
                if (std::regex_search(line, match, re_func)) info.symbols.push_back(match[1]);
                if (std::regex_search(line, match, re_include)) info.dependencies.push_back(match[1]);
            }
        } catch (...) { info.error_state = "exception"; }
        return info;
    }

    json calculate_blast_radius(const std::string& symbol) {
        json radius = json::array();
        std::lock_guard<std::mutex> lock(data_mutex);
        for (auto const& [path, info] : mental_map) {
            for (const auto& s : info.symbols) if (s == symbol) radius.push_back({{"type", "definition"}, {"file", path}});
            for (const auto& dep : info.dependencies) if (dep.find(symbol) != std::string::npos) radius.push_back({{"type", "usage_leak"}, {"file", path}});
        }
        return radius;
    }

    json to_json() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (auto const& [path, info] : mental_map) {
            j.push_back({{"path", path}, {"purpose", info.purpose}, {"symbols", info.symbols}, {"dependencies", info.dependencies}, {"transitive_dependencies", info.transitive_dependencies}, {"dependants", info.dependants}, {"error", info.error_state}});
        }
        return j;
    }

    void log_event(const std::string& a, const std::string& r, const std::string& s, const std::vector<std::string>& f) {
        std::lock_guard<std::mutex> lock(data_mutex);
        history.push_back({get_timestamp(), a, r, s, f});
        save_history();
    }

    json get_history() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (const auto& e : history) j.push_back({{"timestamp", e.timestamp}, {"action", e.action}, {"rationale", e.rationale}, {"status", e.status}, {"affected_files", e.affected_files}});
        return j;
    }

    void set_current_goal(const std::string& g) { std::lock_guard<std::mutex> lock(data_mutex); current_session["goal"] = g; save_session(); }
    json get_session() { std::lock_guard<std::mutex> lock(data_mutex); return current_session; }

private:
    std::vector<std::string> roots;
    std::map<std::string, FileImpact> mental_map;
    std::vector<MindEvent> history;
    std::map<std::string, IntentRationale> intent_map;
    json current_session;
    std::mutex data_mutex;

    void calculate_global_impact() {
        std::lock_guard<std::mutex> lock(data_mutex);
        for (auto& [path, info] : mental_map) info.dependants.clear();
        for (auto& [path, info] : mental_map) {
            std::set<std::string> transitive;
            std::vector<std::string> stack = info.dependencies;
            while (!stack.empty()) {
                std::string dep = stack.back(); stack.pop_back();
                std::string res = "";
                for (auto const& [p, i] : mental_map) if (p.find(dep) != std::string::npos) { res = p; break; }
                if (!res.empty() && transitive.find(res) == transitive.end()) {
                    transitive.insert(res);
                    mental_map[res].dependants.push_back(path);
                    for (const auto& next : mental_map[res].dependencies) stack.push_back(next);
                }
            }
            info.transitive_dependencies.assign(transitive.begin(), transitive.end());
        }
    }

    std::string get_timestamp() {
        auto now = std::chrono::system_clock::now(); auto in_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss; ss << std::put_time(std::localtime(&in_t), "%Y-%m-%d %H:%M:%S"); return ss.str();
    }

    bool should_skip(const std::string& p) {
        static const std::set<std::string> skip = {".exe", ".obj", ".git", "internal_brain", "node_modules"};
        for (auto const& s : skip) if (p.find(s) != std::string::npos) return true;
        return false;
    }

    void load_intents() { std::ifstream f("internal_brain/intents.json"); if (f.is_open()) { json j; f >> j; for (auto& i : j) intent_map[i["symbol"]] = {i["symbol"], i["purpose"], i["security"]}; } }
    void save_intents() { json j = json::array(); for (auto const& [k, i] : intent_map) j.push_back({{"symbol", i.symbol}, {"purpose", i.business_purpose}, {"security", i.security_constraints}}); std::ofstream f("internal_brain/intents.json"); f << j.dump(4); }
    void load_mental_map() { std::ifstream f("internal_brain/mental_map.json"); if (f.is_open()) { json j; f >> j; for (auto& item : j) { FileImpact i; i.path = item["path"]; i.symbols = item["symbols"].get<std::vector<std::string>>(); i.dependencies = item["dependencies"].get<std::vector<std::string>>(); if (fs::exists(i.path)) i.last_write_time = fs::last_write_time(i.path); mental_map[i.path] = i; } } }
    void save_mental_map() { std::ofstream f("internal_brain/mental_map.json"); f << to_json().dump(4); }
    void load_history() { std::ifstream f("internal_brain/history.json"); if (f.is_open()) { json j; f >> j; for (auto& e : j) history.push_back({e["timestamp"], e["action"], e["rationale"], e["status"], e["affected_files"].get<std::vector<std::string>>()}); } }
    void save_history() { std::ofstream f("internal_brain/history.json"); f << get_history().dump(4); }
    void load_session() { std::ifstream f("internal_brain/session.json"); if (f.is_open()) f >> current_session; else current_session = {{"goal", "None"}}; }
    void save_session() { std::ofstream f("internal_brain/session.json"); f << current_session.dump(4); }
};

int main() {
    try {
        CortexMind mind({"."}); mind.research_large_scale();
        httplib::Server svr;
        svr.Get("/blast_radius", [&](const httplib::Request &req, httplib::Response &res) {
            res.set_content(mind.calculate_blast_radius(req.get_param_value("symbol")).dump(4), "application/json");
        });
        svr.Post("/graft", [&](const httplib::Request &req, httplib::Response &res) {
            try {
                auto body = json::parse(req.body); std::string sym = body["symbol"];
                json resp; bool coll = false; std::vector<std::string> aff;
                auto map = mind.to_json();
                for (auto& f : map) for (auto& s : f["symbols"]) if (s == sym) { coll = true; aff.push_back(f["path"]); }
                if (coll) { mind.log_event("GRAFT_FAILED", "Collision", "high_risk", aff); resp = {{"status", "high_risk"}, {"affected_files", aff}}; }
                else { mind.log_event("GRAFT_SIM", "Safe", "safe", {"main.cpp"}); resp = {{"status", "safe"}, {"confidence", 0.95}}; }
                res.set_content(resp.dump(4), "application/json");
            } catch (...) { res.status = 400; }
        });
        svr.Get("/map", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.to_json().dump(4), "application/json"); });
        svr.Get("/history", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mind.get_history().dump(4), "application/json"); });
        std::cout << "[Cortex Mind] Port 9090" << std::endl;
        svr.listen("0.0.0.0", 9090);
    } catch (const std::exception& e) { std::cerr << "[Fatal] " << e.what() << std::endl; return 1; }
    return 0;
}
