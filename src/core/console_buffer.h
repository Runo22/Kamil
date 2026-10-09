#pragma once

// Text of one console tab: program output split into lines, with what a terminal would do to it
// (CR LF, a lone CR that rewrites the line for progress bars, ANSI escape codes removed) and each
// line classified as error / warning / source location so the console can colour it and jump
// to the file.

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace kamil {

struct ConsoleLine {
    enum class Kind : uint8_t { Normal, Error, Warning, Location, Note };
    std::wstring text;
    Kind kind = Kind::Normal;
    std::wstring file;  // for Error / Warning / Location
    int line = 0;
};

class ConsoleBuffer {
public:
    explicit ConsoleBuffer(size_t max_lines = 100'000) : max_lines_(max_lines) {}

    // Program output; may end in the middle of a line or of an escape sequence.
    void append(std::wstring_view text);
    // A whole line written by Kamil itself (e.g. "▶ started", "■ exit code 1").
    void note(std::wstring_view text);

    size_t size() const { return lines_.size(); }
    const ConsoleLine& line(size_t i) const { return lines_[i]; }
    // Lines dropped from the front so far (the view keeps its position stable with it).
    uint64_t dropped() const { return dropped_; }
    size_t errors() const { return errors_; }
    size_t warnings() const { return warnings_; }

    // Next / previous error or warning line after / before `from` (wraps around).
    std::optional<size_t> next_issue(size_t from, bool backwards) const;
    std::wstring text(size_t first, size_t last) const;  // lines [first, last], joined with CRLF

private:
    void put(wchar_t c);
    void end_line();
    void classify(ConsoleLine& l);
    ConsoleLine& current();

    std::deque<ConsoleLine> lines_;
    size_t max_lines_;
    uint64_t dropped_ = 0;
    size_t errors_ = 0, warnings_ = 0;
    bool open_ = false;        // the last line is still being written
    bool cr_ = false;          // a CR was seen; the next character decides (LF: new line, else: rewrite)
    enum class Esc : uint8_t { None, Start, Csi, Osc, OscEsc } esc_ = Esc::None;
};

bool is_valid_utf8(std::string_view bytes);
// Number of bytes at the end of `bytes` that start an incomplete UTF-8 sequence (0..3).
size_t incomplete_utf8_tail(std::string_view bytes);

}  // namespace kamil
