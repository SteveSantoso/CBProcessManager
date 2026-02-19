// ConfigService.h  -  配置文件读写服务
#pragma once
#include <string>
#include <vector>

// ─── 数据结构 ─────────────────────────────────────────────────────────────────

struct ProcessConfig {
    std::string id;
    std::string name;
    std::string path;
    std::string type;          // "exe" 或 "bat"
    std::string args;
    int         delaySeconds      = 0;
    bool        guardEnabled      = true;
    int         guardDelaySeconds = 1;
    bool        enabled           = true;
};

struct AppConfig {
    bool                       autoStartOnOpen = false;
    std::vector<ProcessConfig> processes;
};

// ─── 服务类 ───────────────────────────────────────────────────────────────────

class ConfigService {
public:
    static ConfigService& instance();

    // 加载 config.json；不存在时自动创建默认配置
    bool load();

    // 将当前配置保存到 config.json
    bool save();

    AppConfig& config();
    const AppConfig& config() const;

    // 生成类 UUID 的唯一 id
    static std::string newId();

    // 根据文件扩展名判断进程类型
    static std::string typeFromPath(const std::string& path);

    // JSON 序列化辅助函数（供 MessageRouter 调用）
    static std::string appConfigToJson(const AppConfig& cfg);
    static std::string processConfigToJson(const ProcessConfig& p);

private:
    ConfigService() = default;
    AppConfig  m_config;
    std::string configFilePath() const;
};
