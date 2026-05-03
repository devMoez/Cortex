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
    std::vector<std::string> deps;
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
                        this->condition.wait(lock, [this]{ return stop.load() || !this->tasks.empty(); });
                        if(stop.load() && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    if (task) task();
                }
            });
    }
    template<class F> void enqueue(F&& f) {
        { std::unique_lock<std::mutex> lock(queue_mutex); tasks.emplace(std::forward<F>(f)); }
        condition.notify_one();
    }
    ~WorkerPool() {
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

class UserMapper {
public:
    UserMapper(const std::vector<std::string>& roots) : roots(roots) { load_map(); }
    void scan_parallel() {
        unsigned int n = std::thread::hardware_concurrency();
        WorkerPool pool(n > 0 ? n : 4);
        std::atomic<int> active(0);
        std::mutex mtx;
        for (const auto& root : roots) {
            if (!fs::exists(root)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (path.find(".git") != std::string::npos) continue;
                    active++;
                    pool.enqueue([this, path, &mtx, &active]() {
                        FileInfo info = process_file(path);
                        { std::lock_guard<std::mutex> lock(mtx); file_map[path] = info; }
                        active--;
                    });
                }
            }
        }
        while(active.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        save_map();
    }
    void generate_stubs() {
        for(auto const& [path, info] : file_map) {
            if(info.purpose.empty() && path.find(".cpp") != std::string::npos) {
                std::ifstream ifs(path); std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                std::ofstream ofs(path); ofs << "// Purpose: TODO: verify\n" << content;
            }
        }
    }
    json to_json() {
        json j = json::array();
        for(auto const& [p, i] : file_map) j.push_back({{"path", p}, {"symbols", i.symbols}});
        return j;
    }
private:
    std::vector<std::string> roots;
    std::map<std::string, FileInfo> file_map;
    FileInfo process_file(const std::string& path) {
        FileInfo info; info.path = path;
        std::ifstream f(path); std::string line;
        std::regex re_p(R"(//\s*Purpose:\s*(.*))");
        std::regex re_f(R"((?:void|int|auto|std::string)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\()");
        while(std::getline(f, line)) {
            std::smatch m;
            if(std::regex_search(line, m, re_p)) info.purpose = m[1];
            if(std::regex_search(line, m, re_f)) info.symbols.push_back(m[1]);
        }
        return info;
    }
    void load_map() { std::ifstream f("map.json"); if(f.is_open()) { json j; try { f >> j; for(auto& item: j) { FileInfo i; i.path = item["path"]; i.symbols = item["symbols"]; file_map[i.path] = i; } } catch(...) {} } }
    void save_map() { std::ofstream f("map.json"); f << to_json().dump(4); }
};

int main(int argc, char* argv[]) {
    std::vector<std::string> roots = {"."};
    UserMapper mapper(roots);
    mapper.scan_parallel();
    bool server = false;
    for(int i=1; i<argc; ++i) if(std::string(argv[i]) == "--server") server = true;
    if(server) {
        httplib::Server svr;
        svr.Get("/map", [&](const httplib::Request&, httplib::Response &res) { res.set_content(mapper.to_json().dump(4), "application/json"); });
        std::cout << "[User Mapper] Port 8080" << std::endl;
        svr.listen("0.0.0.0", 8080);
    }
    return 0;
}
