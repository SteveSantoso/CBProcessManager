// MessageRouter.h  -  前端↔后端消息路由
#pragma once
#include <string>
#include <windows.h>

class MessageRouter {
public:
    static MessageRouter& instance();

    // 收到前端消息时由 WebViewHost 调用
    // json 为 WebMessageReceived 回调传入的宽字符 JSON
    void dispatch(const std::wstring& json);

    // 由 ProcessService 状态回调调用，将状态推送给前端
    void pushProcessStatus(const std::string& id, const std::string& status);

    // 向前端推送完整进程列表
    void pushProcessList();

    // 向前端推送全局配置
    void pushConfig();

private:
    MessageRouter() = default;

    // 各消息动作的处理函数
    void handleGetProcessList();
    void handleStartProcess(const std::string& id);
    void handleStopProcess(const std::string& id);
    void handleAddProcess(const std::string& jsonObj);
    void handleUpdateProcess(const std::string& jsonObj);
    void handleDeleteProcess(const std::string& id);
    void handleOpenFilePicker(HWND hwnd);
    void handleSaveConfig(const std::string& jsonObj);
    void handleGetConfig();
    void handleStartAll();
    void handleStopAll();

    // 将 WebView2 传来的宽字符 JSON 转换为 UTF-8
    static std::string wideToUtf8(const std::wstring& w);
};
