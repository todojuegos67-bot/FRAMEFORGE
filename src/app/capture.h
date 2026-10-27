#pragma once
#include <d3d11.h>
#include <memory>
#include <string>

namespace ff {
struct CapturedFrame {
    ID3D11Texture2D* tex = nullptr;  // valid until release()/next acquire()
    UINT w = 0, h = 0;               // content size
    double sysTime = 0;              // SystemRelativeTime in seconds (QPC based)
};

// Windows Graphics Capture of a single window: no cursor, no yellow border (when the OS allows),
// minimal update interval (when the OS exposes it), free-threaded pool, 2 buffers.
class WgcCapture {
public:
    WgcCapture();
    ~WgcCapture();
    static bool supported();
    bool start(ID3D11Device* dev, HWND hwnd, std::string* err);
    void stop();
    bool running() const;
    bool closed() const;
    HANDLE event() const;
    bool acquire(CapturedFrame& out);
    void release();
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
}
