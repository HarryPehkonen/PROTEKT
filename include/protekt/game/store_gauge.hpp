#pragma once

#include <array>
#include <string>
#include <vector>

#include "protekt/game/radar.hpp"

namespace protekt::game {

/// A rectangle in frame pixels, from the left and from the top.
struct GaugeRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

/// THE STORE GAUGE (Harri, 2026-11-01), a vertical gauge immediately beside the radar. It is DRAWN,
/// never written as a fraction. Its whole extent is the store's CAPACITY and it is SEGMENTED by
/// where the room came from: a GREEN section on top for the base store, and a GOLD section added at
/// the BOTTOM for each deployed scavenger's 20. A white bar rises from the bottom to the scrap the
/// side HOLDS, and that number is written below it. The layout is the whole decision, so the scene
/// only paints rectangles and a test can read every one of them.
///
/// THE CHUNKS (Harri, 2026-11-03): "each 20-unit scrap storage-addition should be easily visible by
/// having a small gap between the bars. That way I can quickly count if I have lost any deployed
/// scavengers." So the column is not two slabs but SEPARABLE SEGMENTS with a gap between each: one
/// green chunk for the base store, then one gold chunk for every `kStoreCapacityPerDeployed` of
/// room a deployed scavenger added. Counting the gold chunks counts the scavengers. The white bar
/// is cut by the same gaps, so a full bank does not paint over them.
struct GaugeChunk {
    GaugeRect box;    ///< where the chunk is drawn, the gap already taken out of it
    bool gold{false}; ///< a deployed scavenger's room (else the base store)
};

struct StoreGaugeLayout {
    GaugeRect frame;  ///< the rounded pill that holds the gauge
    GaugeRect column; ///< the whole extent: capacity, green over gold
    GaugeRect green;  ///< the base store, the top of the column
    GaugeRect gold;   ///< the deployed scavengers' room, the bottom of the column (empty if none)
    GaugeRect level;  ///< the scrap held, from the column's bottom up
    GaugeRect number; ///< where the number is written, under the column
    std::string text; ///< the number: the scrap held
    /// The segments, top to bottom: the base chunk, then a gold one per deployed scavenger. Each is
    /// its share of the column less `kGaugeChunkGapCells` of gap against a neighbour.
    std::vector<GaugeChunk> chunks;
    /// The white bar as the chunks cut it: one rectangle per chunk the scrap reaches, in the
    /// chunk's own width inset, so the gaps stay open all the way up.
    std::vector<GaugeRect> fills;
    /// The pill as horizontal bands that tile `frame` exactly, split at every chunk edge. The pill
    /// is never ONE tall quad: a tall quad's depth sags in its middle and, behind a layer only a
    /// hair nearer, hides it there (measured: the map-mode gauge lost two thirds of its sections).
    /// Bands no taller than a chunk cannot sag past it.
    std::vector<GaugeRect> pill;
};

/// The gauge's inks. Green is the base store and gold the deployed scavengers' room (his words:
/// "the green section" and "the yellow section"); the white bar is what is held. The gold is the
/// capture's own dull gold, NOT `kLedgerInk`, so the ledger's amber stays the one amber on screen.
inline constexpr std::array<float, 3> kGaugeGreen{0.20F, 0.72F, 0.42F};
inline constexpr std::array<float, 3> kGaugeGold{0.80F, 0.72F, 0.35F};
inline constexpr std::array<float, 3> kGaugeLevel{1.0F, 1.0F, 1.0F};
inline constexpr std::array<float, 3> kGaugePill{0.25F, 0.35F, 0.80F};
inline constexpr std::array<float, 3> kGaugeInk{0.95F, 0.95F, 1.0F};

/// The gap between the radar's right edge and the gauge, and the pill's width as a fraction of the
/// radar's. The gauge's height is the radar's, and its bottom is the radar's bottom.
inline constexpr float kGaugeGapPixels = 6.0F;
inline constexpr float kGaugeWidthFraction = 0.16F;
/// The pill's padding round the column (a fraction of the pill's width), and the white bar's
/// inset inside the column (a fraction of the column's width), as in his capture.
inline constexpr float kGaugePadFraction = 0.18F;
inline constexpr float kGaugeLevelInsetFraction = 0.22F;
/// The room kept under the column for the number, in text cells (a glyph is 5 cells tall).
inline constexpr float kGaugeNumberCells = 8.0F;
/// The gap between two chunks, in text cells: 1.5 cells is 3 px at 720, enough to read at a glance
/// and small enough that the chunk is still a bar.
inline constexpr float kGaugeChunkGapCells = 1.5F;
/// The pill never narrows below what a three-digit number needs (17 cells) and its padding: the
/// half-size world map is narrower than the radar, and a fraction of ITS width would clip the
/// number.
inline constexpr float kGaugeMinWidthCells = 21.0F;

/// The gauge beside `beside` (the radar's panel, or the map's square when R has swapped them).
/// `capacity` is the store, `band` the gold part of it (`Sim::fast_band`), `held` the scrap in it;
/// `cell` is the HUD's text cell in pixels. `held` above `capacity` draws full.
[[nodiscard]] StoreGaugeLayout store_gauge_layout(const RadarPanel& beside, int capacity, int band,
                                                  int held, float cell);

} // namespace protekt::game
