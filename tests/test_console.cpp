#include "core/console_buffer.h"
#include "core/cmake.h"
#include "test.h"

using namespace kamil;

TEST(console_lines_and_carriage_return) {
    ConsoleBuffer b;
    b.append(L"hello\r\nwor");
    b.append(L"ld\n");
    CHECK_EQ(b.size(), 2u);
    CHECK(b.line(0).text == L"hello");
    CHECK(b.line(1).text == L"world");
    // Progress output: a lone CR rewrites the line; a CR LF split over two chunks is one line end.
    b.append(L"10%\r50%\r100%\r");
    b.append(L"\ndone\n");
    CHECK_EQ(b.size(), 4u);
    CHECK(b.line(2).text == L"100%");
    CHECK(b.line(3).text == L"done");
    // Partial last line is visible.
    b.append(L"partial");
    CHECK_EQ(b.size(), 5u);
    CHECK(b.line(4).text == L"partial");
    b.note(L"■ exit code 0");
    CHECK(b.line(5).kind == ConsoleLine::Kind::Note);
}

TEST(console_strips_ansi_and_classifies) {
    ConsoleBuffer b;
    b.append(L"\x1b[31mred\x1b[0m text\n\x1b]0;title\x07" L"after\n\x1b[1");  // escape split across chunks
    b.append(L";32mgreen\n");
    CHECK(b.line(0).text == L"red text");
    CHECK(b.line(1).text == L"after");
    CHECK(b.line(2).text == L"green");

    b.append(L"C:\\src\\a.cpp(12): error C2065: 'x': undeclared identifier\n");
    b.append(L"src/b.cpp:7:3: warning: unused variable 'y' [-Wunused-variable]\n");
    b.append(L"  File \"D:\\tools\\flash.py\", line 42, in main\n");
    CHECK(b.line(3).kind == ConsoleLine::Kind::Error);
    CHECK(b.line(3).file == L"C:\\src\\a.cpp");
    CHECK_EQ(b.line(3).line, 12);
    CHECK(b.line(4).kind == ConsoleLine::Kind::Warning);
    CHECK(b.line(4).file == L"src/b.cpp");
    CHECK_EQ(b.line(4).line, 7);
    CHECK(b.line(5).kind == ConsoleLine::Kind::Location);
    CHECK_EQ(b.line(5).line, 42);
    CHECK_EQ(b.errors(), 1u);
    CHECK_EQ(b.warnings(), 1u);
    CHECK(b.next_issue(0, false) == std::optional<size_t>(3));
    CHECK(b.next_issue(3, false) == std::optional<size_t>(4));
    CHECK(b.next_issue(4, false) == std::optional<size_t>(3));  // wraps
    CHECK(b.next_issue(3, true) == std::optional<size_t>(4));
}

TEST(console_line_limit) {
    ConsoleBuffer b(3);
    b.append(L"a.cpp(1): error C1: x\nb\nc\nd\n");
    CHECK_EQ(b.size(), 3u);
    CHECK_EQ(b.dropped(), 1u);
    CHECK(b.line(0).text == L"b");
    CHECK_EQ(b.errors(), 0u);  // the dropped error is no longer counted
}

TEST(build_line_parser) {
    auto msvc = parse_build_line("  12>C:\\x\\main.cpp(88,5): error C2440: 'initializing': cannot convert");
    CHECK(msvc && msvc->error && msvc->line == 88 && msvc->file == "C:\\x\\main.cpp" && msvc->code == "C2440");
    auto gcc = parse_build_line("C:/x/main.cpp:88:5: fatal error: foo.h: No such file or directory");
    CHECK(gcc && gcc->error && gcc->line == 88 && gcc->file == "C:/x/main.cpp");
    CHECK(!parse_build_line("all good: nothing to see"));
    CHECK(!parse_build_line("error: no location"));
}

TEST(utf8_helpers) {
    CHECK(is_valid_utf8("plain"));
    CHECK(is_valid_utf8("\xC3\xA7\xC4\xB1"));  // çı
    CHECK(!is_valid_utf8("\x87\x8D"));          // OEM 857 bytes
    CHECK_EQ(incomplete_utf8_tail("ab\xC3"), 1u);
    CHECK_EQ(incomplete_utf8_tail("ab\xE2\x82"), 2u);
    CHECK_EQ(incomplete_utf8_tail("ab\xE2\x82\xAC"), 0u);
    CHECK_EQ(incomplete_utf8_tail("abc"), 0u);
}
