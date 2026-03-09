#pragma once
#include <string>
#include <vector>

class FileDialogs {
public:
    // Returns an empty string if the user closes the window without saving/loading
    static std::string OpenFile(const char* title, const std::vector<std::string>& filters);
    static std::string SaveFile(const char* title, const std::vector<std::string>& filters);
};