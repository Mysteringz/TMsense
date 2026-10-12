#pragma once
#include "Arduino.h"
#include <map>
inline std::map<std::string, std::vector<uint8_t>> host_nvs;
inline std::string host_nvs_fail;
inline bool host_nvs_unavailable = false;
class Preferences {
    std::string ns;
    bool open = false;
    std::string name(const char* key) { return ns + "/" + key; }
    template <typename T> T get(const char* key, T fallback) {
        auto i = host_nvs.find(name(key));
        if (!open || i == host_nvs.end() || i->second.size() != sizeof(T)) return fallback;
        T out; memcpy(&out, i->second.data(), sizeof(T)); return out;
    }
public:
    bool begin(const char* value, bool) { ns = value; open = !host_nvs_unavailable; return open; }
    void end() { open = false; }
    bool isKey(const char* key) { return open && host_nvs.count(name(key)); }
    bool remove(const char* key) {
        if (!open || host_nvs_fail == name(key)) return false;
        return host_nvs.erase(name(key)) != 0;
    }
    bool clear() {
        if (!open || !host_nvs_fail.empty()) return false;
        for (auto i = host_nvs.begin(); i != host_nvs.end();) {
            if (i->first.rfind(ns + "/", 0) == 0) i = host_nvs.erase(i); else ++i;
        }
        return true;
    }
    size_t putBytes(const char* key, const void* data, size_t len) {
        if (!open || host_nvs_fail == name(key)) return 0;
        const auto* b = static_cast<const uint8_t*>(data); host_nvs[name(key)] = {b, b + len}; return len;
    }
    size_t getBytes(const char* key, void* data, size_t max) {
        auto i = host_nvs.find(name(key));
        if (!open || i == host_nvs.end() || i->second.size() > max) return 0;
        memcpy(data, i->second.data(), i->second.size()); return i->second.size();
    }
    size_t putString(const char* key, const char* text) {
        return putBytes(key, text, strlen(text) + 1) ? strlen(text) : 0;
    }
    size_t getString(const char* key, char* text, size_t max) { return getBytes(key, text, max); }
    uint8_t getUChar(const char* key, uint8_t fallback) { return get(key, fallback); }
    uint16_t getUShort(const char* key, uint16_t fallback) { return get(key, fallback); }
    uint32_t getUInt(const char* key, uint32_t fallback) { return get(key, fallback); }
    size_t putUChar(const char* key, uint8_t v) { return putBytes(key, &v, sizeof(v)); }
    size_t putUShort(const char* key, uint16_t v) { return putBytes(key, &v, sizeof(v)); }
    size_t putUInt(const char* key, uint32_t v) { return putBytes(key, &v, sizeof(v)); }
};
