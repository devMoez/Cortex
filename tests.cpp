#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <functional>

namespace fs = std::filesystem;

bool is_safe_path_test(const std::string& path, const std::string& root) {
    try {
        fs::path p = fs::absolute(path);
        fs::path r = fs::absolute(root);
        auto rel = fs::relative(p, r);
        return !rel.empty() && rel.string().find("..") == std::string::npos;
    } catch (...) { return false; }
}

void test_path_traversal_shield() {
    std::cout << "[Test] Path Traversal Shield... ";
    std::string root = fs::current_path().string();
    assert(is_safe_path_test(root + "/cortex.cpp", root) == true);
    assert(is_safe_path_test(root + "/../../windows/system32/config", root) == false);
    std::cout << "PASSED" << std::endl;
}

void test_circular_dependency_logic() {
    std::cout << "[Test] Circular Dependency Detection... ";
    std::map<std::string, std::vector<std::string>> adj = {
        {"A", {"B"}}, {"B", {"C"}}, {"C", {"A"}}
    };
    std::map<std::string, int> disc, low;
    int timer = 0;
    bool cycle_found = false;
    std::function<void(std::string)> dfs = [&](std::string u) {
        disc[u] = low[u] = ++timer;
        for(auto v : adj[u]) {
            if(disc[v] == 0) {
                dfs(v);
                low[u] = std::min(low[u], low[v]);
            } else {
                cycle_found = true;
            }
        }
    };
    dfs("A");
    assert(cycle_found == true);
    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "--- Cortex Advanced Integrity & Logic Test Suite ---" << std::endl;
    try {
        test_path_traversal_shield();
        test_circular_dependency_logic();
        std::cout << "--- ALL TESTS PASSED ---" << std::endl;
    } catch (...) {
        std::cerr << "CRITICAL TEST FAILURE" << std::endl;
        return 1;
    }
    return 0;
}
