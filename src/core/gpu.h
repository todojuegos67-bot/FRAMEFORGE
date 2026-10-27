#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ff {
using Microsoft::WRL::ComPtr;

struct Tex2D {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT w = 0, h = 0;
    void reset() { tex.Reset(); srv.Reset(); uav.Reset(); rtv.Reset(); w = h = 0; }
};

// Always creates an SRV; bindExtra may add D3D11_BIND_UNORDERED_ACCESS / D3D11_BIND_RENDER_TARGET.
bool createTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bindExtra, Tex2D& out,
               const void* init = nullptr, UINT pitch = 0);

const unsigned char* findEmbedded(const char* name, size_t* size);
ComPtr<ID3DBlob> compileEmbedded(const char* file, const char* entry, const char* target, std::string* err,
                                 const D3D_SHADER_MACRO* defines = nullptr);
ComPtr<ID3D11ComputeShader> createCS(ID3D11Device* dev, const char* file, const char* entry, std::string* err);

struct DynCB {
    ComPtr<ID3D11Buffer> buf;
    bool create(ID3D11Device* dev, UINT size);
    void set(ID3D11DeviceContext* ctx, const void* data, UINT size);
};

struct Samplers {
    ComPtr<ID3D11SamplerState> linear, point;
    bool create(ID3D11Device* dev);
};

// Dispatch helper: binds, dispatches and unbinds.
void dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11Buffer* cb, const Samplers& s,
              std::initializer_list<ID3D11ShaderResourceView*> srvs,
              std::initializer_list<ID3D11UnorderedAccessView*> uavs, UINT gx, UINT gy);

// CPU readback (tests / debugging). Returns tightly packed rows of w*bpp bytes.
bool readback(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT bpp, std::vector<uint8_t>& out);

double nowSeconds();
inline UINT divUp(UINT a, UINT b) { return (a + b - 1) / b; }
}
