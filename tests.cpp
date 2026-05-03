#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

// Mock of the safety check from mind.cpp
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
    
    // Internal path should be safe
    assert(is_safe_path_test(root + "/cortex.cpp", root) == true);
    
    // Parent directory attempt should be blocked
    assert(is_safe_path_test(root + "/../../windows/system32/config", root) == false);
    
    std::cout << "PASSED" << std::endl;
}

void test_filesystem_integrity() {
    std::cout << "[Test] Filesystem Integrity... ";
    assert(fs::exists("cortex.cpp"));
    assert(fs::exists("internal_brain/mind.cpp"));
    assert(fs::exists("mapper.cpp"));
    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "--- Cortex Security & Integrity Test Suite ---" << std::endl;
    try {
        test_path_traversal_shield();
        test_filesystem_integrity();
        std::cout << "--- ALL TESTS PASSED ---" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
