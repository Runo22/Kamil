#include "core/line_edit.h"

#include "core/text.h"

namespace kamil {

namespace {

bool is_low_surrogate(wchar_t c) { return sizeof(wchar_t) == 2 && c >= 0xDC00 && c <= 0xDFFF; }
bool is_word_char(wchar_t c) { return !(c == L' ' || c == L'\t' || is_separator(c)); }

}  // namespace

std::wstring LineEdit::selected_text() const { return text_.substr(selection_start(), selection_end() - selection_start()); }

void LineEdit::set_text(std::wstring text, bool select_all_text) {
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = select_all_text ? 0 : caret_;
}

size_t LineEdit::prev_index(size_t i) const {
    if (i == 0) return 0;
    --i;
    if (i > 0 && is_low_surrogate(text_[i])) --i;
    return i;
}

size_t LineEdit::next_index(size_t i) const {
    if (i >= text_.size()) return text_.size();
    ++i;
    if (i < text_.size() && is_low_surrogate(text_[i])) ++i;
    return i;
}

size_t LineEdit::word_left(size_t i) const {
    while (i > 0 && !is_word_char(text_[i - 1])) --i;
    while (i > 0 && is_word_char(text_[i - 1])) --i;
    return i;
}

size_t LineEdit::word_right(size_t i) const {
    const size_t n = text_.size();
    while (i < n && is_word_char(text_[i])) ++i;
    while (i < n && !is_word_char(text_[i])) ++i;
    return i;
}

bool LineEdit::erase_selection() {
    if (!has_selection()) return false;
    const size_t s = selection_start();
    text_.erase(s, selection_end() - s);
    caret_ = anchor_ = s;
    return true;
}

void LineEdit::place(size_t pos, bool extend) {
    caret_ = pos;
    if (!extend) anchor_ = pos;
}

bool LineEdit::insert(std::wstring_view s) {
    std::wstring clean;
    clean.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        if (c < 0x20 || c == 0x7F) continue;
        clean.push_back(c);
    }
    const bool erased = erase_selection();
    if (clean.empty()) return erased;
    text_.insert(caret_, clean);
    caret_ += clean.size();
    anchor_ = caret_;
    return true;
}

bool LineEdit::backspace(bool word) {
    if (erase_selection()) return true;
    if (caret_ == 0) return false;
    const size_t from = word ? word_left(caret_) : prev_index(caret_);
    text_.erase(from, caret_ - from);
    caret_ = anchor_ = from;
    return true;
}

bool LineEdit::del(bool word) {
    if (erase_selection()) return true;
    if (caret_ >= text_.size()) return false;
    const size_t to = word ? word_right(caret_) : next_index(caret_);
    text_.erase(caret_, to - caret_);
    return true;
}

void LineEdit::move_left(bool word, bool extend) {
    if (!extend && has_selection()) {
        place(selection_start(), false);
        return;
    }
    place(word ? word_left(caret_) : prev_index(caret_), extend);
}

void LineEdit::move_right(bool word, bool extend) {
    if (!extend && has_selection()) {
        place(selection_end(), false);
        return;
    }
    place(word ? word_right(caret_) : next_index(caret_), extend);
}

void LineEdit::home(bool extend) { place(0, extend); }
void LineEdit::end(bool extend) { place(text_.size(), extend); }

void LineEdit::select_all() {
    anchor_ = 0;
    caret_ = text_.size();
}

}  // namespace kamil
