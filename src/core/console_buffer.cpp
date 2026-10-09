#include "core/console_buffer.h"

#include "core/cmake.h"
#include "core/text.h"

namespace kamil {

ConsoleLine& ConsoleBuffer::current() {
    if (!open_) {
        lines_.emplace_back();
        open_ = true;
        if (lines_.size() > max_lines_) {
            const ConsoleLine& first = lines_.front();
            errors_ -= first.kind == ConsoleLine::Kind::Error;
            warnings_ -= first.kind == ConsoleLine::Kind::Warning;
            lines_.pop_front();
            ++dropped_;
        }
    }
    return lines_.back();
}

void ConsoleBuffer::classify(ConsoleLine& l) {
    if (l.kind == ConsoleLine::Kind::Note) return;
    errors_ -= l.kind == ConsoleLine::Kind::Error;
    warnings_ -= l.kind == ConsoleLine::Kind::Warning;
    l.kind = ConsoleLine::Kind::Normal;
    l.file.clear();
    l.line = 0;
    if (l.text.size() > 2000) return;  // not a compiler message; skip the parse
    if (auto issue = parse_build_line(narrow(l.text))) {
        l.kind = issue->code == "trace" ? ConsoleLine::Kind::Location
                 : issue->error         ? ConsoleLine::Kind::Error
                                        : ConsoleLine::Kind::Warning;
        l.file = widen(issue->file);
        l.line = issue->line;
    }
    errors_ += l.kind == ConsoleLine::Kind::Error;
    warnings_ += l.kind == ConsoleLine::Kind::Warning;
}

void ConsoleBuffer::end_line() {
    ConsoleLine& l = current();
    classify(l);
    open_ = false;
}

void ConsoleBuffer::put(wchar_t c) {
    // ANSI escape sequences: ESC [ ... final (CSI), ESC ] ... BEL or ESC \ (OSC), ESC x.
    switch (esc_) {
        case Esc::Start:
            esc_ = c == L'[' ? Esc::Csi : c == L']' ? Esc::Osc : Esc::None;
            return;
        case Esc::Csi:
            if (c >= 0x40 && c <= 0x7E) esc_ = Esc::None;
            return;
        case Esc::Osc:
            if (c == 0x07) esc_ = Esc::None;
            else if (c == 0x1B) esc_ = Esc::OscEsc;
            return;
        case Esc::OscEsc:
            esc_ = Esc::None;
            return;
        case Esc::None:
            break;
    }
    if (c == 0x1B) {
        esc_ = Esc::Start;
        return;
    }
    if (cr_) {
        cr_ = false;
        if (c == L'\n') {
            end_line();
            return;
        }
        if (open_) current().text.clear();  // lone CR: the line is rewritten (progress output)
    }
    switch (c) {
        case L'\r':
            cr_ = true;
            return;
        case L'\n':
            end_line();
            return;
        case L'\b': {
            ConsoleLine& l = current();
            if (!l.text.empty()) l.text.pop_back();
            return;
        }
        case L'\t': {
            ConsoleLine& l = current();
            l.text.append(4 - l.text.size() % 4, L' ');
            return;
        }
        default:
            if (c < 0x20) return;  // other control characters
            current().text.push_back(c);
    }
}

void ConsoleBuffer::append(std::wstring_view text) {
    for (wchar_t c : text) put(c);
    if (open_) classify(lines_.back());  // keep the partial line classified too
}

void ConsoleBuffer::note(std::wstring_view text) {
    if (open_) end_line();
    cr_ = false;
    ConsoleLine& l = current();
    l.text = text;
    l.kind = ConsoleLine::Kind::Note;
    open_ = false;
}

std::optional<size_t> ConsoleBuffer::next_issue(size_t from, bool backwards) const {
    const size_t n = lines_.size();
    if (n == 0) return std::nullopt;
    for (size_t step = 1; step <= n; ++step) {
        const size_t i = backwards ? (from + n - step % n) % n : (from + step) % n;
        const auto k = lines_[i].kind;
        if (k == ConsoleLine::Kind::Error || k == ConsoleLine::Kind::Warning) return i;
    }
    return std::nullopt;
}

std::wstring ConsoleBuffer::text(size_t first, size_t last) const {
    std::wstring out;
    for (size_t i = first; i <= last && i < lines_.size(); ++i) {
        out += lines_[i].text;
        if (i != last) out += L"\r\n";
    }
    return out;
}

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

size_t incomplete_utf8_tail(std::string_view s) {
    for (size_t back = 1; back <= 3 && back <= s.size(); ++back) {
        const auto c = static_cast<unsigned char>(s[s.size() - back]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte: look further back
        const size_t need = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        return need > back ? back : 0;
    }
    return 0;
}

}  // namespace kamil
