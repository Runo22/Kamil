#pragma once

// SVG path data ("M4 5.5a3.5 3.5 0 1 1 4.49 3.36 ...") turned into absolute drawing commands, so
// the icons (Fluent UI System Icons, embedded as path data) are drawn as Direct2D geometry without
// an SVG renderer. Platform independent and unit tested.

#include <string_view>
#include <vector>

namespace kamil {

struct PathCmd {
    enum class Type { Move, Line, Cubic, Quad, Arc, Close };
    Type type = Type::Move;
    float x = 0, y = 0;    // end point (absolute)
    float x1 = 0, y1 = 0;  // Cubic: first control point; Quad: the control point
    float x2 = 0, y2 = 0;  // Cubic: second control point
    float rx = 0, ry = 0, rotation = 0;  // Arc
    bool large_arc = false, sweep = false;  // Arc (sweep: clockwise in SVG's y-down space)
};

// Parses M L H V C S Q T A Z (and their relative forms), repeated coordinates after a command
// ("M1 2 3 4" = move, then line), compact numbers ("1.5.5" = 1.5 .5, "1-2" = 1 -2, "4e-1") and
// arc flags written together ("a1 1 0 01.5.5"). H/V become lines, S/T become curves with the
// reflected control point. Stops at the first malformed token and returns what came before.
std::vector<PathCmd> parse_svg_path(std::string_view d);

}  // namespace kamil
