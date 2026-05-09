//////////////////////////////////////////////////////////////////////
//
//  HDRRenderWidget.h - Windows HDR display widget for the Qt RISE-GUI.
//
//  L5b Path A (scRGB) — owns a native HWND with a DXGI swap chain
//  configured at `DXGI_FORMAT_R16G16B16A16_FLOAT` +
//  `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (linear scRGB; values
//  in linear sRGB primaries scaled so 1.0 = 80 nits, can extend
//  past 1.0 for HDR highlights).  The OS compositor handles the
//  display-side tone map.  Identical buffer math to the macOS EDR
//  path (`RGBA16F_ExtendedLinearSRGB`); the only platform-specific
//  surface is this widget + the swap chain.
//
//  Drives the same `RGBA16F_ExtendedLinearSRGB` TargetFormat through
//  RenderEngine — so the rasterizer / FrameStore / VFS layers are
//  unchanged.  The widget consumes binary16 RGBA half-floats from
//  RenderEngine's `hdrImageUpdated` signal, copies them into a
//  staging texture, and Presents.
//
//  Headroom detection: queries `IDXGIOutput6::GetDesc1` on screen
//  change.  Emits `hdrAvailabilityChanged` when the active monitor
//  flips between HDR-capable and SDR; MainWindow uses this to enable
//  / disable the HDR toggle.
//
//  Lifecycle: D3D11 device + swap chain are created lazily on the
//  first `updateHDRImage` call (we need a valid HWND, which Qt
//  doesn't realise until the widget is shown).  Subsequent calls
//  reuse the chain; widget resize triggers
//  `IDXGISwapChain::ResizeBuffers`.  Destruction releases everything
//  via ComPtr.
//
//  Non-Windows builds: this header is `#if defined(_WIN32)` gated.
//  RISE-GUI.vcxproj is the only project that compiles this widget
//  (the macOS Xcode build has its own MetalEDRView).
//
//////////////////////////////////////////////////////////////////////

#ifndef HDRRENDERWIDGET_H
#define HDRRENDERWIDGET_H

#include <QWidget>
#include <QByteArray>
#include <QString>

#if defined(_WIN32)
#include <wrl/client.h>      // Microsoft::WRL::ComPtr
#include <d3d11.h>
#include <dxgi1_6.h>
#endif

class HDRRenderWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HDRRenderWidget(QWidget* parent = nullptr);
    ~HDRRenderWidget() override;

    // L5b — declare we don't want Qt's native paint engine.  The HWND
    // is owned by us via the swap chain; Qt's paint events are
    // ignored (paintEngine() returns nullptr below).
    QPaintEngine* paintEngine() const override { return nullptr; }

    // True iff the active monitor reports an HDR colorspace + max
    // luminance > SDR.  Polled at construction and on every screen
    // change; MainWindow uses this to enable the HDR toggle.
    bool hdrAvailable() const { return m_hdrAvailable; }

    // Stateless availability probe — walks every IDXGIAdapter1 +
    // IDXGIOutput6 enumerable on the system and returns true if ANY
    // attached output reports `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`
    // + `MaxLuminance > 100`.  Used by MainWindow at startup to set
    // the View > HDR Preview toggle's initial enabled state, before
    // the (hidden-in-QStackedWidget) HDR widget gets a Show event.
    // No swap chain or D3D device required; cheap to call.  Returns
    // false on non-Windows builds.
    static bool probeAnyAdapterHDRAvailable();

    // Last-known image dimensions (0 until the first update).  Used
    // by MainWindow for aspect-fit sizing and by the SDR/HDR toggle
    // path to coordinate with the legacy RenderWidget's pixmap dims.
    int sourceWidth() const  { return m_srcW; }
    int sourceHeight() const { return m_srcH; }

public slots:
    // RenderEngine::hdrImageUpdated → this slot.  Half-floats are
    // row-major RGBA binary16, 8 bytes per pixel.  Empty payload
    // (length == 0) clears the swap chain to opaque black; the
    // engine emits this on `clearScene`.
    void updateHDRImage(const QByteArray& halfFloats, int W, int H);

signals:
    // Fires when the active monitor's HDR capability flips between
    // capable and not (e.g. window dragged between an HDR display
    // and an SDR display, or a display's HDR mode is toggled in
    // Windows Settings).  MainWindow connects this to the HDR
    // toggle's enabled state.
    void hdrAvailabilityChanged(bool available);

protected:
    // Resize the swap chain when Qt resizes the widget.  Called
    // before the first paint after a resize.
    void resizeEvent(QResizeEvent* event) override;

    // Force a Present(0, 0) on every paint event so the swap chain's
    // back buffer is composited even when no new pixels arrived
    // (e.g. window restore from minimised, monitor flip).
    void paintEvent(QPaintEvent* event) override;

    // Track screen changes (window drag between displays) so we can
    // re-probe HDR headroom on the new screen.
    bool event(QEvent* ev) override;

private:
#if defined(_WIN32)
    // Lazy-create the D3D11 device + DXGI swap chain on the widget's
    // HWND.  Idempotent; subsequent calls return early.  Sets
    // `m_initFailed` on failure so we don't retry every frame.
    bool ensureDevice();

    // Re-probe `IDXGIOutput6::GetDesc1` for the active monitor.
    // Updates `m_hdrAvailable` and emits `hdrAvailabilityChanged` on
    // a transition.  Called from ensureDevice and from screen-change
    // events.
    void refreshHDRAvailability();

    // Resize swap chain back buffers to match the widget's current
    // device-pixel size.  Called from resizeEvent after the swap
    // chain exists.
    void resizeBackBuffer();

    // Upload a half-float RGBA frame to the staging texture and
    // present.  No-op until `ensureDevice` succeeds.
    void presentFrame(const QByteArray& halfFloats, int W, int H);

    // ── D3D11 / DXGI handles ──────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>        m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;

    // Source-image staging texture (CPU-writable, GPU-readable).
    // Recreated on dim change; reused otherwise.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_srcTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srcSRV;
    int m_srcTexW = 0;
    int m_srcTexH = 0;

    // Fullscreen-quad pipeline used to blit the source texture into
    // the swap chain back buffer with aspect-fit letterboxing.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>     m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>     m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_constantBuffer;

    // HDR availability snapshot from the last refreshHDRAvailability
    // call; emitted via `hdrAvailabilityChanged` on a transition.
    float m_displayMaxLuminance = 0.0f;
#endif

    // Cross-platform state (read by Qt slots even on non-Windows
    // builds where the D3D paths compile out).
    bool m_hdrAvailable = false;
    bool m_initFailed   = false;
    int  m_srcW = 0;
    int  m_srcH = 0;

    // Latched last-frame payload — used so paintEvent (and resize
    // re-Present) can blit even when no fresh pixels arrived.
    QByteArray m_lastFrame;
};

#endif // HDRRENDERWIDGET_H
