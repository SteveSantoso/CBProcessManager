// WebViewHost.cpp  - WebView2 宿主封装
#include "WebViewHost.h"
#include "Logger.h"
#include <shlwapi.h>
#include <stdexcept>
#include <fstream>
#include <ctime>

using namespace Microsoft::WRL;

// pmLog / pmLogF 已由 Logger.h 宏提供，此处仅保留 pmLogHR 辅助函数
static void pmLogHR(const wchar_t* tag, HRESULT hr) {
    wchar_t buf[256];
    swprintf_s(buf, L"[WebView2] %s  hr=0x%08X", tag, (unsigned)hr);
    pmLog(buf);
}

// ─── 单例 ─────────────────────────────────────────────────────────────────────
WebViewHost& WebViewHost::instance() {
    static WebViewHost inst;
    return inst;
}

// ─── 初始化 ───────────────────────────────────────────────────────────────────
void WebViewHost::initialize(HWND hwnd, ReadyCallback readyCb) {
    m_hwnd    = hwnd;
    m_readyCb = std::move(readyCb);

    pmLog(L"[WebView2] 初始化开始");

    // 计算 exe 同目录下的各路径
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    wchar_t udDir[MAX_PATH] = {};
    wcscpy_s(udDir, exeDir);
    PathAppendW(udDir, L"WebView2UserData");

    wchar_t htmlDir[MAX_PATH] = {};
    wcscpy_s(htmlDir, exeDir);
    PathAppendW(htmlDir, L"html");

    // 检查 html 目录是否存在
    if (!PathIsDirectoryW(htmlDir)) {
        wchar_t err[512];
        swprintf_s(err, L"[WebView2] html 目录不存在：%s", htmlDir);
        pmLog(err);
        MessageBoxW(hwnd, err, L"ProcessManager 错误", MB_ICONERROR);
        return;
    }

    {
        wchar_t buf[512];
        swprintf_s(buf, L"[WebView2] exeDir=%s  htmlDir=%s", exeDir, htmlDir);
        pmLog(buf);
    }

    // 将路径存入 std::wstring，以便在 lambda 中捕获
    std::wstring htmlDirStr(htmlDir);
    std::wstring udDirStr(udDir);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        udDirStr.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, hwnd, htmlDirStr](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                pmLogHR(L"环境创建完成", result);
                if (FAILED(result) || !env) {
                    wchar_t msg[256];
                    swprintf_s(msg, L"WebView2 环境创建失败 (0x%08X)\n请确认已安装 WebView2 运行时：\nhttps://aka.ms/webview2", (unsigned)result);
                    MessageBoxW(hwnd, msg, L"ProcessManager", MB_ICONERROR);
                    return result;
                }

                HRESULT hr2 = env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, hwnd, htmlDirStr](
                            HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                            pmLogHR(L"控制器创建完成", result);
                            if (FAILED(result) || !ctrl) {
                                wchar_t msg[256];
                                swprintf_s(msg, L"WebView2 控制器创建失败 (0x%08X)", (unsigned)result);
                                MessageBoxW(hwnd, msg, L"ProcessManager", MB_ICONERROR);
                                return result;
                            }

                            m_controller = ctrl;
                            m_controller->put_IsVisible(TRUE);

                            HRESULT grv = ctrl->get_CoreWebView2(m_webview.put());
                            pmLogHR(L"获取CoreWebView2接口", grv);
                            if (FAILED(grv) || !m_webview) {
                                MessageBoxW(hwnd, L"无法获取 ICoreWebView2 接口", L"ProcessManager", MB_ICONERROR);
                                return grv;
                            }

                            m_webview3 = m_webview.try_query<ICoreWebView2_3>();
                            pmLog(m_webview3 ? L"[WebView2] ICoreWebView2_3 接口可用" : L"[WebView2] ICoreWebView2_3 接口不可用");

                            // 将 WebView2 设置为填满窗口客户区
                            RECT bounds{};
                            GetClientRect(hwnd, &bounds);
                            {
                                wchar_t b[128];
                                swprintf_s(b, L"[WebView2] 窗口客户区大小=%d,%d,%d,%d", bounds.left, bounds.top, bounds.right, bounds.bottom);
                                pmLog(b);
                            }
                            m_controller->put_Bounds(bounds);

                            // 配置 WebView2 选项
                            wil::com_ptr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webview->get_Settings(&settings))) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE); // 禁用右键菜单
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE); 
                            }

                            // 将本地 html 目录映射到虚拟主机名 app.local
                            if (m_webview3) {
                                HRESULT mhr = m_webview3->SetVirtualHostNameToFolderMapping(
                                    L"app.local", htmlDirStr.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                wchar_t mb[512];
                                swprintf_s(mb, L"[WebView2] 虚拟主机映射('%s') hr=0x%08X", htmlDirStr.c_str(), (unsigned)mhr);
                                pmLog(mb);
                            } else {
                                pmLog(L"[WebView2] 跳过虚拟主机映射：ICoreWebView2_3 接口不可用");
                            }

                            // 注册前端消息接收回调
                            m_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string jsonStr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&jsonStr)) && m_msgCb) {
                                            m_msgCb(jsonStr.get());
                                        }
                                        return S_OK;
                                    }).Get(), &m_msgToken);

                            m_ready = true;

                            // 页面首次导航完成后触发就绪回调，确保前端已加载
                            m_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2* wv,
                                        ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL success = FALSE;
                                        args->get_IsSuccess(&success);
                                        pmLog(success ? L"[WebView2] 页面导航成功" : L"[WebView2] 页面导航失败");
                                        if (!success) {
                                            COREWEBVIEW2_WEB_ERROR_STATUS errStatus{};
                                            args->get_WebErrorStatus(&errStatus);
                                            wchar_t eb[128];
                                            swprintf_s(eb, L"[WebView2] 网页错误状态码=%d", (int)errStatus);
                                            pmLog(eb);
                                        }
                                        // 只触发一次，之后移除监听
                                        wv->remove_NavigationCompleted(m_navToken);
                                        if (success && m_readyCb) m_readyCb();
                                        return S_OK;
                                    }).Get(), &m_navToken);

                            const wchar_t* url = m_webview3
                                ? L"https://app.local/index.html"
                                : L"about:blank";
                            {
                                wchar_t nb[256];
                                swprintf_s(nb, L"[WebView2] 开始导航 -> %s", url);
                                pmLog(nb);
                            }
                            m_webview->Navigate(url);

                            return S_OK;
                        }).Get());
                pmLogHR(L"创建控制器", hr2);
                return S_OK;
            }).Get());

    pmLogHR(L"创建WebView2环境", hr);
    if (FAILED(hr)) {
        wchar_t msg[256];
        swprintf_s(msg, L"WebView2 初始化失败 (0x%08X)\n请确认已安装 WebView2 运行时：\nhttps://aka.ms/webview2", (unsigned)hr);
        MessageBoxW(hwnd, msg, L"ProcessManager", MB_ICONERROR);
    }
}


// ─── 向前端发送 JSON 消息 ────────────────────────────────────────────────────
void WebViewHost::sendMessage(const std::wstring& json) {
    if (m_webview && m_ready)
        m_webview->PostWebMessageAsJson(json.c_str());
}

void WebViewHost::sendMessage(const std::string& utf8json) {
    if (!m_webview || !m_ready) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8json.c_str(), -1, nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8json.c_str(), -1, wide.data(), len);
    wide.resize(wcslen(wide.c_str()));
    m_webview->PostWebMessageAsJson(wide.c_str());
}

// ─── 导航到指定 URL ──────────────────────────────────────────────────────────
void WebViewHost::navigate(const std::wstring& url) {
    if (m_webview && m_ready)
        m_webview->Navigate(url.c_str());
}

// ─── 设置前端消息回调 ────────────────────────────────────────────────────────
void WebViewHost::setMessageCallback(MessageCallback cb) {
    m_msgCb = std::move(cb);
}

// ─── 调整 WebView2 大小 ──────────────────────────────────────────────────────
void WebViewHost::resize(RECT bounds) {
    if (m_controller) m_controller->put_Bounds(bounds);
}

void WebViewHost::onResize(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    resize(rc);
}
