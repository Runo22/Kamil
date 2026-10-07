#pragma once

#include <string>
#include <string_view>

namespace kamil {

// Single-line text editing model used by the search box (platform independent, unit tested).
// Positions are code unit indices; the caret never splits a UTF-16 surrogate pair.
class LineEdit {
public:
    const std::wstring& text() const { return text_; }
    size_t caret() const { return caret_; }
    size_t anchor() const { return anchor_; }
    bool has_selection() const { return caret_ != anchor_; }
    size_t selection_start() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t selection_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::wstring selected_text() const;

    void set_text(std::wstring text, bool select_all = false);
    void clear() { set_text({}); }

    // Each returns true if the text changed.
    bool insert(std::wstring_view s);  // replaces the selection; control characters are dropped
    bool backspace(bool word);
    bool del(bool word);

    void move_left(bool word, bool extend);
    void move_right(bool word, bool extend);
    void home(bool extend);
    void end(bool extend);
    void select_all();

private:
    size_t prev_index(size_t i) const;
    size_t next_index(size_t i) const;
    size_t word_left(size_t i) const;
    size_t word_right(size_t i) const;
    bool erase_selection();
    void place(size_t pos, bool extend);

    std::wstring text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
};

}  // namespace kamil
