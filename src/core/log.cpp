#include "core/log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ff {
static std::mutex g_mx;
static FILE* g_f = nullptr;

void logInit(const std::wstring& path) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_f) fclose(g_f);
    g_f = _wfopen(path.c_str(), L"w");
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    std::lock_guard<std::mutex> l(g_mx);
    char line[2200];
    snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    OutputDebugStringA(line);
    if (g_f) { fputs(line, g_f); fflush(g_f); }
}

std::wstring exeDir() {
    wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
}
