// main.cpp  -  进程管理器入口点
// Win32 + WebView2 进程管理器
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include "resource.h"
#include "WebViewHost.h"
#include "ProcessService.h"
#include "ConfigService.h"
#include "MessageRouter.h"
#include "Logger.h"
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

static const wchar_t* CLASS_NAME = L"ProcessManagerWnd";
static const wchar_t* WINDOW_TITLE = L"CB进程管理软件";
static HWND           g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static bool           g_trayAdded = false;

// ─── 托盘图标辅助函数 ────────────────────────────────────────────────────────
static void trayAdd(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(GetModuleHandleW(nullptr),
                                       MAKEINTRESOURCEW(IDI_TRAY));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"CB进程管理软件");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = true;
}

static void trayRemove() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void showContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RESTORE, L"显示窗口");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT,    L"退出");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

// ─── WebView2 就绪回调（在 UI 线程中执行）─────────────────────────────────────
static void onWebViewReady() {
    auto& ps  = ProcessService::instance();
    auto& cfg = ConfigService::instance().config();

    // 页面加载完毕；前端挂载后会主动请求进程列表和配置
    // （通过 getProcessList/getConfig 消息触发，消息路由负责处理）

    if (cfg.autoStartOnOpen) {
        ps.startAll();
    }
}

// ─── 窗口消息处理函数 ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;

        // 初始化日志（创建 logs/ 目录，写入启动分隔符）
        AppLogger::init();
        pmLog(L"════════════════════════════════════════════════════════");
        pmLog(L"  此程序由 SteveSantoso 开发，如有盗用违者必究");
        pmLog(L"  Copyright (C) SteveSantoso. All rights reserved.");
        pmLog(L"════════════════════════════════════════════════════════");

        // 初始化 COM 库（单线程套间模式）
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // 添加系统托盘图标
        trayAdd(hwnd);

        // 加载配置文件
        ConfigService::instance().load();

        // 初始化进程服务
        ProcessService::instance().setMainWindow(hwnd);
        ProcessService::instance().syncConfig();

        // 异步初始化 WebView2（完成后回调 onWebViewReady）
        WebViewHost::instance().setMessageCallback(
            [](const std::wstring& json) {
                MessageRouter::instance().dispatch(json);
            });
        WebViewHost::instance().initialize(hwnd, onWebViewReady);
        return 0;
    }

    case WM_SIZE: {
        WebViewHost::instance().onResize(hwnd);
        return 0;
    }

    case WM_SETFOCUS: {
        // 无障碍：将焦点传递给 WebView2 控制器
        // （只要 WebView2 填满窗口，会自动处理焦点传递）
        return 0;
    }

    // ── 进程退出通知（来自线程池 WaitCallback）──────────────────────────────
    case WM_APP_PROC_EXIT: {
        ProcExitCtx* ctx = reinterpret_cast<ProcExitCtx*>(lParam);
        if (ctx) {
            ProcessService::instance().onProcessExited(
                std::string(ctx->id), ctx->pid, ctx->exitCode);
            delete ctx;
        }
        return 0;
    }

    // ── 进程状态变更（来自 ProcessService::notifyStatus）────────────────────
    case WM_APP_STATUS_CHANGED: {
        ProcStatusMsg* msg = reinterpret_cast<ProcStatusMsg*>(lParam);
        if (msg) {
            MessageRouter::instance().pushProcessStatus(
                msg->id, std::string(statusStr(msg->status)));
            delete msg;
        }
        return 0;
    }

    // ── 系统托盘消息 ──────────────────────────────────────────────────────
    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            showContextMenu(hwnd);
        }
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_TRAY_RESTORE:
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            break;
        case ID_TRAY_EXIT:
            ProcessService::instance().stopAll();
            trayRemove();
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }

    case WM_CLOSE: {
        // 点击关闭按钮时最小化到托盘，不退出程序
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY: {
        ProcessService::instance().stopAll();
        trayRemove();
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── 程序入口 ────────────────────────────────────────────────────────────────
int APIENTRY wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       nCmdShow)
{
    // 防止重复运行，限制单实例
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"ProcessManager_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        MessageBoxW(nullptr, L"CB进程管理软件已在运行。", L"提示", MB_ICONINFORMATION);
        return 0;
    }

    // 启用通用控件视觉样式
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // 注册窗口类
    WNDCLASSEXW wc     = {};
    wc.cbSize          = sizeof(wc);
    wc.style           = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc     = WndProc;
    wc.hInstance       = hInstance;
    wc.hIcon           = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm         = wc.hIcon;
    wc.hCursor         = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground   = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName   = CLASS_NAME;
    RegisterClassExW(&wc);

    // 创建主窗口（1280×760，屏幕居中）
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 1280, wh = 760;
    int wx = (sw - ww) / 2, wy = (sh - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return -1;

    // 默认隐藏到系统托盘，不弹出主界面
    // 用户可双击托盘图标或从右键菜单选择"显示窗口"来打开界面
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    // 消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
