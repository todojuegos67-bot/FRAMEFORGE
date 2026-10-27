#include "core/gpu.h"
#include "core/embedded.h"
#include "core/log.h"
#include <cstring>

namespace ff {

bool createTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bindExtra, Tex2D& out,
               const void* init, UINT pitch) {
    out.reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | bindExtra;
    D3D11_SUBRESOURCE_DATA sd{init, pitch, 0};
    if (FAILED(dev->CreateTexture2D(&d, init ? &sd : nullptr, &out.tex))) return false;
    if (FAILED(dev->CreateShaderResourceView(out.tex.Get(), nullptr, &out.srv))) return false;
    if (bindExtra & D3D11_BIND_UNORDERED_ACCESS)
        if (FAILED(dev->CreateUnorderedAccessView(out.tex.Get(), nullptr, &out.uav))) return false;
    if (bindExtra & D3D11_BIND_RENDER_TARGET)
        if (FAILED(dev->CreateRenderTargetView(out.tex.Get(), nullptr, &out.rtv))) return false;
    out.w = w; out.h = h;
    return true;
}

const unsigned char* findEmbedded(const char* name, size_t* size) {
    for (size_t i = 0; i < embedded::fileCount; ++i)
        if (std::strcmp(embedded::files[i].name, name) == 0) {
            if (size) *size = embedded::files[i].size;
            return embedded::files[i].data;
        }
    return nullptr;
}

namespace {
struct EmbeddedInclude : ID3DInclude {
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        const char* base = name;
        for (const char* p = name; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
        size_t sz = 0;
        const unsigned char* d = findEmbedded(base, &sz);
        if (!d) return E_FAIL;
        *data = d; *bytes = (UINT)sz;
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};
}

ComPtr<ID3DBlob> compileEmbedded(const char* file, const char* entry, const char* target, std::string* err,
                                 const D3D_SHADER_MACRO* defines) {
    size_t sz = 0;
    const unsigned char* src = findEmbedded(file, &sz);
    if (!src) { if (err) *err = std::string("shader not embedded: ") + file; return nullptr; }
    EmbeddedInclude inc;
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, sz, file, defines, &inc, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string(file) + ":" + entry + " compile failed";
        if (errors) msg += std::string(": ") + (const char*)errors->GetBufferPointer();
        if (err) *err = msg;
        logf("%s", msg.c_str());
        return nullptr;
    }
    return code;
}

ComPtr<ID3D11ComputeShader> createCS(ID3D11Device* dev, const char* file, const char* entry, std::string* err) {
    auto blob = compileEmbedded(file, entry, "cs_5_0", err);
    if (!blob) return nullptr;
    ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs))) {
        if (err) *err = std::string("CreateComputeShader failed: ") + file;
        return nullptr;
    }
    return cs;
}

bool DynCB::create(ID3D11Device* dev, UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (size + 15) & ~15u; d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, &buf));
}

void DynCB::set(ID3D11DeviceContext* ctx, const void* data, UINT size) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, data, size);
        ctx->Unmap(buf.Get(), 0);
    }
}

bool Samplers::create(ID3D11Device* dev) {
    D3D11_SAMPLER_DESC d{};
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&d, &linear))) return false;
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    return SUCCEEDED(dev->CreateSamplerState(&d, &point));
}

void dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11Buffer* cb, const Samplers& s,
              std::initializer_list<ID3D11ShaderResourceView*> srvs,
              std::initializer_list<ID3D11UnorderedAccessView*> uavs, UINT gx, UINT gy) {
    ID3D11ShaderResourceView* sv[8] = {};
    ID3D11UnorderedAccessView* uv[4] = {};
    UINT ns = 0, nu = 0;
    for (auto* p : srvs) if (ns < 8) sv[ns++] = p;
    for (auto* p : uavs) if (nu < 4) uv[nu++] = p;
    ID3D11SamplerState* ss[2] = {s.linear.Get(), s.point.Get()};
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetSamplers(0, 2, ss);
    ctx->CSSetShaderResources(0, ns, sv);
    ctx->CSSetUnorderedAccessViews(0, nu, uv, nullptr);
    ctx->Dispatch(gx, gy, 1);
    ID3D11ShaderResourceView* nsv[8] = {};
    ID3D11UnorderedAccessView* nuv[4] = {};
    ctx->CSSetShaderResources(0, 8, nsv);
    ctx->CSSetUnorderedAccessViews(0, 4, nuv, nullptr);
}

bool readback(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT bpp, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return false;
    ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize((size_t)d.Width * d.Height * bpp);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(out.data() + (size_t)y * d.Width * bpp, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * bpp);
    ctx->Unmap(st.Get(), 0);
    return true;
}

double nowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
}
