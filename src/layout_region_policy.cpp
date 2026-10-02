#include "layout_region_policy.hpp"
#include <array>

namespace dococr {
const LayoutRegionPolicy& layout_region_policy(int class_id) {
    using T = RegionType;
    using P = LayoutPurpose;
    static const std::array<LayoutRegionPolicy, 25> classes = {{
        {"abstract", T::Text, P::Body},
        {"algorithm", T::Text, P::Body},
        {"aside_text", T::Text, P::Aside},
        {"chart", T::Image, P::None},
        {"content", T::Text, P::Body},
        {"formula", T::Formula, P::None},
        {"doc_title", T::Text, P::Heading},
        {"figure_title", T::Text, P::Annotation},
        {"footer", T::Text, P::PageMarker},
        {"footer", T::Image, P::PageMarker},
        {"footnote", T::Text, P::Footnote},
        {"formula_number", T::Text, P::None},
        {"header", T::Text, P::PageMarker},
        {"header", T::Image, P::PageMarker},
        {"image", T::Image, P::None},
        {"formula", T::Formula, P::None},
        {"number", T::Text, P::Number},
        {"paragraph_title", T::Text, P::Heading},
        {"reference", T::Text, P::Body},
        {"reference_content", T::Text, P::Body},
        {"seal", T::Image, P::None},
        {"table", T::Table, P::None},
        {"text", T::Text, P::Body},
        {"text", T::Text, P::Body},
        {"vision_footnote", T::Text, P::Annotation},
    }};
    static const LayoutRegionPolicy unknown{nullptr, T::Unknown, P::None};
    return class_id >= 0 && class_id < int(classes.size()) ? classes[size_t(class_id)] : unknown;
}

const char* region_type_name(RegionType type) {
    switch (type) {
        case RegionType::Text: return "text";
        case RegionType::Formula: return "formula";
        case RegionType::Table: return "table";
        case RegionType::Image: return "image";
        case RegionType::Unknown: return "unknown";
    }
    return "unknown";
}

bool requires_ovis(std::string_view type) {
    return type == "text" || type == "formula" || type == "table";
}
}
