#pragma once
#include <cstddef>
namespace ff::embedded {
struct File { const char* name; const unsigned char* data; size_t size; };
extern const File files[];
extern const size_t fileCount;
}
