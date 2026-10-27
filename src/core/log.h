#pragma once
#include <string>
namespace ff {
void logInit(const std::wstring& path);
void logf(const char* fmt, ...);
std::wstring exeDir();
}
