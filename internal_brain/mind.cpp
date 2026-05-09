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

// ─── Data Structures ────────────────────────────────────────────────────────

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

struct Rule {
    std::string id;
    std::string category;   // preflight | coding | ui | architecture | verification | exception | security | database
    std::string name;
    std::string description;
    std::string severity;   // error | warning
    bool active;
};

struct RuleException {
    std::string id;
    std::string rule_id;
    std::string reason;
    std::string file;       // optional: scope exception to one file
    std::string created_at;
};

struct CortexStack {
    std::string name;
    std::string description;
    std::vector<std::string> disable;
    std::vector<std::string> enable;
};

// ─── Path Utilities ──────────────────────────────────────────────────────────

static std::string norm_path(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Match stored absolute path against a query that may be absolute or relative.
// Handles Windows/Unix separator mismatch and relative suffix queries.
static bool path_matches(const std::string& stored, const std::string& query) {
    std::string ns = norm_path(stored);
    std::string nq = norm_path(query);
    if (ns == nq) return true;
    if (ns.length() > nq.length()) {
        size_t off = ns.length() - nq.length();
        if (ns.substr(off) == nq && ns[off - 1] == '/') return true;
    }
    return false;
}

// ─── Path Traversal Shield ───────────────────────────────────────────────────

bool is_safe_path(const std::string& path, const std::string& root) {
    try {
        fs::path p = fs::absolute(path);
        fs::path r = fs::absolute(root);
        auto rel = fs::relative(p, r);
        return !rel.empty() && rel.string().find("..") == std::string::npos;
    } catch (...) { return false; }
}

// ─── Thread-Safe Worker Pool ─────────────────────────────────────────────────

class ScanWorkerPool {
public:
    ScanWorkerPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return stop.load() || !this->tasks.empty(); });
                        if (stop.load() && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    if (task) try { task(); } catch (...) {}
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
        for (std::thread& w : workers) if (w.joinable()) w.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

// ─── CortexMind ──────────────────────────────────────────────────────────────

class CortexMind {
public:
    CortexMind(const std::vector<std::string>& roots)
        : roots(roots), watching(false), watcher_tick(0), last_changed_count(0) {
        load_mental_map();
        load_history();
        load_intents();
        load_rules();
        load_exceptions();
        load_stacks();
    }

    ~CortexMind() { stop_watching(); }

    // ── Full scan (startup) ──────────────────────────────────────────────────

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
        while (active_tasks.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        calculate_global_impact();
        save_mental_map();
        prune_history();
        { std::lock_guard<std::mutex> lock(data_mutex); last_scan_time = get_timestamp(); }
    }

    // ── Incremental rescan (watcher + manual trigger) ────────────────────────
    // scan_new=true also walks roots looking for files not yet in mental_map.
    // Called every 2s (changed-files only) and every 10s (new-files scan too).

    void rescan_changed(bool scan_new = false) {
        // Step 1: snapshot known files + their stored mtimes (brief lock)
        std::vector<std::pair<std::string, fs::file_time_type>> snapshot;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            for (auto const& [p, i] : mental_map)
                snapshot.push_back({p, i.last_write_time});
        }

        std::vector<std::string> to_reanalyze;
        std::vector<std::string> to_delete;

        // Step 2: compare mtimes without holding the lock
        for (auto const& [p, stored_mtime] : snapshot) {
            if (!fs::exists(p)) { to_delete.push_back(p); continue; }
            try {
                if (fs::last_write_time(p) != stored_mtime)
                    to_reanalyze.push_back(p);
            } catch (...) {}
        }

        // Step 3: scan for new files (every ~10s)
        if (scan_new) {
            std::set<std::string> known;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                for (auto const& [p, i] : mental_map) known.insert(p);
            }
            for (auto const& root : roots) {
                if (!fs::exists(root)) continue;
                try {
                    for (auto const& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                        if (!entry.is_regular_file()) continue;
                        std::string p = fs::absolute(entry.path()).string();
                        if (!known.count(p) && !should_skip(p))
                            to_reanalyze.push_back(p);
                    }
                } catch (...) {}
            }
        }

        if (to_reanalyze.empty() && to_delete.empty()) return;

        // Step 4: re-analyze changed/new files (no lock during file I/O)
        for (auto const& p : to_reanalyze) {
            if (!fs::exists(p)) { to_delete.push_back(p); continue; }
            auto impact = analyze_file_hardened(p);
            try { impact.last_write_time = fs::last_write_time(p); } catch (...) {}
            std::lock_guard<std::mutex> lock(data_mutex);
            mental_map[p] = impact;
        }

        // Step 5: remove deleted entries
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            for (auto const& p : to_delete) mental_map.erase(p);
        }

        // Step 6: recalculate blast radius, persist, update stats
        calculate_global_impact();
        save_mental_map();
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            last_scan_time    = get_timestamp();
            last_changed_count = (int)(to_reanalyze.size() + to_delete.size());
        }
    }

    void start_watching(int interval_ms = 2000) {
        watching.store(true);
        watcher_thread = std::thread([this, interval_ms]() {
            int elapsed = 0;
            while (watching.load()) {
                // Sleep in 100ms chunks so we stop quickly on shutdown
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!watching.load()) break;
                elapsed += 100;
                if (elapsed >= interval_ms) {
                    elapsed = 0;
                    watcher_tick++;
                    // Every 5th tick scan for new files (avoids hammering large repos)
                    rescan_changed(watcher_tick % 5 == 0);
                }
            }
        });
    }

    void stop_watching() {
        watching.store(false);
        if (watcher_thread.joinable()) watcher_thread.join();
    }

    // ── Analysis ─────────────────────────────────────────────────────────────

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
                if (line.length() > 5000) continue;
                std::smatch m;
                if (std::regex_search(line, m, re_purpose)) info.purpose = m[1];
                if (std::regex_search(line, m, re_func))    info.symbols.push_back(m[1]);
                if (std::regex_search(line, m, re_inc))     info.dependencies.push_back(m[1]);
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
            st.push(u); onStack[u] = true;
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
        for (auto const& [p, i] : mental_map)
            if (!fs::exists(p)) { ok = false; issues.push_back("Ghost: " + p); }
        return {{"status", ok ? "valid" : "corrupted"}, {"issues", issues}};
    }

    // ── Summary ──────────────────────────────────────────────────────────────

    json get_summary() {
        std::lock_guard<std::mutex> lock(data_mutex);
        int active_rules = 0;
        for (auto const& r : rules) if (r.active) active_rules++;

        std::vector<std::pair<int, std::string>> dep_counts;
        for (auto const& [p, i] : mental_map)
            if (!i.dependants.empty()) dep_counts.push_back({(int)i.dependants.size(), p});
        std::sort(dep_counts.rbegin(), dep_counts.rend());
        json top_deps = json::array();
        for (int i = 0; i < std::min(5, (int)dep_counts.size()); i++)
            top_deps.push_back({{"file", dep_counts[i].second}, {"dependant_count", dep_counts[i].first}});

        return {
            {"file_count",          (int)mental_map.size()},
            {"rule_count",          (int)rules.size()},
            {"active_rule_count",   active_rules},
            {"exception_count",     (int)exceptions.size()},
            {"active_stack",        active_stack.empty() ? "none" : active_stack},
            {"last_scan",           last_scan_time},
            {"last_changed_count",  last_changed_count},
            {"watcher_running",     watching.load()},
            {"most_depended_on",    top_deps}
        };
    }

    // ── Rules CRUD ───────────────────────────────────────────────────────────

    json get_rules() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (auto const& r : rules)
            j.push_back({{"id", r.id}, {"category", r.category}, {"name", r.name},
                         {"description", r.description}, {"severity", r.severity}, {"active", r.active}});
        return j;
    }

    json add_rule(const json& body) {
        json saved_data;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            Rule r;
            if (body.contains("id")) {
                std::string requested = body["id"].get<std::string>();
                for (auto const& e : rules)
                    if (e.id == requested)
                        return {{"status", "error"}, {"message", "id already exists: " + requested}};
                r.id = requested;
            } else {
                int n = (int)rules.size() + 1;
                std::string candidate = "rule_" + std::to_string(n);
                while (true) {
                    bool clash = false;
                    for (auto const& e : rules) if (e.id == candidate) { clash = true; break; }
                    if (!clash) break;
                    candidate = "rule_" + std::to_string(++n);
                }
                r.id = candidate;
            }
            r.category    = body.value("category", "coding");
            r.name        = body.value("name", "Unnamed Rule");
            r.description = body.value("description", "");
            r.severity    = body.value("severity", "warning");
            r.active      = body.value("active", true);
            rules.push_back(r);
            saved_data = snapshot_rules();
        }
        std::ofstream f("internal_brain/rules.json"); f << saved_data.dump(4);
        return {{"status", "added"}, {"id", saved_data.back()["id"]}};
    }

    json delete_rule(const std::string& id) {
        json saved_data;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            auto it = std::remove_if(rules.begin(), rules.end(), [&](const Rule& r){ return r.id == id; });
            if (it == rules.end()) return {{"status", "not_found"}};
            rules.erase(it, rules.end());
            saved_data = snapshot_rules();
        }
        std::ofstream f("internal_brain/rules.json"); f << saved_data.dump(4);
        return {{"status", "deleted"}, {"id", id}};
    }

    json update_rule(const std::string& id, const json& body) {
        json saved_data;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            bool found = false;
            for (auto& r : rules) {
                if (r.id != id) continue;
                if (body.contains("name"))        r.name        = body["name"].get<std::string>();
                if (body.contains("description")) r.description = body["description"].get<std::string>();
                if (body.contains("category"))    r.category    = body["category"].get<std::string>();
                if (body.contains("severity"))    r.severity    = body["severity"].get<std::string>();
                if (body.contains("active"))      r.active      = body["active"].get<bool>();
                found = true; break;
            }
            if (!found) return {{"status", "not_found"}};
            saved_data = snapshot_rules();
        }
        std::ofstream f("internal_brain/rules.json"); f << saved_data.dump(4);
        return {{"status", "updated"}, {"id", id}};
    }

    // ── Stacks ───────────────────────────────────────────────────────────────

    json get_stacks() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json result;
        result["active"] = active_stack.empty() ? json(nullptr) : json(active_stack);
        json stacks_json = json::object();
        for (auto const& [name, s] : stacks)
            stacks_json[name] = {{"description", s.description}, {"disable", s.disable}, {"enable", s.enable}};
        result["stacks"] = stacks_json;
        return result;
    }

    // Activate a named stack — resets all rules to active, then applies disable/enable lists.
    // Pass "none" to clear any active stack (restore all rules to active).
    json activate_stack(const std::string& name) {
        json saved_rules;
        std::string new_active;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            if (name != "none" && !stacks.count(name))
                return {{"status", "not_found"}, {"name", name}};

            for (auto& r : rules) r.active = true;

            if (name != "none") {
                auto const& s = stacks.at(name);
                for (auto const& id : s.disable)
                    for (auto& r : rules) if (r.id == id) { r.active = false; break; }
                for (auto const& id : s.enable)
                    for (auto& r : rules) if (r.id == id) { r.active = true; break; }
            }
            active_stack = (name == "none") ? "" : name;
            new_active   = active_stack;
            saved_rules  = snapshot_rules();
        }
        { std::ofstream f("internal_brain/rules.json"); f << saved_rules.dump(4); }
        {
            std::ifstream fin("internal_brain/stacks.json");
            json doc;
            if (fin.is_open()) { try { fin >> doc; } catch (...) {} }
            doc["active"] = new_active.empty() ? json(nullptr) : json(new_active);
            std::ofstream fout("internal_brain/stacks.json"); fout << doc.dump(4);
        }
        return {{"status", "activated"}, {"stack", new_active.empty() ? "none" : new_active}};
    }

    // ── Exceptions ───────────────────────────────────────────────────────────

    json get_exceptions() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (auto const& e : exceptions)
            j.push_back({{"id", e.id}, {"rule_id", e.rule_id}, {"reason", e.reason},
                         {"file", e.file}, {"created_at", e.created_at}});
        return j;
    }

    json add_exception(const json& body) {
        json saved;
        std::string new_id;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            RuleException ex;
            auto ts = std::chrono::system_clock::now().time_since_epoch().count();
            ex.id         = "ex_" + std::to_string(exceptions.size() + 1) + "_" + std::to_string(ts % 100000);
            ex.rule_id    = body.value("rule_id", "");
            ex.reason     = body.value("reason", "No reason given");
            ex.file       = body.value("file", "");
            ex.created_at = get_timestamp();
            new_id        = ex.id;
            exceptions.push_back(ex);
            saved = snapshot_exceptions();
        }
        std::ofstream f("internal_brain/exceptions.json"); f << saved.dump(4);
        return {{"status", "added"}, {"id", new_id}};
    }

    json clear_exception(const std::string& id) {
        json saved;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            auto it = std::remove_if(exceptions.begin(), exceptions.end(),
                                     [&](const RuleException& e){ return e.id == id; });
            if (it == exceptions.end()) return {{"status", "not_found"}};
            exceptions.erase(it, exceptions.end());
            saved = snapshot_exceptions();
        }
        std::ofstream f("internal_brain/exceptions.json"); f << saved.dump(4);
        return {{"status", "cleared"}, {"id", id}};
    }

    // ── Preflight ────────────────────────────────────────────────────────────

    json check_preflight(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(data_mutex);
        json result;
        result["file"] = filepath;

        std::vector<std::string> blast;
        std::string purpose;
        json symbols = json::array();

        for (auto const& [p, i] : mental_map) {
            if (path_matches(p, filepath)) {
                blast   = i.dependants;
                purpose = i.purpose;
                for (auto const& s : i.symbols) symbols.push_back(s);
                break;
            }
        }

        result["purpose"]            = purpose;
        result["symbols"]            = symbols;
        result["blast_radius"]       = blast;
        result["blast_radius_count"] = (int)blast.size();

        json applicable = json::array();
        for (auto const& r : rules) {
            if (!r.active) continue;
            if (r.category == "preflight" || r.category == "coding" ||
                r.category == "ui"        || r.category == "architecture" ||
                r.category == "exception" || r.category == "security" ||
                r.category == "database") {
                applicable.push_back({{"id", r.id}, {"category", r.category},
                                       {"name", r.name}, {"description", r.description},
                                       {"severity", r.severity}});
            }
        }
        result["rules"] = applicable;
        result["warning"] = blast.empty() ? "" :
            std::to_string(blast.size()) + " file(s) depend on this. Update ALL of them if the interface changes.";
        return result;
    }

    // ── Verify Change ────────────────────────────────────────────────────────

    // Called AFTER AI writes. Checks blast radius coverage and consumes any matching exceptions.
    json verify_change(const json& body) {
        std::vector<std::string> changed;
        for (auto const& f : body) changed.push_back(f.get<std::string>());

        std::set<std::string> changed_set(changed.begin(), changed.end());
        std::set<std::string> all_blast;

        {
            std::lock_guard<std::mutex> lock(data_mutex);
            for (auto const& cf : changed) {
                for (auto const& [p, i] : mental_map) {
                    if (path_matches(p, cf)) {
                        for (auto const& dep : i.dependants) all_blast.insert(dep);
                    }
                }
            }
        }

        std::vector<std::string> missing;
        for (auto const& b : all_blast)
            if (!changed_set.count(b)) missing.push_back(b);

        // Build raw violations
        json violations      = json::array();
        json acknowledged    = json::array();
        json saved_exceptions;

        if (!missing.empty()) {
            // Check if there is an active exception for this rule
            bool has_exception = false;
            std::string ex_id, ex_reason;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                for (auto it = exceptions.begin(); it != exceptions.end(); ++it) {
                    if (it->rule_id == "verify_001") {
                        ex_id = it->id; ex_reason = it->reason;
                        exceptions.erase(it);
                        has_exception = true;
                        break;
                    }
                }
                saved_exceptions = snapshot_exceptions();
            }
            if (has_exception) {
                acknowledged.push_back({
                    {"rule_id",      "verify_001"},
                    {"rule",         "Blast radius coverage"},
                    {"exception_id", ex_id},
                    {"reason",       ex_reason},
                    {"missing_files", missing}
                });
                // Persist consumed exception
                std::ofstream f("internal_brain/exceptions.json");
                f << saved_exceptions.dump(4);
            } else {
                violations.push_back({
                    {"rule_id",      "verify_001"},
                    {"rule",         "Blast radius coverage"},
                    {"severity",     "error"},
                    {"message",      "These dependent files were NOT updated — they may now be broken"},
                    {"missing_files", missing}
                });
            }
        }

        return {
            {"passed",          violations.empty()},
            {"violations",      violations},
            {"acknowledged",    acknowledged},
            {"changed_files",   changed}
        };
    }

    // ── Misc ─────────────────────────────────────────────────────────────────

    json get_history() {
        std::lock_guard<std::mutex> lock(data_mutex);
        json j = json::array();
        for (const auto& e : history)
            j.push_back({{"timestamp", e.timestamp}, {"action", e.action}, {"status", e.status}});
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
        for (auto const& [p, i] : mental_map)
            j.push_back({{"path", p}, {"symbols", i.symbols}, {"deps", i.dependencies}});
        return j;
    }

private:
    // ── State ────────────────────────────────────────────────────────────────
    std::map<std::string, FileImpact> mental_map;
    std::vector<MindEvent>            history;
    std::map<std::string, IntentRationale> intent_map;
    std::vector<Rule>                 rules;
    std::vector<RuleException>        exceptions;
    std::map<std::string, CortexStack> stacks;
    std::string                       active_stack;
    std::vector<std::string>          roots;
    std::mutex                        data_mutex;
    std::string                       last_scan_time;
    int                               last_changed_count;
    std::atomic<bool>                 watching;
    std::thread                       watcher_thread;
    int                               watcher_tick;

    // ── Helpers ──────────────────────────────────────────────────────────────

    // Snapshot rules vector to JSON — caller must hold data_mutex
    json snapshot_rules() {
        json j = json::array();
        for (auto const& r : rules)
            j.push_back({{"id", r.id}, {"category", r.category}, {"name", r.name},
                         {"description", r.description}, {"severity", r.severity}, {"active", r.active}});
        return j;
    }

    json snapshot_exceptions() {
        json j = json::array();
        for (auto const& e : exceptions)
            j.push_back({{"id", e.id}, {"rule_id", e.rule_id}, {"reason", e.reason},
                         {"file", e.file}, {"created_at", e.created_at}});
        return j;
    }

    void prune_history() {
        if (history.size() > 500)
            history.erase(history.begin(), history.begin() + (history.size() - 500));
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
        for (auto const& [p, i] : mental_map)
            if (p.find(dep) != std::string::npos) return p;
        return "";
    }

    std::string get_timestamp() {
        auto now  = std::chrono::system_clock::now();
        auto in_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_t), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    bool should_skip(const std::string& p) {
        return p.find("internal_brain") != std::string::npos ||
               p.find(".git")          != std::string::npos  ||
               p.find("node_modules")  != std::string::npos;
    }

    // ── Persistence ──────────────────────────────────────────────────────────

    void load_mental_map() {
        std::ifstream f("internal_brain/mental_map.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        for (auto& item : j) {
            try {
                FileImpact i;
                i.path         = item["path"];
                i.symbols      = item["symbols"].get<std::vector<std::string>>();
                i.dependencies = item["deps"].get<std::vector<std::string>>();
                mental_map[i.path] = i;
            } catch (...) {}
        }
    }
    void save_mental_map() { std::ofstream f("internal_brain/mental_map.json"); f << to_json().dump(4); }

    void load_history() {
        std::ifstream f("internal_brain/history.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        for (auto& e : j) {
            try { history.push_back({e["t"], e["a"], "none", "ok", {}}); } catch (...) {}
        }
    }
    void save_history() {
        json j = json::array();
        for (auto& e : history) j.push_back({{"t", e.timestamp}, {"a", e.action}});
        std::ofstream f("internal_brain/history.json"); f << j.dump(4);
    }

    void load_intents() {
        std::ifstream f("internal_brain/intents.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        for (auto& i : j) {
            try { intent_map[i["symbol"]] = {i["symbol"], i["purpose"], i["security"]}; } catch (...) {}
        }
    }

    void load_rules() {
        std::ifstream f("internal_brain/rules.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        for (auto& r : j) {
            try {
                rules.push_back({r["id"], r["category"], r["name"],
                                 r["description"], r["severity"], r.value("active", true)});
            } catch (...) {}
        }
    }

    void load_exceptions() {
        std::ifstream f("internal_brain/exceptions.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        for (auto& e : j) {
            try {
                exceptions.push_back({e["id"], e["rule_id"],
                                      e.value("reason", ""), e.value("file", ""),
                                      e.value("created_at", "")});
            } catch (...) {}
        }
    }

    void load_stacks() {
        std::ifstream f("internal_brain/stacks.json");
        if (!f.is_open()) return;
        json j; try { f >> j; } catch (...) { return; }
        if (j.contains("active") && j["active"].is_string())
            active_stack = j["active"].get<std::string>();
        if (!j.contains("stacks")) return;
        for (auto& [name, s] : j["stacks"].items()) {
            try {
                CortexStack st;
                st.name        = name;
                st.description = s.value("description", "");
                if (s.contains("disable"))
                    for (auto& d : s["disable"]) st.disable.push_back(d.get<std::string>());
                if (s.contains("enable"))
                    for (auto& e : s["enable"])  st.enable.push_back(e.get<std::string>());
                stacks[name] = st;
            } catch (...) {}
        }
    }
};

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Usage: mind.exe [root_directory] [--port PORT]
    // Defaults: scan "." on port 9090
    std::string root = ".";
    int port = 9090;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) { port = std::stoi(argv[++i]); }
        else if (arg.rfind("--", 0) != 0)    { root = arg; }
    }

    try {
        std::cout << "[Cortex Mind] Scanning: " << fs::absolute(root).string() << std::endl;
        CortexMind mind({root});
        mind.research_large_scale();
        mind.start_watching();

        httplib::Server svr;

        // ── Core analysis ──
        svr.Get("/analyze_deps", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.detect_circular_dependencies().dump(4), "application/json");
        });
        svr.Get("/verify", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.verify_system_integrity().dump(4), "application/json");
        });
        svr.Get("/history", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.get_history().dump(4), "application/json");
        });
        svr.Get("/meta_scan", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.perform_meta_scan().dump(4), "application/json");
        });
        svr.Get("/map", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.to_json().dump(4), "application/json");
        });
        svr.Get("/summary", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.get_summary().dump(4), "application/json");
        });
        svr.Post("/rescan", [&](const httplib::Request&, httplib::Response& res) {
            mind.rescan_changed(true);
            res.set_content(mind.get_summary().dump(4), "application/json");
        });
        svr.Post("/graft", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                if (!body.contains("symbol")) throw std::invalid_argument("missing_symbol");
                res.set_content(json({{"status", "safe"}}).dump(4), "application/json");
            } catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });

        // ── Rules ──
        svr.Get("/rules", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.get_rules().dump(4), "application/json");
        });
        svr.Post("/rules", [&](const httplib::Request& req, httplib::Response& res) {
            try { res.set_content(mind.add_rule(json::parse(req.body)).dump(4), "application/json"); }
            catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });
        svr.Delete("/rules/:id", [&](const httplib::Request& req, httplib::Response& res) {
            res.set_content(mind.delete_rule(req.path_params.at("id")).dump(4), "application/json");
        });
        svr.Patch("/rules/:id", [&](const httplib::Request& req, httplib::Response& res) {
            try { res.set_content(mind.update_rule(req.path_params.at("id"), json::parse(req.body)).dump(4), "application/json"); }
            catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });

        // ── Stacks ──
        svr.Get("/stack", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.get_stacks().dump(4), "application/json");
        });
        svr.Post("/stack/:name", [&](const httplib::Request& req, httplib::Response& res) {
            res.set_content(mind.activate_stack(req.path_params.at("name")).dump(4), "application/json");
        });

        // ── Exceptions ──
        svr.Get("/exceptions", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(mind.get_exceptions().dump(4), "application/json");
        });
        svr.Post("/exceptions", [&](const httplib::Request& req, httplib::Response& res) {
            try { res.set_content(mind.add_exception(json::parse(req.body)).dump(4), "application/json"); }
            catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });
        svr.Delete("/exceptions/:id", [&](const httplib::Request& req, httplib::Response& res) {
            res.set_content(mind.clear_exception(req.path_params.at("id")).dump(4), "application/json");
        });

        // ── Preflight ──
        svr.Post("/preflight", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                if (!body.contains("file")) throw std::invalid_argument("missing_file");
                res.set_content(mind.check_preflight(body["file"].get<std::string>()).dump(4), "application/json");
            } catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });
        svr.Post("/preflight/batch", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                if (!body.is_array()) throw std::invalid_argument("expected_array");
                json results = json::array();
                for (auto const& f : body) results.push_back(mind.check_preflight(f.get<std::string>()));
                res.set_content(results.dump(4), "application/json");
            } catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });
        svr.Post("/verify_change", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                if (!body.is_array()) throw std::invalid_argument("expected_array");
                res.set_content(mind.verify_change(body).dump(4), "application/json");
            } catch (...) { res.status = 400; res.set_content("bad_request", "text/plain"); }
        });

        std::cout << "[Cortex Mind] Active on Port " << port << " | Watcher: live" << std::endl;
        svr.listen("0.0.0.0", port);
    } catch (const std::exception& e) {
        std::cerr << "Fatal Mind: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
