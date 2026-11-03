#include "protekt/game/store_gauge.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "protekt/sim/units.hpp"

namespace protekt::game {

StoreGaugeLayout store_gauge_layout(const RadarPanel& beside, int capacity, int band, int held,
                                    float cell) {
    StoreGaugeLayout layout;
    const int total = std::max(capacity, 1);
    const int gold = std::clamp(band, 0, total);
    const int level = std::clamp(held, 0, total);

    const float width = std::max(beside.width * kGaugeWidthFraction, kGaugeMinWidthCells * cell);
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

    // The chunks: the base store (`total - gold` of it), then a scavenger's worth at a time.
    const float half_gap = kGaugeChunkGapCells * cell * 0.5F;
    std::vector<std::pair<int, bool>> shares;
    if (total - gold > 0) {
        shares.emplace_back(total - gold, false);
    }
    for (int left = gold; left > 0; left -= sim::kStoreCapacityPerDeployed) {
        shares.emplace_back(std::min<int>(left, sim::kStoreCapacityPerDeployed), true);
    }
    float from = layout.column.y;
    for (std::size_t index = 0; index < shares.size(); ++index) {
        const float slice = unit * static_cast<float>(shares.at(index).first);
        const float top = from + (index > 0 ? half_gap : 0.0F);
        const float bottom = from + slice - (index + 1 < shares.size() ? half_gap : 0.0F);
        from += slice;
        if (bottom <= top) {
            continue;
        }
        layout.chunks.push_back(
            GaugeChunk{GaugeRect{layout.column.x, top, layout.column.width, bottom - top},
                       shares.at(index).second});
    }

    // The white bar, cut by the chunks it passes through.
    const float level_top = layout.level.y;
    for (const GaugeChunk& chunk : layout.chunks) {
        const float top = std::max(chunk.box.y, level_top);
        const float bottom = chunk.box.y + chunk.box.height;
        if (level_height > 0.0F && bottom > top) {
            layout.fills.push_back(
                GaugeRect{layout.level.x, top, layout.level.width, bottom - top});
        }
    }

    // The pill, as bands cut at every chunk edge so no band is taller than a chunk.
    std::vector<float> edges{layout.frame.y, layout.frame.y + layout.frame.height};
    for (const GaugeChunk& chunk : layout.chunks) {
        edges.push_back(chunk.box.y);
        edges.push_back(chunk.box.y + chunk.box.height);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (std::size_t index = 0; index + 1 < edges.size(); ++index) {
        layout.pill.push_back(GaugeRect{layout.frame.x, edges.at(index), layout.frame.width,
                                        edges.at(index + 1) - edges.at(index)});
    }
    return layout;
}

} // namespace protekt::game
