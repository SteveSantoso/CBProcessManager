// ProcessService.h  -  进程生命周期管理
#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <map>
#include <mutex>

// ─── 进程状态枚举 ─────────────────────────────────────────────────────────────
enum class ProcStatus { Stopped, Starting, Running, Restarting, Failed };

inline const char* statusStr(ProcStatus s) {
    switch (s) {
    case ProcStatus::Stopped:    return "stopped";
    case ProcStatus::Starting:   return "starting";
    case ProcStatus::Running:    return "running";
    case ProcStatus::Restarting: return "restarting";
    case ProcStatus::Failed:     return "failed";
    }
    return "stopped";
}

// 单个受管进程的运行时状态
struct ManagedProcess {
    std::string  id;
    HANDLE       hProcess      = INVALID_HANDLE_VALUE;
    DWORD        pid           = 0;
    HANDLE       hWait         = nullptr;   // RegisterWaitForSingleObject 返回的句柄
    HANDLE       hJob          = nullptr;   // Job Object，关闭时级联终止整个进程树
    bool         guardStopped  = false;     // 手动停止标志，置为 true 则不自动重启
    ProcStatus   status        = ProcStatus::Stopped;
};

// 进程退出 PostMessage 所携带的堆分配上下文
struct ProcExitCtx {
    char  id[128];
    DWORD pid;
    DWORD exitCode;
};

// 状态变更 PostMessage 所携带的堆分配上下文
struct ProcStatusMsg {
    std::string id;
    ProcStatus  status;
};

class ProcessService {
public:
    using StatusCallback = std::function<void(const std::string& id, ProcStatus status)>;

    static ProcessService& instance();

    void setMainWindow(HWND hwnd);
    // 状态变更现已通过 WM_APP_STATUS_CHANGED 投递，不再需要回调
    // void setStatusCallback(StatusCallback cb);

    // 按配置 id 启动进程；若设有延迟则在独立线程中等待后启动
    bool startProcess(const std::string& id);

    // 用户主动停止（会禁用守护重启）
    bool stopProcess(const std::string& id);

    void startAll();
    void stopAll();

    // 须在 UI 线程中处理 WM_APP_PROC_EXIT 消息时调用
    void onProcessExited(const std::string& id, DWORD pid, DWORD exitCode);

    ProcStatus getStatus(const std::string& id);
    DWORD      getPid(const std::string& id);   // 进程运行时 PID，未运行返回 0

    // 仅用于 bat 启动后子进程 PID 更新（后台线程调用）
    void refreshChildPid(const std::string& id, DWORD cmdPid);

    // 确保运行时表中存在所有配置项的 ManagedProcess 条目
    void syncConfig();

    HWND mainHwnd() { std::lock_guard<std::mutex> lk(m_mutex); return m_hwnd; }
    std::mutex& mutex() { return m_mutex; }

private:
    ProcessService() = default;
    bool launchNow(const std::string& id);          // 实际调用 CreateProcess
    void notifyStatus(const std::string& id, ProcStatus s);
    void cleanupProcess(ManagedProcess& mp);

    HWND       m_hwnd = nullptr;
    std::mutex m_mutex;
    std::map<std::string, ManagedProcess> m_procs;
};
