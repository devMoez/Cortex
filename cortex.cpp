#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

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
    f >> archetypes;

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        std::ifstream file("test.html");
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        res.set_content(content, "text/html");
    });

    svr.Post("/expand", [](const httplib::Request &req, httplib::Response &res) {
        auto body = json::parse(req.body);
        std::string name = body["component_name"];
        std::string desc = body["description"];
        
        json matched = nullptr;
        
        auto match_str = [&](const std::string& s) {
            for (auto& arch : archetypes) {
                for (auto& kw : arch["keywords"]) {
                    std::string k = kw;
                    if (s.find(k) != std::string::npos) return arch;
                }
            }
            return json(nullptr);
        };

        matched = match_str(name);
        if (matched.is_null()) matched = match_str(desc);
        if (matched.is_null()) {
            matched = {
                {"name", "Container"},
                {"default_logic", "/* Generic container */"},
                {"required_imports", json::array()},
                {"states", json::array()}
            };
        }

        std::string code = replaceAll(matched["default_logic"], "{{COMPONENT_NAME}}", name);

        json output = {
            {"matched_archetype", matched["name"]},
            {"injected_code", code},
            {"imports_to_add", matched["required_imports"]},
            {"states_added", matched["states"]}
        };

        res.set_content(output.dump(), "application/json");
    });

    std::cout << "Server starting at http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    return 0;
}
