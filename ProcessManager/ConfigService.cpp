// ConfigService.cpp  -  配置文件读写实现
#include "ConfigService.h"
#include "SimpleJson.hpp"

#include <windows.h>
#include <shlwapi.h>
#include <fstream>
#include <sstream>
#include <random>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// ─── 单例 ─────────────────────────────────────────────────────────────────────
ConfigService& ConfigService::instance() {
    static ConfigService inst;
    return inst;
}

AppConfig& ConfigService::config() { return m_config; }
const AppConfig& ConfigService::config() const { return m_config; }

// ─── 配置文件路径 ─────────────────────────────────────────────────────────────
std::string ConfigService::configFilePath() const {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    PathRemoveFileSpecA(path);
    PathAppendA(path, "config.json");
    return path;
}

// ─── UUID 生成器 ─────────────────────────────────────────────────────────────
std::string ConfigService::newId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis;
    std::ostringstream oss;
    auto r = [&]() { return dis(gen); };
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << r() << '-';
    oss << std::setw(4) << (r() & 0xFFFF) << '-';
    oss << std::setw(4) << ((r() & 0x0FFF) | 0x4000) << '-';
    oss << std::setw(4) << ((r() & 0x3FFF) | 0x8000) << '-';
    oss << std::setw(4) << (r() & 0xFFFF);
    oss << std::setw(8) << r();
    return oss.str();
}

// ─── 根据扩展名判断类型 ──────────────────────────────────────────────────────
std::string ConfigService::typeFromPath(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "exe";
    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "bat" || ext == "cmd") return "bat";
    return "exe";
}

// ─── 加载配置 ────────────────────────────────────────────────────────────────
bool ConfigService::load() {
    std::string fpath = configFilePath();
    std::ifstream ifs(fpath);
    if (!ifs.is_open()) {
        // 文件不存在，创建默认配置
        m_config = AppConfig{};
        return save();
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string text = ss.str();
    if (text.empty()) { m_config = AppConfig{}; return true; }

    try {
        sj::Value root = sj::parse(text);
        m_config.autoStartOnOpen = root.contains("autoStartOnOpen")
            ? root["autoStartOnOpen"].get_bool_or(false) : false;

        m_config.processes.clear();
        if (root.contains("processes") && root["processes"].is_array()) {
            for (size_t i = 0; i < root["processes"].size(); ++i) {
                const sj::Value& pv = root["processes"][i];
                ProcessConfig p;
                p.id               = pv.contains("id")               ? pv["id"].get_string_or("")      : newId();
                p.name             = pv.contains("name")             ? pv["name"].get_string_or("")     : "";
                p.path             = pv.contains("path")             ? pv["path"].get_string_or("")     : "";
                p.type             = pv.contains("type")             ? pv["type"].get_string_or("exe")  : typeFromPath(p.path);
                p.args             = pv.contains("args")             ? pv["args"].get_string_or("")     : "";
                p.delaySeconds     = pv.contains("delaySeconds")     ? pv["delaySeconds"].get_int_or(0) : 0;
                p.guardEnabled     = pv.contains("guardEnabled")     ? pv["guardEnabled"].get_bool_or(true) : true;
                p.guardDelaySeconds= pv.contains("guardDelaySeconds")? pv["guardDelaySeconds"].get_int_or(3) : 3;
                p.enabled          = pv.contains("enabled")          ? pv["enabled"].get_bool_or(true)  : true;
                m_config.processes.push_back(std::move(p));
            }
        }
        return true;
    } catch (...) {
        m_config = AppConfig{};
        return false;
    }
}

// ─── 保存配置 ────────────────────────────────────────────────────────────────
bool ConfigService::save() {
    std::string json = appConfigToJson(m_config);
    std::ofstream ofs(configFilePath());
    if (!ofs.is_open()) return false;
    ofs << json;
    return true;
}

// ─── JSON 序列化 ─────────────────────────────────────────────────────────────
std::string ConfigService::processConfigToJson(const ProcessConfig& p) {
    sj::Object obj;
    obj["id"]               = p.id;
    obj["name"]             = p.name;
    obj["path"]             = p.path;
    obj["type"]             = p.type;
    obj["args"]             = p.args;
    obj["delaySeconds"]     = p.delaySeconds;
    obj["guardEnabled"]     = p.guardEnabled;
    obj["guardDelaySeconds"]= p.guardDelaySeconds;
    obj["enabled"]          = p.enabled;
    return sj::stringify(sj::Value(obj));
}

std::string ConfigService::appConfigToJson(const AppConfig& cfg) {
    sj::Object root;
    root["autoStartOnOpen"] = cfg.autoStartOnOpen;
    sj::Array arr;
    for (const auto& p : cfg.processes) {
        sj::Object obj;
        obj["id"]               = p.id;
        obj["name"]             = p.name;
        obj["path"]             = p.path;
        obj["type"]             = p.type;
        obj["args"]             = p.args;
        obj["delaySeconds"]     = p.delaySeconds;
        obj["guardEnabled"]     = p.guardEnabled;
        obj["guardDelaySeconds"]= p.guardDelaySeconds;
        obj["enabled"]          = p.enabled;
        arr.push_back(sj::Value(obj));
    }
    root["processes"] = arr;
    return sj::stringify(sj::Value(root));
}
