// Logger.h  -  进程管理器日志模块（header-only）
// 每次程序启动在 exe目录/logs/ 下创建一个带时间戳的日志文件
// 可在任意 .cpp 中直接 #include "Logger.h" 后调用 appLog / appLogF
#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "shlwapi.lib")

namespace AppLogger {

// 返回本次运行对应的日志文件完整路径，首次调用时创建 logs/ 目录
inline const wchar_t* logFilePath() {
    static wchar_t s_path[MAX_PATH] = {};
    if (s_path[0] != L'\0') return s_path;

    // 获取 exe 所在目录
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    // 创建 logs 子目录（已存在时静默跳过）
    wchar_t logsDir[MAX_PATH] = {};
    wcscpy_s(logsDir, exeDir);
    PathAppendW(logsDir, L"logs");
    CreateDirectoryW(logsDir, nullptr);

    // 文件名：pm_YYYYMMDD_HHMMSS.log
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t fileName[64] = {};
    swprintf_s(fileName, L"pm_%04d%02d%02d_%02d%02d%02d.log",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    wcscpy_s(s_path, logsDir);
    PathAppendW(s_path, fileName);
    return s_path;
}

// 写一条日志（带时间戳前缀）
inline void appLog(const wchar_t* msg) {
    // 时间戳
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t line[2048] = {};
    swprintf_s(line, L"[%02d:%02d:%02d] %s",
        st.wHour, st.wMinute, st.wSecond, msg);

    FILE* f = nullptr;
    if (_wfopen_s(&f, logFilePath(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", line);
        fclose(f);
    }
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
}

// 格式化写日志
inline void appLogF(const wchar_t* fmt, ...) {
    wchar_t buf[2048] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    appLog(buf);
}

// 初始化日志（写入首行分隔符，确保 logs/ 目录在程序启动时就创建好）
inline void init() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t header[256] = {};
    swprintf_s(header,
        L"════════ 程序启动  %04d-%02d-%02d %02d:%02d:%02d ════════",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    appLog(header);
}

} // namespace AppLogger

// 便捷宏，保持调用简洁
#define pmLog(msg)       AppLogger::appLog(msg)
#define pmLogF(fmt, ...) AppLogger::appLogF(fmt, __VA_ARGS__)
