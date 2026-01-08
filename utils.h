#ifndef UTILS_H
#define UTILS_H

#include <cstdlib>
#include <string>

inline int getenv_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

inline double getenv_double(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

inline bool getenv_bool(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    std::string s(v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

inline std::string getenv_string(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    return std::string(v);
}

#endif // UTILS_H
