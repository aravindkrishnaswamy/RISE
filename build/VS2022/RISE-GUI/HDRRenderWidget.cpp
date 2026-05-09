//////////////////////////////////////////////////////////////////////
//
//  HDRRenderWidget.cpp — Windows HDR display widget (D3D11 + DXGI).
//
//  Lazy-creates a DXGI swap chain on the widget's native HWND when
//  the first frame arrives.  See header for the architectural
//  rationale (L5b Path A — scRGB).
//
//////////////////////////////////////////////////////////////////////

#include "HDRRenderWidget.h"

#include <QEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QWindow>
#include <QDebug>

#if defined(_WIN32)
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================
// HLSL: fullscreen-quad blit with aspect-fit viewport letterbox.
// ============================================================
//
// The constant buffer carries the source UV scale (used to flip Y
// because we want top-left origin) and a "scale" pair that maps
// the [-1, 1] NDC quad to the aspect-fit subrectangle of the swap
// chain.  Letterboxed background is the swap chain clear value
// (opaque black).  Source format is binary16 RGBA in linear sRGB
// primaries; output format is the same.  The compositor handles
// scRGB → display-native tone mapping.
//
#if defined(_WIN32)
namespace {

const char* kHDRBlitVertexShader = R"(
cbuffer CB : register(b0)
{
    float2 ScalePos;   // NDC scale  (xScale, yScale)  from CPU
    float2 OffsetPos;  // NDC offset (xOffset, yOffset)
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    // Quad corners in [0,1]:  (0,0) (1,0) (0,1) (1,1)
    float2 uv = float2( (vid & 1u) ? 1.0 : 0.0,
                         (vid & 2u) ? 1.0 : 0.0 );
    // Map [0,1] -> [-1,1] then apply aspect-fit scale + center.
    float2 ndc = (uv * 2.0 - 1.0) * ScalePos + OffsetPos;
    VSOut o;
    o.pos = float4(ndc.x, -ndc.y, 0.0, 1.0);  // flip Y so source row 0 is at top
    o.uv  = uv;
    return o;
}
)";

const char* kHDRBlitPixelShader = R"(
Texture2D    src     : register(t0);
SamplerState linearS : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    // Linear sRGB primaries in / out — no transfer change.
    return src.Sample(linearS, uv);
}
)";

struct CBData {
    float scalePos[2];
    float offsetPos[2];
};

} // anonymous namespace
#endif // _WIN32

// ============================================================
// HDRRenderWidget
// ============================================================

HDRRenderWidget::HDRRenderWidget(QWidget* parent)
    : QWidget(parent)
{
    // Tell Qt we own the painting — no double-buffering, no native
    // paint engine.  setAttribute(WA_NativeWindow) forces a winId()
    // backing HWND; setAttribute(WA_PaintOnScreen) bypasses Qt's
    // backing store entirely so DXGI's flip-model swap chain
    // composites directly to the window.
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NativeWindow, true);
}

HDRRenderWidget::~HDRRenderWidget() = default;

// ------------------------------------------------------------
// Static HDR-availability probe (no swap chain required)
// ------------------------------------------------------------

bool HDRRenderWidget::probeAnyAdapterHDRAvailable()
{
#if defined(_WIN32)
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(a, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, output.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) continue;

            DXGI_OUTPUT_DESC1 desc = {};
            if (FAILED(output6->GetDesc1(&desc))) continue;

            // Same gate as refreshHDRAvailability (HDR10 colorspace
            // + > SDR luminance).  ANY attached output qualifying
            // is enough — at-startup we don't yet know which monitor
            // the user will drag the window to.
            if (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
             && desc.MaxLuminance > 100.0f) {
                return true;
            }
        }
    }
#endif
    return false;
}

// ------------------------------------------------------------
// Qt slots
// ------------------------------------------------------------

void HDRRenderWidget::updateHDRImage(const QByteArray& halfFloats, int W, int H)
{
    // Empty payload: the engine signalled scene-clear.  Drop the
    // latched frame; subsequent paints will clear-to-black.
    if (halfFloats.isEmpty() || W <= 0 || H <= 0) {
        m_lastFrame.clear();
        m_srcW = 0;
        m_srcH = 0;
        update();
        return;
    }

    m_lastFrame = halfFloats;  // detach-on-write keeps this stable
    m_srcW = W;
    m_srcH = H;

#if defined(_WIN32)
    if (!ensureDevice()) {
        // Init failed — silently drop.  ensureDevice already logged.
        return;
    }
    presentFrame(halfFloats, W, H);
#else
    update();
#endif
}

// ------------------------------------------------------------
// Qt event overrides
// ------------------------------------------------------------

void HDRRenderWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
#if defined(_WIN32)
    if (m_swapChain) {
        resizeBackBuffer();
        // Re-Present the latched frame so the resized swap chain
        // shows current content immediately (otherwise the back
        // buffer is empty until the next rasterizer tile lands).
        if (!m_lastFrame.isEmpty() && m_srcW > 0 && m_srcH > 0) {
            presentFrame(m_lastFrame, m_srcW, m_srcH);
        }
    }
#endif
}

void HDRRenderWidget::paintEvent(QPaintEvent* /*event*/)
{
#if defined(_WIN32)
    if (m_swapChain && !m_lastFrame.isEmpty() && m_srcW > 0 && m_srcH > 0) {
        presentFrame(m_lastFrame, m_srcW, m_srcH);
    }
#endif
}

bool HDRRenderWidget::event(QEvent* ev)
{
    // The widget's QWindow emits screenChanged when dragged between
    // displays.  We can't override `screenChanged` directly on a
    // QWidget (it's a QWindow signal), so we hook into ScreenChange-
    // Internal at the QEvent layer.  refreshHDRAvailability re-probes
    // IDXGIOutput6 for the new monitor and emits the toggle update.
    //
    // We force `ensureDevice` on Show events too: without a swap
    // chain, refreshHDRAvailability can't query the active monitor's
    // colorspace, so the View > HDR Preview toggle would stay greyed
    // out until the first frame arrives — which by then is too late
    // for the user to flip into HDR before render starts.
    if (ev->type() == QEvent::ScreenChangeInternal
     || ev->type() == QEvent::WindowStateChange
     || ev->type() == QEvent::Show) {
#if defined(_WIN32)
        ensureDevice();           // creates swap chain on first show
        refreshHDRAvailability(); // emits hdrAvailabilityChanged on transition
#endif
    }
    return QWidget::event(ev);
}

// ============================================================
// Windows-only D3D11 backend
// ============================================================
#if defined(_WIN32)

bool HDRRenderWidget::ensureDevice()
{
    if (m_swapChain) return true;
    if (m_initFailed) return false;

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        // winId is realised lazily; first call may run before the
        // widget is shown.  Return without setting m_initFailed so
        // a subsequent attempt (after show) can succeed.
        return false;
    }

    HRESULT hr = S_OK;

    // --- Device + immediate context --------------------------------
    UINT flags = 0;
#ifdef _DEBUG
    // BGRA support (for D2D interop, not currently used) + debug
    // layer.  D3D11_CREATE_DEVICE_DEBUG fails on machines without
    // the D3D11 debug layer installed; fall back silently.
    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
#else
    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#endif

    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = D3D11CreateDevice(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // no software module
        flags,
        requestedLevels, ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        m_device.GetAddressOf(),
        &chosenLevel,
        m_context.GetAddressOf());

    if (FAILED(hr)) {
        // Retry without the debug flag.
#ifdef _DEBUG
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, requestedLevels, ARRAYSIZE(requestedLevels),
            D3D11_SDK_VERSION, m_device.GetAddressOf(),
            &chosenLevel, m_context.GetAddressOf());
#endif
        if (FAILED(hr)) {
            qWarning() << "HDRRenderWidget: D3D11CreateDevice failed, hr=0x"
                       << QString::number(hr, 16);
            m_initFailed = true;
            return false;
        }
    }

    // --- DXGI factory + swap chain ---------------------------------
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) { m_initFailed = true; return false; }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) { m_initFailed = true; return false; }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width        = std::max(1, width()  * static_cast<int>(devicePixelRatioF()));
    scDesc.Height       = std::max(1, height() * static_cast<int>(devicePixelRatioF()));
    scDesc.Format       = DXGI_FORMAT_R16G16B16A16_FLOAT;  // scRGB
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount  = 2;
    scDesc.Scaling      = DXGI_SCALING_NONE;
    scDesc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode    = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(
        m_device.Get(), hwnd, &scDesc, nullptr, nullptr,
        swapChain1.GetAddressOf());
    if (FAILED(hr)) {
        qWarning() << "HDRRenderWidget: CreateSwapChainForHwnd failed, hr=0x"
                   << QString::number(hr, 16);
        m_initFailed = true;
        return false;
    }

    // Disable Alt-Enter fullscreen — RISE-GUI manages window state.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Promote to IDXGISwapChain3 for SetColorSpace1.
    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        qWarning() << "HDRRenderWidget: IDXGISwapChain3 not supported (need Win10 1709+).";
        m_initFailed = true;
        return false;
    }

    // scRGB linear (Path A — same buffer math as macOS EDR).
    UINT colorSpaceSupport = 0;
    hr = m_swapChain->CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &colorSpaceSupport);
    if (SUCCEEDED(hr)
     && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        hr = m_swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        if (FAILED(hr)) {
            qWarning() << "HDRRenderWidget: SetColorSpace1(scRGB) failed, hr=0x"
                       << QString::number(hr, 16);
        }
    } else {
        qWarning() << "HDRRenderWidget: scRGB color space not supported on this device.";
    }

    // --- RTV for the back buffer -----------------------------------
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) { m_initFailed = true; return false; }
    hr = m_device->CreateRenderTargetView(
        backBuffer.Get(), nullptr, m_rtv.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }

    // --- Shaders ---------------------------------------------------
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompile(kHDRBlitVertexShader, std::strlen(kHDRBlitVertexShader),
        "HDRBlitVS", nullptr, nullptr, "main", "vs_5_0",
        0, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errBlob) qWarning() << "HDR VS compile:" << static_cast<const char*>(errBlob->GetBufferPointer());
        m_initFailed = true;
        return false;
    }
    hr = D3DCompile(kHDRBlitPixelShader, std::strlen(kHDRBlitPixelShader),
        "HDRBlitPS", nullptr, nullptr, "main", "ps_5_0",
        0, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errBlob) qWarning() << "HDR PS compile:" << static_cast<const char*>(errBlob->GetBufferPointer());
        m_initFailed = true;
        return false;
    }
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }

    // --- Sampler ---------------------------------------------------
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&samp, m_sampler.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }

    // --- Constant buffer (16 B, 4 floats) --------------------------
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CBData);
    cbDesc.Usage     = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf());
    if (FAILED(hr)) { m_initFailed = true; return false; }

    // Initial HDR availability probe.
    refreshHDRAvailability();

    return true;
}

void HDRRenderWidget::refreshHDRAvailability()
{
    if (!m_swapChain) return;

    bool nowAvailable = false;
    float maxLuminance = 0.0f;

    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(m_swapChain->GetContainingOutput(output.GetAddressOf()))) {
        ComPtr<IDXGIOutput6> output6;
        if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 desc = {};
            if (SUCCEEDED(output6->GetDesc1(&desc))) {
                maxLuminance = desc.MaxLuminance;
                // HDR-capable when the OS reports BT.2020 PQ output
                // colorspace AND the monitor advertises >100-nit max
                // luminance.  Some SDR monitors return P709 even
                // when "HDR" is enabled in Windows settings; the
                // luminance check filters those.
                nowAvailable = (desc.ColorSpace
                              == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                            && (desc.MaxLuminance > 100.0f);
            }
        }
    }

    m_displayMaxLuminance = maxLuminance;
    if (nowAvailable != m_hdrAvailable) {
        m_hdrAvailable = nowAvailable;
        emit hdrAvailabilityChanged(m_hdrAvailable);
    }
}

void HDRRenderWidget::resizeBackBuffer()
{
    if (!m_swapChain) return;
    m_rtv.Reset();

    const float dpr = static_cast<float>(devicePixelRatioF());
    UINT w = static_cast<UINT>(std::max(1.0f, width()  * dpr));
    UINT h = static_cast<UINT>(std::max(1.0f, height() * dpr));

    HRESULT hr = m_swapChain->ResizeBuffers(
        0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        qWarning() << "HDRRenderWidget: ResizeBuffers failed, hr=0x"
                   << QString::number(hr, 16);
        return;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) return;
    m_device->CreateRenderTargetView(
        backBuffer.Get(), nullptr, m_rtv.GetAddressOf());
}

void HDRRenderWidget::presentFrame(const QByteArray& halfFloats, int W, int H)
{
    if (!m_swapChain || !m_rtv) return;

    const size_t expectedBytes = static_cast<size_t>(W) * H * 4 * sizeof(uint16_t);
    if (static_cast<size_t>(halfFloats.size()) != expectedBytes) {
        qWarning() << "HDRRenderWidget: payload size mismatch:"
                   << halfFloats.size() << "vs expected" << expectedBytes;
        return;
    }

    // --- Source texture (recreate if dims change) ------------------
    if (W != m_srcTexW || H != m_srcTexH || !m_srcTex) {
        m_srcSRV.Reset();
        m_srcTex.Reset();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width      = static_cast<UINT>(W);
        td.Height     = static_cast<UINT>(H);
        td.MipLevels  = 1;
        td.ArraySize  = 1;
        td.Format     = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage      = D3D11_USAGE_DYNAMIC;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_srcTex.GetAddressOf()))) {
            return;
        }
        if (FAILED(m_device->CreateShaderResourceView(
                m_srcTex.Get(), nullptr, m_srcSRV.GetAddressOf()))) {
            return;
        }
        m_srcTexW = W;
        m_srcTexH = H;
    }

    // --- Map / memcpy / Unmap source texture -----------------------
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_srcTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(halfFloats.constData());
    uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
    const size_t srcRowBytes = static_cast<size_t>(W) * 4 * sizeof(uint16_t);
    for (int y = 0; y < H; ++y) {
        std::memcpy(dst + y * mapped.RowPitch,
                    src + y * srcRowBytes,
                    srcRowBytes);
    }
    m_context->Unmap(m_srcTex.Get(), 0);

    // --- Letterboxed viewport -------------------------------------
    const float dpr = static_cast<float>(devicePixelRatioF());
    const float dstWf = std::max(1.0f, width()  * dpr);
    const float dstHf = std::max(1.0f, height() * dpr);
    const float srcAspect = static_cast<float>(W) / static_cast<float>(H);
    const float dstAspect = dstWf / dstHf;

    // Aspect-fit: shrink along the dimension that's "too long".
    float fitW = dstWf, fitH = dstHf;
    if (srcAspect > dstAspect) {
        // Source wider — fit to width.
        fitH = dstWf / srcAspect;
    } else {
        // Source taller — fit to height.
        fitW = dstHf * srcAspect;
    }
    const float xScale = fitW / dstWf;
    const float yScale = fitH / dstHf;

    // --- Update constant buffer -----------------------------------
    D3D11_MAPPED_SUBRESOURCE cbMap = {};
    if (FAILED(m_context->Map(m_constantBuffer.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &cbMap))) {
        return;
    }
    CBData cb = {};
    cb.scalePos[0]  = xScale;
    cb.scalePos[1]  = yScale;
    cb.offsetPos[0] = 0.0f;
    cb.offsetPos[1] = 0.0f;
    std::memcpy(cbMap.pData, &cb, sizeof(cb));
    m_context->Unmap(m_constantBuffer.Get(), 0);

    // --- Clear + draw ---------------------------------------------
    const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);

    D3D11_VIEWPORT vp = {};
    vp.Width    = dstWf;
    vp.Height   = dstHf;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, m_srcSRV.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->Draw(4, 0);

    m_swapChain->Present(0, 0);
}

#endif // _WIN32
