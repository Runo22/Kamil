#include <cmath>

#include "core/svg_path.h"
#include "test.h"

using namespace kamil;
using T = PathCmd::Type;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool at(const PathCmd& c, T type, float x, float y) { return c.type == type && near(c.x, x) && near(c.y, y); }
}  // namespace

TEST(svg_path_basic_and_relative) {
    auto p = parse_svg_path("M4 5 L10 5 H12 V8 Z");
    CHECK_EQ(p.size(), 5u);
    CHECK(at(p[0], T::Move, 4, 5));
    CHECK(at(p[1], T::Line, 10, 5));
    CHECK(at(p[2], T::Line, 12, 5));  // H keeps y
    CHECK(at(p[3], T::Line, 12, 8));  // V keeps x
    CHECK(p[4].type == T::Close);

    p = parse_svg_path("m1 1 2 0 0 3h-1v-1z m5 5 l1 1");  // implicit lineto after m, relative everything
    CHECK_EQ(p.size(), 8u);
    CHECK(at(p[0], T::Move, 1, 1));
    CHECK(at(p[1], T::Line, 3, 1));
    CHECK(at(p[2], T::Line, 3, 4));
    CHECK(at(p[3], T::Line, 2, 4));
    CHECK(at(p[4], T::Line, 2, 3));
    CHECK(p[5].type == T::Close);
    CHECK(at(p[6], T::Move, 6, 6));  // relative to the closed sub path's start (1,1)
    CHECK(at(p[7], T::Line, 7, 7));
}

TEST(svg_path_numbers) {
    auto p = parse_svg_path("M1.5.5L-2-3l1e1,2.5E-1");
    CHECK_EQ(p.size(), 3u);
    CHECK(at(p[0], T::Move, 1.5f, 0.5f));
    CHECK(at(p[1], T::Line, -2, -3));
    CHECK(at(p[2], T::Line, 8, -2.75f));
}

TEST(svg_path_curves_and_arcs) {
    auto p = parse_svg_path("M0 0C1 2 3 4 5 6s7 8 9 10Q1 1 2 2t4 4");
    CHECK_EQ(p.size(), 5u);
    CHECK(at(p[1], T::Cubic, 5, 6) && near(p[1].x1, 1) && near(p[1].y2, 4));
    // S: first control point is the reflection of (3,4) about (5,6) -> (7,8); the rest is relative.
    CHECK(at(p[2], T::Cubic, 14, 16) && near(p[2].x1, 7) && near(p[2].y1, 8) && near(p[2].x2, 12) && near(p[2].y2, 14));
    CHECK(at(p[3], T::Quad, 2, 2) && near(p[3].x1, 1));
    // T: control point reflected (1,1) about (2,2) -> (3,3); end relative -> (6,6).
    CHECK(at(p[4], T::Quad, 6, 6) && near(p[4].x1, 3) && near(p[4].y1, 3));

    // Arc flags written together, as the Fluent icons do: "a3.5 3.5 0 1 1 4.49 3.36" and "a1 1 0 01.5.5".
    p = parse_svg_path("M4 5.5a3.5 3.5 0 1 1 4.49 3.36A1 1 0 011.5.5");
    CHECK_EQ(p.size(), 3u);
    CHECK(at(p[1], T::Arc, 8.49f, 8.86f) && near(p[1].rx, 3.5f) && p[1].large_arc && p[1].sweep);
    CHECK(at(p[2], T::Arc, 1.5f, 0.5f) && !p[2].large_arc && p[2].sweep);
}

TEST(svg_path_malformed) {
    CHECK(parse_svg_path("").empty());
    CHECK(parse_svg_path("12 13").empty());              // numbers before any command
    CHECK_EQ(parse_svg_path("M1 1 L2").size(), 1u);      // stops at the incomplete line
    CHECK_EQ(parse_svg_path("M1 1 X 3 3").size(), 1u);   // unknown command
    CHECK_EQ(parse_svg_path("M0 0A1 1 0 2 0 1 1").size(), 1u);  // a flag must be 0 or 1
}

#include "ui/fluent_icons.h"

TEST(svg_path_every_embedded_icon_parses) {
    for (const auto& icon : kFluentIcons) {
        const auto p = parse_svg_path(icon.path);
        // The whole path is read: it starts with a move and its last figure is closed.
        CHECK(p.size() > 3);
        CHECK(!p.empty() && p.front().type == T::Move);
        CHECK(!p.empty() && p.back().type == T::Close);
        CHECK(icon.view == 24.f || icon.view == 20.f);
    }
}
