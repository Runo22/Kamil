#pragma once

// Minimal late-bound COM automation (IDispatch by name), enough to drive Visual Studio's DTE
// without type libraries or #import. Errors are reported as ComError exceptions.

#include <oleauto.h>

#include <initializer_list>
#include <string>
#include <utility>

#include "platform/win.h"
#include "core/i18n.h"

namespace kamil {

struct ComError {
    HRESULT hr = E_FAIL;
    std::wstring what;
};

class Variant {
public:
    Variant() { VariantInit(&v); }
    ~Variant() { VariantClear(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    Variant(Variant&& o) noexcept {
        v = o.v;
        VariantInit(&o.v);
    }

    static Variant str(const std::wstring& s) {
        Variant r;
        r.v.vt = VT_BSTR;
        r.v.bstrVal = SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
        return r;
    }
    static Variant i4(long n) {
        Variant r;
        r.v.vt = VT_I4;
        r.v.lVal = n;
        return r;
    }

    std::wstring as_string() const {
        if (v.vt == VT_BSTR) return v.bstrVal ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : std::wstring();
        VARIANT tmp;
        VariantInit(&tmp);
        std::wstring out;
        if (SUCCEEDED(VariantChangeType(&tmp, const_cast<VARIANT*>(&v), 0, VT_BSTR)) && tmp.bstrVal) out.assign(tmp.bstrVal, SysStringLen(tmp.bstrVal));
        VariantClear(&tmp);
        return out;
    }
    long as_long() const {
        VARIANT tmp;
        VariantInit(&tmp);
        long out = 0;
        if (SUCCEEDED(VariantChangeType(&tmp, const_cast<VARIANT*>(&v), 0, VT_I4))) out = tmp.lVal;
        VariantClear(&tmp);
        return out;
    }
    ComPtr<IDispatch> as_dispatch() const {
        ComPtr<IDispatch> d;
        if (v.vt == VT_DISPATCH && v.pdispVal) d = v.pdispVal;
        else if (v.vt == VT_UNKNOWN && v.punkVal) v.punkVal->QueryInterface(IID_PPV_ARGS(&d));
        return d;
    }

    VARIANT v;
};

class Disp {
public:
    Disp() = default;
    explicit Disp(ComPtr<IDispatch> p) : p_(std::move(p)) {}
    bool valid() const { return p_ != nullptr; }
    IDispatch* raw() const { return p_.Get(); }

    // Calls a method or reads a property (both flags are passed, which DTE accepts).
    Variant invoke(const wchar_t* name, std::initializer_list<const Variant*> args = {}, WORD flags = DISPATCH_METHOD | DISPATCH_PROPERTYGET) const {
        if (!p_) throw ComError{E_POINTER, std::wstring(name) + loc(L": no object", L": nesne yok")};
        DISPID id = 0;
        LPOLESTR n = const_cast<LPOLESTR>(name);
        HRESULT hr = p_->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
        if (FAILED(hr)) throw ComError{hr, std::wstring(name) + loc(L": member not found", L": üye bulunamadı")};
        // DISPPARAMS expects arguments in reverse order.
        VARIANT argv[8];
        UINT argc = 0;
        for (auto it = args.end(); it != args.begin() && argc < 8;) {
            --it;
            argv[argc++] = (*it)->v;  // shallow copy; the Variants outlive the call
        }
        DISPPARAMS dp{argc ? argv : nullptr, nullptr, argc, 0};
        Variant result;
        EXCEPINFO ex{};
        hr = p_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, &result.v, &ex, nullptr);
        if (FAILED(hr)) {
            std::wstring msg = std::wstring(name) + L": ";
            if (ex.bstrDescription) msg.append(ex.bstrDescription, SysStringLen(ex.bstrDescription));
            else msg += win32_error_message(static_cast<DWORD>(hr));
            SysFreeString(ex.bstrDescription);
            SysFreeString(ex.bstrSource);
            SysFreeString(ex.bstrHelpFile);
            throw ComError{hr, msg};
        }
        return result;
    }

    Disp get(const wchar_t* name) const { return Disp(invoke(name).as_dispatch()); }
    Disp item(long index) const {
        const Variant i = Variant::i4(index);
        return Disp(invoke(L"Item", {&i}).as_dispatch());
    }
    std::wstring get_string(const wchar_t* name) const { return invoke(name).as_string(); }
    long get_long(const wchar_t* name) const { return invoke(name).as_long(); }

private:
    ComPtr<IDispatch> p_;
};

}  // namespace kamil
