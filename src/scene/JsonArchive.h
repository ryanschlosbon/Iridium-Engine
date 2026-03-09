#pragma once
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

using json = nlohmann::json;

// --- WRITER ---
class JsonWriteArchive {
public:
    json j; // The JSON object being built

    bool operator()(const char* name, const float& val) { j[name] = val; return false; }
    bool operator()(const char* name, const int& val) { j[name] = val; return false; }
    bool operator()(const char* name, const bool& val) { j[name] = val; return false; }
    bool operator()(const char* name, const glm::vec3& val) {
        j[name] = { val.x, val.y, val.z };
        return false;
    }
    bool operator()(const char* name, const std::string& val) { j[name] = val; return false; }
    bool filePath(const char* name, const std::string& val) { j[name] = val; return false; }
    bool readOnly(const char* name, const std::string& val) { j[name] = val; return false; }
    bool button(const char* name) { return false; }
};

// --- READER ---
class JsonReadArchive {
public:
    const json& j; // The JSON object being read
    JsonReadArchive(const json& j) : j(j) {}

    bool operator()(const char* name, float& val) { if (j.contains(name)) val = j[name]; return false; }
    bool operator()(const char* name, int& val) { if (j.contains(name)) val = j[name]; return false; }
    bool operator()(const char* name, bool& val) { if (j.contains(name)) val = j[name]; return false; }
    bool operator()(const char* name, glm::vec3& val) {
        if (j.contains(name)) {
            val.x = j[name][0];
            val.y = j[name][1];
            val.z = j[name][2];
        }
        return false;
    }
    bool operator()(const char* name, std::string& val) { if (j.contains(name)) val = j[name]; return false; }
    bool filePath(const char* name, std::string& val) { if (j.contains(name)) val = j[name]; return false; }
    bool readOnly(const char* name, std::string& val) { if (j.contains(name)) val = j[name]; return false; }
    bool button(const char* name) { return false; }
};