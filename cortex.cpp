#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <iterator>
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
    if (!f.is_open()) {
        std::cerr << "Error: Could not open archetypes.json" << std::endl;
        return 1;
    }
    try {
        f >> archetypes;
    } catch (const std::exception& e) {
        std::cerr << "Error parsing archetypes.json: " << e.what() << std::endl;
        return 1;
    }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        std::ifstream file("test.html");
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.set_content(content, "text/html");
        } else {
            res.status = 404;
            res.set_content("test.html not found", "text/plain");
        }
    });

    svr.Post("/expand", [](const httplib::Request &req, httplib::Response &res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid JSON", "text/plain");
            return;
        }

        std::string name = body.contains("component_name") ? body["component_name"].get<std::string>() : "Unknown";
        std::string desc = body.contains("description") ? body["description"].get<std::string>() : "";
        
        try {
            std::cout << "[Cortex Core] Consulting brain for risk assessment..." << std::endl;
            httplib::Client brain("localhost", 9090);
            brain.set_connection_timeout(0, 500); 
            
            json graft_req = {{"symbol", name}};
            auto graft_res = brain.Post("/graft", graft_req.dump(), "application/json");
            
            if (graft_res && graft_res->status == 200) {
                auto risk = json::parse(graft_res->body);
                if (risk["status"] == "high_risk") {
                    std::cout << "[Cortex Core] WARNING: Potential collision with " << risk["affected_files"].dump() << std::endl;
                } else {
                    std::cout << "[Cortex Core] Risk assessment clear. Confidence: " << risk["confidence"] << std::endl;
                }
            } else {
                std::cerr << "[Cortex Core] Brain unreachable. Proceeding in standalone mode." << std::endl;
            }
        } catch (...) {
            std::cerr << "[Cortex Core] Brain communication error." << std::endl;
        }

        auto match_str = [&](const std::string& s) {
            for (auto& arch : archetypes) {
                if (arch.contains("keywords")) {
                    for (auto& kw : arch["keywords"]) {
                        std::string k = kw.get<std::string>();
                        if (s.find(k) != std::string::npos) return arch;
                    }
                }
            }
            return json(nullptr);
        };

        json matched = match_str(name);
        if (matched.is_null() && !desc.empty()) matched = match_str(desc);
        if (matched.is_null()) {
            matched = {
                {"name", "Container"},
                {"default_logic", "/* Generic container */"},
                {"required_imports", json::array()},
                {"states", json::array()}
            };
        }

        std::string logic = matched.contains("default_logic") ? matched["default_logic"].get<std::string>() : "";
        std::string code = replaceAll(logic, "{{COMPONENT_NAME}}", name);

        json output = {
            {"matched_archetype", matched["name"]},
            {"injected_code", code},
            {"imports_to_add", matched.contains("required_imports") ? matched["required_imports"] : json::array()},
            {"states_added", matched.contains("states") ? matched["states"] : json::array()}
        };

        res.set_content(output.dump(), "application/json");
    });

    std::cout << "Server starting at http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    return 0;
}
