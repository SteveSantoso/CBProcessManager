// WebViewHost.h  -  WebView2 单例封装
#pragma once
#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include <string>
#include <functional>

class WebViewHost {
public:
    using MessageCallback = std::function<void(const std::wstring& json)>;
    using ReadyCallback   = std::function<void()>;

    static WebViewHost& instance();

    // 异步初始化 WebView2；就绪后在 UI 线程上调用 readyCb
    void initialize(HWND hwnd, ReadyCallback readyCb);

    // 向前端发送 JSON 消息（必须在 UI 线程调用）
    void sendMessage(const std::wstring& json);
    void sendMessage(const std::string& utf8json);

    // 导航到指定 URL（初始化完成后调用）
    void navigate(const std::wstring& url);

    // 设置前端消息回调
    void setMessageCallback(MessageCallback cb);

    // 调整 WebView2 控件大小以填满 hwnd 客户区
    void resize(RECT bounds);

    // 由 WM_SIZE 消息触发
    void onResize(HWND hwnd);

    bool isReady() const { return m_ready; }

private:
    WebViewHost() = default;
    void setupAfterInit(HWND hwnd);

    HWND                            m_hwnd      = nullptr;
    bool                            m_ready     = false;
    ReadyCallback                   m_readyCb;
    MessageCallback                 m_msgCb;

    wil::com_ptr<ICoreWebView2Controller>   m_controller;
    wil::com_ptr<ICoreWebView2>             m_webview;
    wil::com_ptr<ICoreWebView2_3>           m_webview3;

    EventRegistrationToken                  m_msgToken{};  // 前端消息监听 token
    EventRegistrationToken                  m_navToken{};  // 导航完成监听 token（仅触发一次）
};
