#include "ui/renderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace kamil {

bool Renderer::init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts{};
#ifndef NDEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_NONE;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    return true;
}

void Renderer::release_device() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    dc_.Reset();
    device_.Reset();
    swap_chain_.Reset();
    effect_.Reset();
    visual_.Reset();
    dcomp_target_.Reset();
    dcomp_.Reset();
}

bool Renderer::create_device() {
    release_device();
    ++generation_;

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d,
                                   nullptr, nullptr);
    if (FAILED(hr))  // e.g. remote desktop without a GPU
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr,
                               nullptr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) return false;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgi_factory;
    if (FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_ ? width_ : 1;
    desc.Height = height_ ? height_ : 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgi_factory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &swap_chain_))) return false;

    if (FAILED(factory_->CreateDevice(dxgi.Get(), &device_))) return false;
    if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    dc_->SetDpi(dpi_, dpi_);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque target

    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_))) return false;
    if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
    if (FAILED(dcomp_->CreateEffectGroup(&effect_))) return false;
    visual_->SetContent(swap_chain_.Get());
    visual_->SetEffect(effect_.Get());
    dcomp_target_->SetRoot(visual_.Get());
    if (FAILED(dcomp_->Commit())) return false;
    return create_target();
}

bool Renderer::create_target() {
    dc_->SetTarget(nullptr);
    target_.Reset();
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
    dc_->SetTarget(target_.Get());
    return true;
}

bool Renderer::ensure_size(UINT width, UINT height) {
    if (width == width_ && height == height_ && swap_chain_) return true;
    width_ = width;
    height_ = height;
    if (!swap_chain_) return create_device();
    dc_->SetTarget(nullptr);
    target_.Reset();
    if (FAILED(swap_chain_->ResizeBuffers(2, width_, height_, DXGI_FORMAT_UNKNOWN, 0))) return create_device();
    return create_target();
}

void Renderer::set_dpi(float dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi;
    if (dc_) {
        dc_->SetDpi(dpi_, dpi_);
        create_target();
    }
}

ID2D1DeviceContext* Renderer::begin_draw() {
    if (!swap_chain_ && !create_device()) return nullptr;
    if (!target_ && !create_target()) return nullptr;
    dc_->BeginDraw();
    return dc_.Get();
}

bool Renderer::end_draw() {
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr)) hr = swap_chain_->Present(1, 0);
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET) || hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        release_device();
        return false;
    }
    return true;
}

void Renderer::animate_in(bool enabled) {
    if (!dcomp_ || !effect_ || !visual_) return;
    if (!enabled) {
        effect_->SetOpacity(1.f);
        visual_->SetOffsetY(0.f);
        dcomp_->Commit();
        return;
    }
    constexpr float kSeconds = 0.09f;
    ComPtr<IDCompositionAnimation> fade, slide;
    if (SUCCEEDED(dcomp_->CreateAnimation(&fade)) && SUCCEEDED(dcomp_->CreateAnimation(&slide))) {
        fade->AddCubic(0.0, 0.f, 1.f / kSeconds, 0.f, 0.f);
        fade->End(kSeconds, 1.f);
        const float dy = -6.f * dpi_ / 96.f;
        slide->AddCubic(0.0, dy, -dy / kSeconds, 0.f, 0.f);
        slide->End(kSeconds, 0.f);
        effect_->SetOpacity(fade.Get());
        visual_->SetOffsetY(slide.Get());
    }
    dcomp_->Commit();
}

ComPtr<ID2D1Bitmap1> Renderer::create_bitmap(const uint32_t* bgra, UINT width, UINT height) {
    ComPtr<ID2D1Bitmap1> bmp;
    if (!dc_) return bmp;
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(width, height), bgra, width * 4, &props, &bmp);
    return bmp;
}

}  // namespace kamil
