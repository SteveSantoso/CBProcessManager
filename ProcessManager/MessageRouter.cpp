// MessageRouter.cpp  -  前端↔后端消息路由
#include "MessageRouter.h"
#include "WebViewHost.h"
#include "ConfigService.h"
#include "ProcessService.h"
#include "SimpleJson.hpp"
#include <wil/com.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <sstream>
#include <algorithm>

// ─── 单例 ─────────────────────────────────────────────────────────────────────
MessageRouter& MessageRouter::instance() {
    static MessageRouter inst;
    return inst;
}

// ─── UTF-8 编码转换辅助函数 ───────────────────────────────────────────────────
std::string MessageRouter::wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ─── 消息分发 ────────────────────────────────────────────────────────────────
void MessageRouter::dispatch(const std::wstring& json) {
    std::string utf8 = wideToUtf8(json);
    sj::Value msg;
    try { msg = sj::parse(utf8); } catch (...) { return; }
    if (!msg.is_object() || !msg.contains("action")) return;

    std::string action = msg["action"].get_string_or("");

    if (action == "getProcessList") {
        handleGetProcessList();
    } else if (action == "startProcess") {
        handleStartProcess(msg.contains("id") ? msg["id"].get_string_or("") : "");
    } else if (action == "stopProcess") {
        handleStopProcess(msg.contains("id") ? msg["id"].get_string_or("") : "");
    } else if (action == "addProcess") {
        handleAddProcess(msg.contains("process") ? sj::stringify(msg["process"]) : "{}");
    } else if (action == "updateProcess") {
        handleUpdateProcess(msg.contains("process") ? sj::stringify(msg["process"]) : "{}");
    } else if (action == "deleteProcess") {
        handleDeleteProcess(msg.contains("id") ? msg["id"].get_string_or("") : "");
    } else if (action == "openFilePicker") {
        HWND hwnd = ProcessService::instance().mainHwnd();
        handleOpenFilePicker(hwnd);
    } else if (action == "saveConfig") {
        handleSaveConfig(msg.contains("config") ? sj::stringify(msg["config"]) : "{}");
    } else if (action == "getConfig") {
        handleGetConfig();
    } else if (action == "startAll") {
        handleStartAll();
    } else if (action == "stopAll") {
        handleStopAll();
    }
}

// ─── 获取进程列表 ────────────────────────────────────────────────────────────
void MessageRouter::handleGetProcessList() {
    pushProcessList();
}

void MessageRouter::pushProcessList() {
    auto& cfg = ConfigService::instance().config();
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
        obj["background"]       = p.background;
        obj["status"]           = std::string(statusStr(ProcessService::instance().getStatus(p.id)));
        obj["pid"]              = (int)ProcessService::instance().getPid(p.id);
        arr.push_back(sj::Value(std::move(obj)));
    }
    sj::Object resp;
    resp["type"]    = std::string("processListResponse");
    resp["processes"] = std::move(arr);
    WebViewHost::instance().sendMessage(sj::stringify(sj::Value(resp)));
}

// ─── 启动 / 停止进程 ──────────────────────────────────────────────────────────
void MessageRouter::handleStartProcess(const std::string& id) {
    if (id.empty()) return;
    ProcessService::instance().syncConfig();
    ProcessService::instance().startProcess(id);
}

void MessageRouter::handleStopProcess(const std::string& id) {
    if (id.empty()) return;
    ProcessService::instance().stopProcess(id);
}

// ─── 推送进程状态变更 ────────────────────────────────────────────────────────
void MessageRouter::pushProcessStatus(const std::string& id, const std::string& status) {
    sj::Object resp;
    resp["type"]   = std::string("processStatusChanged");
    resp["id"]     = id;
    resp["status"] = status;    resp["pid"]    = (int)ProcessService::instance().getPid(id);    WebViewHost::instance().sendMessage(sj::stringify(sj::Value(resp)));
}

// ─── 添加进程 ────────────────────────────────────────────────────────────────
void MessageRouter::handleAddProcess(const std::string& jsonObj) {
    sj::Value pv;
    try { pv = sj::parse(jsonObj); } catch (...) { return; }
    if (!pv.is_object()) return;

    ProcessConfig p;
    p.id               = ConfigService::newId();
    p.name             = pv.contains("name")             ? pv["name"].get_string_or("") : "";
    p.path             = pv.contains("path")             ? pv["path"].get_string_or("") : "";
    p.type             = pv.contains("type")             ? pv["type"].get_string_or("exe") : ConfigService::typeFromPath(p.path);
    p.args             = pv.contains("args")             ? pv["args"].get_string_or("") : "";
    p.delaySeconds     = pv.contains("delaySeconds")     ? pv["delaySeconds"].get_int_or(0) : 0;
    p.guardEnabled     = pv.contains("guardEnabled")     ? pv["guardEnabled"].get_bool_or(true) : true;
    p.guardDelaySeconds= pv.contains("guardDelaySeconds")? pv["guardDelaySeconds"].get_int_or(3) : 3;
    p.enabled          = pv.contains("enabled")          ? pv["enabled"].get_bool_or(true) : true;
    p.background       = pv.contains("background")       ? pv["background"].get_bool_or(false) : false;

    ConfigService::instance().config().processes.push_back(p);
    ConfigService::instance().save();
    ProcessService::instance().syncConfig();
    pushProcessList();
}

// ─── 更新进程 ────────────────────────────────────────────────────────────────
void MessageRouter::handleUpdateProcess(const std::string& jsonObj) {
    sj::Value pv;
    try { pv = sj::parse(jsonObj); } catch (...) { return; }
    if (!pv.is_object()) return;

    std::string id = pv.contains("id") ? pv["id"].get_string_or("") : "";
    if (id.empty()) return;

    auto& procs = ConfigService::instance().config().processes;
    auto it = std::find_if(procs.begin(), procs.end(),
        [&](const ProcessConfig& c) { return c.id == id; });
    if (it == procs.end()) return;

    if (pv.contains("name"))             it->name             = pv["name"].get_string_or(it->name);
    if (pv.contains("path"))             it->path             = pv["path"].get_string_or(it->path);
    if (pv.contains("type"))             it->type             = pv["type"].get_string_or(it->type);
    if (pv.contains("args"))             it->args             = pv["args"].get_string_or(it->args);
    if (pv.contains("delaySeconds"))     it->delaySeconds     = pv["delaySeconds"].get_int_or(it->delaySeconds);
    if (pv.contains("guardEnabled"))     it->guardEnabled     = pv["guardEnabled"].get_bool_or(it->guardEnabled);
    if (pv.contains("guardDelaySeconds"))it->guardDelaySeconds= pv["guardDelaySeconds"].get_int_or(it->guardDelaySeconds);
    if (pv.contains("enabled"))          it->enabled          = pv["enabled"].get_bool_or(it->enabled);
    if (pv.contains("background"))       it->background       = pv["background"].get_bool_or(it->background);

    ConfigService::instance().save();
    pushProcessList();
}

// ─── 删除进程 ────────────────────────────────────────────────────────────────
void MessageRouter::handleDeleteProcess(const std::string& id) {
    if (id.empty()) return;
    // 先停止进程
    ProcessService::instance().stopProcess(id);

    auto& procs = ConfigService::instance().config().processes;
    procs.erase(std::remove_if(procs.begin(), procs.end(),
        [&](const ProcessConfig& c) { return c.id == id; }), procs.end());
    ConfigService::instance().save();
    pushProcessList();
}

// ─── 打开文件选择对话框 ──────────────────────────────────────────────────────
void MessageRouter::handleOpenFilePicker(HWND hwnd) {
    wil::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;

    COMDLG_FILTERSPEC filters[] = {
        { L"可执行/批处理文件", L"*.exe;*.bat;*.cmd" },
        { L"所有文件",          L"*.*"               }
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(L"选择要管理的程序");
    dlg->SetOkButtonLabel(L"选择");

    if (FAILED(dlg->Show(hwnd))) return; // 用户取消了选择

    wil::com_ptr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;

    wil::unique_cotaskmem_string wpath;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) return;

    std::string path = wideToUtf8(wpath.get());
    std::string type = ConfigService::typeFromPath(path);

    sj::Object resp;
    resp["type"] = std::string("filePickerResult");
    resp["path"] = path;
    resp["fileType"] = type;
    WebViewHost::instance().sendMessage(sj::stringify(sj::Value(resp)));
}

// ─── 保存配置 ────────────────────────────────────────────────────────────────
void MessageRouter::handleSaveConfig(const std::string& jsonObj) {
    sj::Value cv;
    try { cv = sj::parse(jsonObj); } catch (...) { return; }
    if (!cv.is_object()) return;

    if (cv.contains("autoStartOnOpen"))
        ConfigService::instance().config().autoStartOnOpen =
            cv["autoStartOnOpen"].get_bool_or(false);

    ConfigService::instance().save();
    pushConfig();
}

// ─── 获取配置 ────────────────────────────────────────────────────────────────
void MessageRouter::handleGetConfig() { pushConfig(); }

void MessageRouter::pushConfig() {
    const auto& cfg = ConfigService::instance().config();
    sj::Object resp;
    resp["type"]            = std::string("configResponse");
    resp["autoStartOnOpen"] = cfg.autoStartOnOpen;
    WebViewHost::instance().sendMessage(sj::stringify(sj::Value(resp)));
}

// ─── 全部启动 / 全部停止 ─────────────────────────────────────────────────────
void MessageRouter::handleStartAll() {
    ProcessService::instance().startAll();
}

void MessageRouter::handleStopAll() {
    ProcessService::instance().stopAll();
}
