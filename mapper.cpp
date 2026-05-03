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
#include "json.hpp"
#include "httplib.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

struct FileInfo {
    std::string path;
    std::string purpose;
    std::vector<std::string> symbols;
    std::vector<std::string> dependencies;
    std::vector<std::string> transitive_dependencies;
    bool is_unused = false;
    fs::file_time_type last_write_time;
};

class WorkerPool {
public:
    WorkerPool(size_t threads) : stop(false) {
        for(size_t i = 0; i<threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F> void enqueue(F&& f) {
        { std::unique_lock<std::mutex> lock(queue_mutex); tasks.emplace(std::forward<F>(f)); }
        condition.notify_one();
    }
    ~WorkerPool() {
        { std::unique_lock<std::mutex> lock(queue_mutex); stop = true; }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

class UserMapper {
public:
    UserMapper(const std::vector<std::string>& roots) : roots(roots) {
        load_map();
    }

    void scan_parallel() {
        unsigned int n = std::thread::hardware_concurrency();
        WorkerPool pool(n > 0 ? n : 4);
        std::mutex map_mutex;
        std::atomic<int> active_tasks(0);

        std::cout << "[User Mapper] Industrial scan active (" << n << " threads)..." << std::endl;

        for (const auto& root : roots) {
            if (!fs::exists(root)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (should_skip(path)) continue;

                    active_tasks++;
                    pool.enqueue([this, path, &map_mutex, &active_tasks, entry]() {
                        auto info = process_file_optimized(path);
                        info.last_write_time = fs::last_write_time(entry);
                        std::lock_guard<std::mutex> lock(map_mutex);
                        file_map[path] = info;
                        active_tasks--;
                    });
                }
            }
        }
        while(active_tasks > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        save_map();
    }

    void watch() {
        if (roots.empty()) return;
        HANDLE hDir = CreateFileA(roots[0].c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hDir == INVALID_HANDLE_VALUE) return;
        char buffer[1024]; DWORD bytesReturned;
        std::cout << "[User Mapper] Watching for changes..." << std::endl;
        while (ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, &bytesReturned, NULL, NULL)) {
            FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)buffer;
            do {
                std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
                std::string name(wname.begin(), wname.end());
                std::string full_path = (fs::path(roots[0]) / name).string();
                if (!should_skip(full_path) && fs::exists(full_path) && fs::is_regular_file(full_path)) {
                    std::cout << "Update detected: " << full_path << std::endl;
                    file_map[full_path] = process_file_optimized(full_path);
                    save_map();
                }
                if (info->NextEntryOffset == 0) break;
                info = (FILE_NOTIFY_INFORMATION*)((char*)info + info->NextEntryOffset);
            } while (true);
        }
    }

    json to_json() {
        json j = json::array();
        for (auto const& [path, info] : file_map) {
            j.push_back({{"path", path}, {"symbols", info.symbols}, {"purpose", info.purpose}});
        }
        return j;
    }

    void generate_stubs() {
        for (auto const& [path, info] : file_map) {
            if (info.purpose.empty() && (path.find(".cpp") != std::string::npos)) {
                std::ifstream ifs(path);
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();
                std::ofstream ofs(path);
                ofs << "// Purpose: TODO: verify\n" << content;
            }
        }
    }

private:
    std::vector<std::string> roots;
    std::map<std::string, FileInfo> file_map;

    FileInfo process_file_optimized(const std::string& path) {
        FileInfo info; info.path = path;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return info;
        
        std::string line;
        std::regex re_purpose(R"(//\s*Purpose:\s*(.*))");
        std::regex re_func(R"((?:void|int|auto|std::string|char|float|double)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\()");
        int lines = 0;
        while (std::getline(file, line) && lines < 20000) {
            lines++;
            std::smatch match;
            if (std::regex_search(line, match, re_purpose)) info.purpose = match[1];
            if (std::regex_search(line, match, re_func)) info.symbols.push_back(match[1]);
        }
        return info;
    }

    bool should_skip(const std::string& path) {
        return path.find("node_modules") != std::string::npos || path.find(".git") != std::string::npos || path.find(".exe") != std::string::npos;
    }

    void load_map() {
        std::ifstream f("map.json"); if (!f.is_open()) return;
        json j; try { f >> j; for (auto& item : j) {
            FileInfo i; i.path = item["path"]; i.symbols = item["symbols"].get<std::vector<std::string>>();
            i.purpose = item.contains("purpose") ? item["purpose"].get<std::string>() : "";
            file_map[i.path] = i;
        }} catch(...) {}
    }

    void save_map() { std::ofstream f("map.json"); f << to_json().dump(4); }
};

int main(int argc, char* argv[]) {
    std::vector<std::string> roots = {"."};
    bool watch = false, server = false, stubs = false;
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if(arg == "--watch") watch = true;
        if(arg == "--server") server = true;
        if(arg == "--stubs") stubs = true;
    }

    UserMapper mapper(roots);
    mapper.scan_parallel();
    if (stubs) mapper.generate_stubs();

    if (server) {
        httplib::Server svr;
        svr.Get("/map", [&](const httplib::Request&, httplib::Response &res) {
            res.set_content(mapper.to_json().dump(4), "application/json");
        });
        std::cout << "[User Mapper] Active on http://localhost:8080" << std::endl;
        svr.listen("0.0.0.0", 8080);
    } else if (watch) {
        mapper.watch();
    }
    return 0;
}
