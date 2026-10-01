#pragma once
#include <string_view>

namespace dococr {
enum class RegionType { Text, Formula, Table, Image, Unknown };
enum class LayoutPurpose { Body, Heading, Annotation, Footnote, PageMarker, Number, None };
struct LayoutRegionPolicy {
    const char* model_label;
    RegionType type;
    LayoutPurpose purpose;
};
// Frozen PP-DocLayoutV3 class IDs; aliases never erase source class identity.
const LayoutRegionPolicy& layout_region_policy(int class_id);
const char* region_type_name(RegionType type);
// Image is a resource type, not an Ovis task.
bool requires_ovis(std::string_view type);
}
