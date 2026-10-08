#include "platform/icon_loader.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>

#include "core/pixels.h"

namespace kamil {

namespace {

bool copy_pixels(IWICBitmapSource* src, UINT w, UINT h, std::vector<uint32_t>* out) {
    out->assign(static_cast<size_t>(w) * h, 0);
    return SUCCEEDED(src->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->size() * 4), reinterpret_cast<BYTE*>(out->data())));
}

bool extract(IWICImagingFactory* wic, const std::wstring& source, uint32_t size, std::vector<uint32_t>* out) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP hbmp = nullptr;
    const SIZE sz{static_cast<LONG>(size), static_cast<LONG>(size)};
    if (FAILED(factory->GetImage(sz, SIIGBF_ICONONLY, &hbmp)) || !hbmp) return false;

    // Read the raw channels without letting WIC reinterpret them; whether the alpha is straight
    // or premultiplied differs per icon and is decided by normalize_icon_pixels().
    bool ok = false;
    UINT w = 0, h = 0;
    std::vector<uint32_t> raw;
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &bitmap)) && SUCCEEDED(bitmap->GetSize(&w, &h))) {
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)))
            ok = copy_pixels(conv.Get(), w, h, &raw);
    }
    DeleteObject(hbmp);
    if (!ok || w == 0 || h == 0) return false;
    normalize_icon_pixels(raw);

    if (w == size && h == size) {
        *out = std::move(raw);
        return true;
    }
    // Scale premultiplied data (scaling straight alpha would bring the halo back).
    ComPtr<IWICBitmap> pre;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, static_cast<UINT>(raw.size() * 4),
                                           reinterpret_cast<BYTE*>(raw.data()), &pre)) ||
        FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(pre.Get(), size, size, WICBitmapInterpolationModeFant)))
        return false;
    return copy_pixels(scaler.Get(), size, size, out);
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
