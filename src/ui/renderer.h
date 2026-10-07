#pragma once

#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>

#include <cstdint>

#include "platform/win.h"

namespace kamil {

// Direct2D drawing into a DirectComposition swap chain with premultiplied alpha, so the window
// can have anti-aliased rounded corners and a soft shadow on Windows 10 without layered windows.
//
// The swap chain is sized for the largest possible window; the window itself is resized to the
// content, which avoids buffer reallocation (and flicker) while the result list grows or shrinks.
class Renderer {
public:
    bool init(HWND hwnd);
    void release_device();

    // Physical pixel size of the swap chain.
    bool ensure_size(UINT width, UINT height);
    void set_dpi(float dpi);
    float dpi() const { return dpi_; }

    // Returns nullptr if the device could not be (re)created.
    ID2D1DeviceContext* begin_draw();
    // Ends drawing and presents. Returns false if the device was lost (resources must be recreated).
    bool end_draw();

    // Short fade + slide in, run by DirectComposition independently of our thread.
    void animate_in(bool enabled);

    ID2D1Factory1* d2d_factory() const { return factory_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    ID2D1DeviceContext* context() const { return dc_.Get(); }

    // Incremented whenever the device is recreated; device-dependent caches (bitmaps, brushes)
    // compare against it.
    uint64_t generation() const { return generation_; }

    ComPtr<ID2D1Bitmap1> create_bitmap(const uint32_t* premultiplied_bgra, UINT width, UINT height);

private:
    bool create_device();
    bool create_target();

    HWND hwnd_ = nullptr;
    UINT width_ = 0, height_ = 0;
    float dpi_ = 96.f;
    uint64_t generation_ = 0;

    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcomp_target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDCompositionEffectGroup> effect_;
};

}  // namespace kamil
