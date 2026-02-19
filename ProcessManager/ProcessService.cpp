// ProcessService.cpp  -  进程生命周期管理
#include "ProcessService.h"
#include "ConfigService.h"
#include "Logger.h"
#include "resource.h"
#include <winbase.h>            // RegisterWaitForSingleObject、UnregisterWaitEx
#ifndef WT_EXECUTEONCE
#define WT_EXECUTEONCE 0x00000008
#endif
#include <tlhelp32.h>           // CreateToolhelp32Snapshot、PROCESSENTRY32W
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>

// ─── 单例 ─────────────────────────────────────────────────────────────────────
ProcessService& ProcessService::instance() {
    static ProcessService inst;
    return inst;
}

void ProcessService::setMainWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_hwnd = hwnd;
}

// ─── 线程池等待回调（在线程池线程中执行）──────────────────────────────────────
static void CALLBACK WaitCallback(PVOID lpParam, BOOLEAN /*timedOut*/) {
    ProcExitCtx* ctx = reinterpret_cast<ProcExitCtx*>(lpParam);

    // 获取退出码（hProcess 在 cleanupProcess 关闭句柄前保持有效）
    {
        std::lock_guard<std::mutex> lk(ProcessService::instance().mutex());
    }

    HWND hwnd = ProcessService::instance().mainHwnd();
    if (hwnd) {
        PostMessage(hwnd, WM_APP_PROC_EXIT, 0, (LPARAM)ctx);
    } else {
        delete ctx;
    }
}

// ─── 同步配置（将配置中的进程补充到运行时表）──────────────────────────────────
void ProcessService::syncConfig() {
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto& procs = ConfigService::instance().config().processes;
    for (const auto& pc : procs) {
        if (m_procs.find(pc.id) == m_procs.end()) {
            ManagedProcess mp;
            mp.id = pc.id;
            mp.status = ProcStatus::Stopped;
            m_procs[pc.id] = std::move(mp);
        }
    }
}

// ─── 查询进程当前状态 ────────────────────────────────────────────────────────
ProcStatus ProcessService::getStatus(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_procs.find(id);
    if (it == m_procs.end()) return ProcStatus::Stopped;
    return it->second.status;
}
// ─── 查询进程 PID ─────────────────────────────────────────────────────────────────────────
DWORD ProcessService::getPid(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_procs.find(id);
    if (it == m_procs.end()) return 0;
    return it->second.pid;
}

// ─── 刷新 bat 子进程 PID（bat 启动后由后台线程调用）────────────────────────────
// 枚举系统快照，找到 cmdPid 的第一个直接子进程（跳过 conhost.exe 等辅助进程）
// 若找到则更新 mp.pid 并通知前端刷新显示
void ProcessService::refreshChildPid(const std::string& id, DWORD cmdPid) {
    // 等待子进程启动，最多重试 5 次，每次间隔 1.5 秒
    DWORD childPid = 0;
    for (int i = 0; i < 5 && childPid == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ParentProcessID != cmdPid) continue;
                if (pe.th32ProcessID == cmdPid)      continue;
                // 跳过 Windows 辅助进程，找真正的业务子进程
                std::wstring name(pe.szExeFile);
                if (name == L"conhost.exe" || name == L"WerFault.exe") continue;
                childPid = pe.th32ProcessID;
                break;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (childPid == 0) return;   // 未找到子进程，保留 cmd.exe PID

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_procs.find(id);
        // 仅当 PID 仍为原始 cmd.exe PID 时才更新（避免进程已停止后误写）
        if (it == m_procs.end() || it->second.pid != cmdPid) return;
        it->second.pid = childPid;
    }
    // 推送状态更新，让前端刷新 PID 显示
    notifyStatus(id, ProcStatus::Running);    pmLogF(L"[进程] %-20S  子进程 PID 更新: %lu",
        id.c_str(), (unsigned long)childPid);}
// ─── 通知状态变更 ─────────────────────────────────────────────────────────────
// 线程安全：向主窗口投递 WM_APP_STATUS_CHANGED 消息，可在任意线程调用。
void ProcessService::notifyStatus(const std::string& id, ProcStatus s) {
    HWND hwnd = mainHwnd();
    if (!hwnd) return;
    ProcStatusMsg* msg = new ProcStatusMsg{ id, s };
    PostMessage(hwnd, WM_APP_STATUS_CHANGED, 0, (LPARAM)msg);
}

// ─── 清理进程资源（调用时必须持有 m_mutex）──────────────────────────────────
void ProcessService::cleanupProcess(ManagedProcess& mp) {
    if (mp.hWait) {
        UnregisterWaitEx(mp.hWait, INVALID_HANDLE_VALUE);
        mp.hWait = nullptr;
    }
    // 关闭 Job 句柄（若 stopProcess 已提前关闭则此处为 nullptr，安全跳过）
    if (mp.hJob != nullptr) {
        CloseHandle(mp.hJob);
        mp.hJob = nullptr;
    }
    if (mp.hProcess != INVALID_HANDLE_VALUE && mp.hProcess != nullptr) {
        CloseHandle(mp.hProcess);
        mp.hProcess = INVALID_HANDLE_VALUE;
    }
    mp.pid = 0;
}

// ─── 立即启动进程（可在任意线程调用）──────────────────────────────────────────
bool ProcessService::launchNow(const std::string& id) {
    // 查找进程配置
    ProcessConfig cfg;
    {
        const auto& procs = ConfigService::instance().config().processes;
        auto it = std::find_if(procs.begin(), procs.end(),
            [&](const ProcessConfig& p) { return p.id == id; });
        if (it == procs.end()) return false;
        cfg = *it;
    }

    // 构建命令行字符串
    std::wstring cmdLine;
    {
        // 将路径和参数从 UTF-8 转换为宽字符
        int pathLen = MultiByteToWideChar(CP_UTF8, 0, cfg.path.c_str(), -1, nullptr, 0);
        std::wstring wpath(pathLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cfg.path.c_str(), -1, wpath.data(), pathLen);
        wpath.resize(wcslen(wpath.c_str()));

        int argsLen = MultiByteToWideChar(CP_UTF8, 0, cfg.args.c_str(), -1, nullptr, 0);
        std::wstring wargs(argsLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cfg.args.c_str(), -1, wargs.data(), argsLen);
        wargs.resize(wcslen(wargs.c_str()));

        if (cfg.type == "bat") {
            // cmd /c 执行 bat 时用双引号嵌套，确保路径含空格也能正确解析
            cmdLine = L"cmd.exe /c \"\"" + wpath + L"\"\"";
            if (!wargs.empty()) cmdLine += L" " + wargs;
        } else {
            cmdLine = L"\"" + wpath + L"\"";
            if (!wargs.empty()) cmdLine += L" " + wargs;
        }
    }

    // 提取 bat/exe 所在目录作为工作目录，确保相对路径能正确解析
    std::wstring workDir;
    {
        int pathLen2 = MultiByteToWideChar(CP_UTF8, 0, cfg.path.c_str(), -1, nullptr, 0);
        std::wstring wpath2(pathLen2, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cfg.path.c_str(), -1, wpath2.data(), pathLen2);
        wpath2.resize(wcslen(wpath2.c_str()));
        auto pos = wpath2.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            workDir = wpath2.substr(0, pos);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    // CREATE_SUSPENDED：先挂起进程，将其加入 Job Object 后再恢复，确保子进程也在 Job 内
    pmLogF(L"[进程] %-20S  正在启动  cmdLine=%s", id.c_str(), cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(),
        nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),  // 工作目录设为 bat/exe 所在目录
        &si, &pi);

    if (!ok) {
        DWORD err = GetLastError();
        // 用 FormatMessageW 把错误码转成系统描述文字，方便非开发人员阅读日志
        wchar_t* errMsg = nullptr;
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, err, MAKELANGID(LANG_CHINESE_SIMPLIFIED, SUBLANG_CHINESE_SIMPLIFIED),
            reinterpret_cast<LPWSTR>(&errMsg), 0, nullptr);
        // 若简体中文消息获取失败，降级到系统默认语言
        if (!errMsg) {
            FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, err, 0,
                reinterpret_cast<LPWSTR>(&errMsg), 0, nullptr);
        }
        // 去掉末尾的换行符
        if (errMsg) {
            for (wchar_t* p = errMsg + wcslen(errMsg) - 1;
                 p >= errMsg && (*p == L'\r' || *p == L'\n'); --p) *p = L'\0';
        }
        pmLogF(L"[进程] %-20S  启动失败  错误码=%lu  原因：%s",
            id.c_str(), (unsigned long)err, errMsg ? errMsg : L"未知错误");
        if (errMsg) LocalFree(errMsg);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_procs[id].status = ProcStatus::Failed;
        }
        notifyStatus(id, ProcStatus::Failed);
        return false;
    }

    // 创建 Job Object，设置 KILL_ON_JOB_CLOSE
    // 关闭 hJob 句柄时，Job 内所有进程（含 bat 启动的子进程）将被级联终止
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    // 加入 Job 后恢复进程运行
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // 为线程池回调分配进程退出上下文
    ProcExitCtx* ctx = new ProcExitCtx{};
    strncpy_s(ctx->id, sizeof(ctx->id), id.c_str(), _TRUNCATE);
    ctx->pid = pi.dwProcessId;

    HANDLE hWait = nullptr;
    RegisterWaitForSingleObject(
        &hWait, pi.hProcess, WaitCallback,
        ctx, INFINITE, WT_EXECUTEONCE);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto& mp = m_procs[id];
        cleanupProcess(mp);   // 清理上一次的句柄
        mp.hProcess     = pi.hProcess;
        mp.pid          = pi.dwProcessId;
        mp.hWait        = hWait;
        mp.hJob         = hJob;   // 保存 Job 句柄，停止时用于级联终止进程树
        mp.guardStopped = false;
        mp.status       = ProcStatus::Running;  // 标记为运行中
    }

    notifyStatus(id, ProcStatus::Running);
    pmLogF(L"[进程] %-20S  已启动  PID=%lu",
        id.c_str(), (unsigned long)pi.dwProcessId);

    // bat 文件：cmd.exe PID 对用户无意义，后台探测真正的子进程 PID 并更新显示
    if (cfg.type == "bat") {
        DWORD cmdPid = pi.dwProcessId;
        std::thread([this, id, cmdPid]() {
            refreshChildPid(id, cmdPid);
        }).detach();
    }

    return true;
}

// ─── 启动进程 ────────────────────────────────────────────────────────────────
bool ProcessService::startProcess(const std::string& id) {
    // 查找进程配置，获取延迟秒数
    int delay = 0;
    {
        const auto& procs = ConfigService::instance().config().processes;
        auto it = std::find_if(procs.begin(), procs.end(),
            [&](const ProcessConfig& p) { return p.id == id; });
        if (it == procs.end()) return false;
        delay = it->delaySeconds;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto& mp = m_procs[id];
        if (mp.status == ProcStatus::Running || mp.status == ProcStatus::Starting)
            return false;
        mp.guardStopped = false;
        mp.status = ProcStatus::Starting;
    }
    if (delay > 0)
        pmLogF(L"[进程] %-20S  准备启动（延迟 %d 秒）", id.c_str(), delay);
    else
        pmLogF(L"[进程] %-20S  准备启动", id.c_str());
    notifyStatus(id, ProcStatus::Starting);

    if (delay > 0) {
        std::thread([this, id, delay]() {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            // 重新检查：用户是否已主动停止
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_procs.find(id);
                if (it == m_procs.end()) return;
                if (it->second.guardStopped) {
                    it->second.status = ProcStatus::Stopped;
                    return;
                }
            }
            launchNow(id);
        }).detach();
    } else {
        launchNow(id);
    }
    return true;
}

// ─── 停止进程 ────────────────────────────────────────────────────────────────
bool ProcessService::stopProcess(const std::string& id) {
    HANDLE hProc = INVALID_HANDLE_VALUE;
    HANDLE hJob  = nullptr;
    DWORD  curPid = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_procs.find(id);
        if (it == m_procs.end()) return false;
        it->second.guardStopped = true;
        hProc  = it->second.hProcess;
        curPid = it->second.pid;
        // 转移 hJob 所有权：本函数负责关闭，cleanupProcess 不会重复关闭
        hJob  = it->second.hJob;
        it->second.hJob = nullptr;
    }
    pmLogF(L"[进程] %-20S  用户停止  PID=%lu", id.c_str(), (unsigned long)curPid);
    if (hJob != nullptr) {
        // 关闭 Job 句柄 → KILL_ON_JOB_CLOSE 触发
        // 整个进程树（cmd.exe 及其所有子进程）被系统级联终止
        CloseHandle(hJob);
    }
    if (hProc != INVALID_HANDLE_VALUE && hProc != nullptr) {
        // 额外对根进程发送终止信号，保证快速退出
        TerminateProcess(hProc, 0);
        // 进程退出后会在 onProcessExited 中自动调用 cleanupProcess
    } else {
        // 进程已不在运行，直接标记为已停止
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_procs[id].status = ProcStatus::Stopped;
        }
        notifyStatus(id, ProcStatus::Stopped);
    }
    return true;
}

// ─── 全部启动 / 全部停止 ─────────────────────────────────────────────────────
void ProcessService::startAll() {
    syncConfig();
    std::vector<std::string> ids;
    {
        const auto& procs = ConfigService::instance().config().processes;
        for (const auto& pc : procs)
            if (pc.enabled) ids.push_back(pc.id);
    }
    for (const auto& id : ids) startProcess(id);
}

void ProcessService::stopAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& [id, mp] : m_procs) ids.push_back(id);
    }
    for (const auto& id : ids) stopProcess(id);
}

// ─── 进程退出处理（由 WM_APP_PROC_EXIT 在 UI 线程中触发）────────────────────
void ProcessService::onProcessExited(const std::string& id, DWORD pid, DWORD exitCode) {
    bool shouldRestart = false;
    int  guardDelay    = 3;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_procs.find(id);
        if (it == m_procs.end()) return;

        ManagedProcess& mp = it->second;

        // 尝试从进程句柄获取真实退出码
        if (mp.hProcess != INVALID_HANDLE_VALUE) {
            DWORD code = 0;
            if (GetExitCodeProcess(mp.hProcess, &code)) exitCode = code;
        }
        cleanupProcess(mp);

        if (!mp.guardStopped) {
            // 检查是否启用了进程守护
            const auto& procs = ConfigService::instance().config().processes;
            auto cit = std::find_if(procs.begin(), procs.end(),
                [&](const ProcessConfig& p) { return p.id == id; });
            if (cit != procs.end() && cit->guardEnabled) {
                shouldRestart = true;
                guardDelay    = cit->guardDelaySeconds;
                mp.status     = ProcStatus::Restarting;
            } else {
                mp.status = ProcStatus::Stopped;
            }
        } else {
            mp.status = ProcStatus::Stopped;
        }
    }

    if (shouldRestart) {
        pmLogF(L"[进程] %-20S  异常退出 (code=%lu)，%d 秒后守护重启",
            id.c_str(), (unsigned long)exitCode, guardDelay);
    } else {
        pmLogF(L"[进程] %-20S  已退出  exitCode=%lu",
            id.c_str(), (unsigned long)exitCode);
    }

    if (shouldRestart) {
        notifyStatus(id, ProcStatus::Restarting);
        // 等待 guardDelay 秒后重启进程
        std::thread([this, id, guardDelay]() {
            std::this_thread::sleep_for(std::chrono::seconds(guardDelay));
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_procs.find(id);
                if (it == m_procs.end()) return;
                if (it->second.guardStopped) {
                    it->second.status = ProcStatus::Stopped;
                    cancelled = true;
                }
            }
            if (cancelled) { notifyStatus(id, ProcStatus::Stopped); return; }
            launchNow(id);
        }).detach();
    } else {
        notifyStatus(id, ProcStatus::Stopped);
    }
}
