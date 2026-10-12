#include "platform/key_hook.h"

#include <atomic>
#include <condition_variable>

#include "core/hotkey.h"

namespace kamil {

namespace {

std::atomic<KeyHook*> g_hook{nullptr};

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

uint32_t current_mods() {
    uint32_t m = 0;
    if (down(VK_MENU)) m |= kModAlt;
    if (down(VK_CONTROL)) m |= kModCtrl;
    if (down(VK_SHIFT)) m |= kModShift;
    if (down(VK_LWIN) || down(VK_RWIN)) m |= kModWin;
    return m;
}

// An unassigned key, pressed and released while Win is held: Windows then treats the Win key as
// part of a combination and does not open the Start menu when it is released (the same trick
// AutoHotkey uses).
void mask_win_release() {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

}  // namespace

KeyHook::~KeyHook() { stop(); }

bool KeyHook::set(std::vector<Binding> bindings, HWND target) {
    {
        std::lock_guard lock(mutex_);
        bindings_ = std::move(bindings);
        held_.assign(bindings_.size(), false);
        target_ = target;
    }
    if (bindings_.empty()) {
        stop();
        return true;
    }
    if (thread_.joinable()) return installed_;

    std::mutex ready_mutex;
    std::condition_variable ready;
    bool started = false;
    g_hook = this;
    thread_ = std::thread([&] {
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyHook::proc, GetModuleHandleW(nullptr), 0);
        {
            std::lock_guard lock(ready_mutex);  // notify under the lock: set() may return right after
            thread_id_ = GetCurrentThreadId();
            installed_ = hook != nullptr;
            started = true;
            ready.notify_one();
        }
        if (!hook) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        UnhookWindowsHookEx(hook);
    });
    std::unique_lock lock(ready_mutex);
    ready.wait(lock, [&] { return started; });
    return installed_;
}

void KeyHook::stop() {
    if (!thread_.joinable()) return;
    PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
    installed_ = false;
    g_hook = nullptr;
}

LRESULT CALLBACK KeyHook::proc(int code, WPARAM wp, LPARAM lp) {
    KeyHook* self = g_hook.load();
    return self ? self->handle(code, wp, lp) : CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT KeyHook::handle(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (kb->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);  // includes our own mask key
    const bool is_down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    const bool is_up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < bindings_.size(); ++i) {
            const Binding& b = bindings_[i];
            if (b.vk != kb->vkCode) continue;
            if (is_up && held_[i]) {
                held_[i] = false;
                return 1;  // swallow the release of a swallowed press
            }
            if (!is_down || current_mods() != b.mods) continue;
            if (!held_[i]) {  // the first press fires; key repeat is swallowed silently
                held_[i] = true;
                if (b.mods & kModWin) mask_win_release();
                PostMessageW(target_, WM_HOTKEY, b.id, 0);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace kamil
