#include "protekt/game/store_gauge.hpp"

#include <algorithm>

namespace protekt::game {

StoreGaugeLayout store_gauge_layout(const RadarPanel& beside, int capacity, int band, int held,
                                    float cell) {
    StoreGaugeLayout layout;
    const int total = std::max(capacity, 1);
    const int gold = std::clamp(band, 0, total);
    const int level = std::clamp(held, 0, total);

    const float width = beside.width * kGaugeWidthFraction;
    layout.frame
        = GaugeRect{beside.x + beside.width + kGaugeGapPixels, beside.y, width, beside.height};

    const float pad = width * kGaugePadFraction;
    const float number_room = kGaugeNumberCells * cell;
    layout.column = GaugeRect{layout.frame.x + pad, layout.frame.y + pad, width - (2.0F * pad),
                              layout.frame.height - pad - number_room};

    // The whole extent is the capacity: gold at the bottom, green over it.
    const float unit = layout.column.height / static_cast<float>(total);
    const float gold_height = unit * static_cast<float>(gold);
    layout.green = GaugeRect{layout.column.x, layout.column.y, layout.column.width,
                             layout.column.height - gold_height};
    layout.gold = GaugeRect{layout.column.x, layout.column.y + layout.green.height,
                            layout.column.width, gold_height};

    const float inset = layout.column.width * kGaugeLevelInsetFraction;
    const float level_height = unit * static_cast<float>(level);
    layout.level
        = GaugeRect{layout.column.x + inset, layout.column.y + layout.column.height - level_height,
                    layout.column.width - (2.0F * inset), level_height};

    layout.number = GaugeRect{layout.frame.x, layout.column.y + layout.column.height, width,
                              layout.frame.y + layout.frame.height
                                  - (layout.column.y + layout.column.height)};
    layout.text = std::to_string(held);
    return layout;
}

} // namespace protekt::game
