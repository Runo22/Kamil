#include "ui/fluent_geometry.h"

#include "core/svg_path.h"

namespace kamil {

const FluentIcon* find_fluent_icon(wchar_t glyph) {
    for (const auto& f : kFluentIcons)
        if (f.glyph == glyph) return &f;
    return nullptr;
}

ComPtr<ID2D1PathGeometry> build_fluent_geometry(ID2D1Factory* factory, const FluentIcon& icon) {
    // Built once from the SVG path data; factory resources survive a lost device.
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);  // SVG's default fill rule (nonzero)
    bool open = false;
    D2D1_POINT_2F start{}, current{};
    for (const PathCmd& c : parse_svg_path(icon.path)) {
        const D2D1_POINT_2F to{c.x, c.y};
        if (c.type == PathCmd::Type::Move) {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(to, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            start = current = to;
            continue;
        }
        if (c.type == PathCmd::Type::Close) {
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            current = start;
            continue;
        }
        if (!open) {  // drawing on after Z: a new figure from the closed one's start
            sink->BeginFigure(current, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
        }
        switch (c.type) {
            case PathCmd::Type::Line: sink->AddLine(to); break;
            case PathCmd::Type::Cubic: sink->AddBezier(D2D1::BezierSegment({c.x1, c.y1}, {c.x2, c.y2}, to)); break;
            case PathCmd::Type::Quad: sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({c.x1, c.y1}, to)); break;
            case PathCmd::Type::Arc:
                sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(c.rx, c.ry), c.rotation,
                                              c.sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                              c.large_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                break;
            default: break;
        }
        current = to;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

}  // namespace kamil
