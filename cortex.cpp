#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

json archetypes;

std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

int main() {
    std::ifstream f("archetypes.json");
    if (!f.is_open()) { std::cerr << "Error: Could not open archetypes.json" << std::endl; return 1; }
    try { f >> archetypes; } catch (...) { std::cerr << "Error parsing archetypes.json" << std::endl; return 1; }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        std::ifstream file("test.html");
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.set_content(content, "text/html");
        } else { res.status = 404; res.set_content("UI not found", "text/plain"); }
    });

    // --- Proxy Endpoints to Internal Mind ---
    auto proxy_get = [](const std::string& endpoint) {
        return [endpoint](const httplib::Request &, httplib::Response &res) {
            httplib::Client brain("localhost", 9090);
            auto brain_res = brain.Get(endpoint);
            if (brain_res && brain_res->status == 200) {
                res.set_content(brain_res->body, "application/json");
            } else {
                res.status = 503;
                res.set_content("{\"error\": \"Internal Brain Offline\"}", "application/json");
            }
        };
    };

    svr.Get("/analyze_deps", proxy_get("/analyze_deps"));
    svr.Get("/history", proxy_get("/history"));
    svr.Get("/verify", proxy_get("/verify"));
    svr.Get("/meta_scan", proxy_get("/meta_scan"));
    svr.Get("/map", proxy_get("/map"));

    svr.Post("/expand", [](const httplib::Request &req, httplib::Response &res) {
        json body; try { body = json::parse(req.body); } catch (...) { res.status = 400; return; }
        std::string name = body.value("component_name", "Unknown");
        
        // Risk assessment via brain
        httplib::Client brain("localhost", 9090);
        json graft_req = {{"symbol", name}};
        auto graft_res = brain.Post("/graft", graft_req.dump(), "application/json");

        auto match_str = [&](const std::string& s) {
            for (auto& arch : archetypes) {
                for (auto& kw : arch["keywords"]) if (s.find(kw.get<std::string>()) != std::string::npos) return arch;
            }
            return json(nullptr);
        };

        json matched = match_str(name);
        if (matched.is_null()) matched = {{"name", "Generic"}, {"default_logic", "/* Logic */"}, {"required_imports", json::array()}, {"states", json::array()}};

        std::string code = replaceAll(matched["default_logic"], "{{COMPONENT_NAME}}", name);
        json out = {{"archetype", matched["name"]}, {"code", code}, {"imports", matched["required_imports"]}};
        res.set_content(out.dump(), "application/json");
    });

    std::cout << "[Cortex Core] Active on http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    return 0;
}
