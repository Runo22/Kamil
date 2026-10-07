#include "platform/icon_loader.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>

namespace kamil {

namespace {

bool extract(IWICImagingFactory* wic, const std::wstring& source, uint32_t size, std::vector<uint32_t>* out) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP hbmp = nullptr;
    const SIZE sz{static_cast<LONG>(size), static_cast<LONG>(size)};
    if (FAILED(factory->GetImage(sz, SIIGBF_ICONONLY, &hbmp)) || !hbmp) return false;

    bool ok = false;
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUsePremultipliedAlpha, &bitmap))) {
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        bitmap->GetSize(&w, &h);
        ComPtr<IWICBitmapScaler> scaler;
        IWICBitmapSource* src = bitmap.Get();
        if ((w != size || h != size) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(bitmap.Get(), size, size, WICBitmapInterpolationModeFant)))
            src = scaler.Get();
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            out->assign(static_cast<size_t>(size) * size, 0);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, size * 4, static_cast<UINT>(out->size() * 4),
                                            reinterpret_cast<BYTE*>(out->data())));
        }
    }
    DeleteObject(hbmp);
    if (!ok) return false;

    // Some legacy icons carry no alpha channel at all: treat them as opaque.
    const bool any_alpha = std::any_of(out->begin(), out->end(), [](uint32_t px) { return (px >> 24) != 0; });
    if (!any_alpha)
        for (auto& px : *out) px |= 0xFF000000u;
    return true;
}

}  // namespace

IconLoader::IconLoader(HWND target) : target_(target), thread_([this] { run(); }) {}

IconLoader::~IconLoader() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_one();
    thread_.join();
}

void IconLoader::request(std::wstring key, std::wstring source, uint32_t size) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Request{std::move(key), std::move(source), size});
        if (queue_.size() > 256) queue_.pop_front();  // drop the oldest, they will be re-requested if still visible
    }
    cv_.notify_one();
}

void IconLoader::run() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        Request req;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            req = std::move(queue_.back());
            queue_.pop_back();
        }
        auto* result = new IconResult{req.key, req.size, {}};
        if (wic) extract(wic.Get(), req.source, req.size, &result->bgra);
        post_owned(target_, WM_KAMIL_ICON_READY, result);
    }
    wic.Reset();
    if (SUCCEEDED(hr)) CoUninitialize();
}

}  // namespace kamil
