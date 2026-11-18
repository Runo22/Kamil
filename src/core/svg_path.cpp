#include "core/svg_path.h"

#include <cstdlib>
#include <string>

namespace kamil {

namespace {

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    void skip_separators() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == ',' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool at_end() {
        skip_separators();
        return i_ >= s_.size();
    }
    bool at_command() {
        skip_separators();
        return i_ < s_.size() && is_command(s_[i_]);
    }
    char command() { return s_[i_++]; }

    // A number: [sign] digits [. digits] [e [sign] digits]; "1.5.5" reads 1.5 and then .5.
    bool number(float* out) {
        skip_separators();
        const size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
        bool digits = false;
        while (i_ < s_.size() && is_digit(s_[i_])) ++i_, digits = true;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && is_digit(s_[i_])) ++i_, digits = true;
        }
        if (!digits) {
            i_ = start;
            return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            size_t j = i_ + 1;
            if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) ++j;
            if (j < s_.size() && is_digit(s_[j])) {
                i_ = j;
                while (i_ < s_.size() && is_digit(s_[i_])) ++i_;
            }
        }
        *out = std::strtof(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
        return true;
    }
    // Arc flags are single characters and may be written without separators ("01").
    bool flag(bool* out) {
        skip_separators();
        if (i_ >= s_.size() || (s_[i_] != '0' && s_[i_] != '1')) return false;
        *out = s_[i_++] == '1';
        return true;
    }

private:
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_command(char c) {
        switch (c) {
            case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v': case 'C': case 'c':
            case 'S': case 's': case 'Q': case 'q': case 'T': case 't': case 'A': case 'a': case 'Z': case 'z':
                return true;
            default:
                return false;
        }
    }
    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

std::vector<PathCmd> parse_svg_path(std::string_view d) {
    std::vector<PathCmd> out;
    Reader r(d);
    float cx = 0, cy = 0;          // current point
    float sx = 0, sy = 0;          // start of the current sub path
    float lcx = 0, lcy = 0;        // last control point (for S / T)
    char last = 0;                 // previous command (upper case)
    char cmd = 0;
    while (!r.at_end()) {
        if (r.at_command()) cmd = r.command();
        else if (!cmd) break;  // numbers before any command
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = static_cast<char>(rel ? cmd - 32 : cmd);
        const float ox = rel ? cx : 0, oy = rel ? cy : 0;
        PathCmd p;
        switch (up) {
            case 'Z':
                p.type = PathCmd::Type::Close;
                out.push_back(p);
                cx = sx, cy = sy;
                last = 'Z';
                cmd = 0;  // Z takes no coordinates; a number after it is malformed
                continue;
            case 'M': case 'L': case 'T': {
                float x, y;
                if (!r.number(&x) || !r.number(&y)) return out;
                p.x = ox + x, p.y = oy + y;
                if (up == 'M') {
                    p.type = PathCmd::Type::Move;
                    sx = p.x, sy = p.y;
                    cmd = rel ? 'l' : 'L';  // further pairs are lines
                } else if (up == 'L') {
                    p.type = PathCmd::Type::Line;
                } else {
                    p.type = PathCmd::Type::Quad;
                    const bool smooth = last == 'Q' || last == 'T';
                    p.x1 = smooth ? 2 * cx - lcx : cx;
                    p.y1 = smooth ? 2 * cy - lcy : cy;
                    lcx = p.x1, lcy = p.y1;
                }
                break;
            }
            case 'H': case 'V': {
                float v;
                if (!r.number(&v)) return out;
                p.type = PathCmd::Type::Line;
                p.x = up == 'H' ? ox + v : cx;
                p.y = up == 'V' ? oy + v : cy;
                break;
            }
            case 'C': case 'S': {
                float a[6];
                const int n = up == 'C' ? 6 : 4;
                for (int k = 0; k < n; ++k)
                    if (!r.number(&a[k])) return out;
                p.type = PathCmd::Type::Cubic;
                if (up == 'C') {
                    p.x1 = ox + a[0], p.y1 = oy + a[1], p.x2 = ox + a[2], p.y2 = oy + a[3], p.x = ox + a[4], p.y = oy + a[5];
                } else {
                    const bool smooth = last == 'C' || last == 'S';
                    p.x1 = smooth ? 2 * cx - lcx : cx;
                    p.y1 = smooth ? 2 * cy - lcy : cy;
                    p.x2 = ox + a[0], p.y2 = oy + a[1], p.x = ox + a[2], p.y = oy + a[3];
                }
                lcx = p.x2, lcy = p.y2;
                break;
            }
            case 'Q': {
                float a[4];
                for (float& v : a)
                    if (!r.number(&v)) return out;
                p.type = PathCmd::Type::Quad;
                p.x1 = ox + a[0], p.y1 = oy + a[1], p.x = ox + a[2], p.y = oy + a[3];
                lcx = p.x1, lcy = p.y1;
                break;
            }
            case 'A': {
                float rx, ry, rot, x, y;
                if (!r.number(&rx) || !r.number(&ry) || !r.number(&rot) || !r.flag(&p.large_arc) || !r.flag(&p.sweep) || !r.number(&x) ||
                    !r.number(&y))
                    return out;
                p.type = PathCmd::Type::Arc;
                p.rx = rx < 0 ? -rx : rx, p.ry = ry < 0 ? -ry : ry, p.rotation = rot;
                p.x = ox + x, p.y = oy + y;
                break;
            }
            default:
                return out;
        }
        out.push_back(p);
        cx = p.x, cy = p.y;
        last = up == 'M' ? 'M' : up;
    }
    return out;
}

}  // namespace kamil
