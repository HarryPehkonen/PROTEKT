#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

/// THE NUMBERED COMMAND MENU (RB0): the rows along the left of the frame, as a pure function of
/// what is selected and what is armed.
///
/// This is the MODEL and nothing else. It draws nothing, reads no input, and never touches the sim:
/// the caller says what it knows -- which thing is selected, whose it is, what command is armed,
/// what the cursor is over and how much scrap is banked -- and gets back the rows to draw. So the
/// table is unit-tested on a machine with no window, and the renderer's rigs stay cheap.
///
/// Three things about the real menu are structural, and this model exists to keep them from being
/// flattened into a list of strings:
///
///  * A row is a {key, verb} PAIR, and the keys are NOT CONTIGUOUS. The harvester's menu reads
///    1 2 3 4 5, then a visible GAP where 6, 7 and 8 would be, then 9 and 0. Numbering rows by
///    their position in the list is therefore wrong, and the row order is not key order.
///  * A key's verb is CONTEXT-DEPENDENT: `3` is Deploy on a recycler, Scavenge on a harvester and
///    GUARDIAN on a deployed recycler's build list. One key, three verbs -- so the rows cannot be
///    a table indexed by key either.
///  * The TOP row is LIVE STATE, not a binding. It is recomputed every time the menu is built from
///    what is armed and what the cursor is over: `SPACE SELECT` when a command is armed on
///    something selectable, `SPACE Deploy` once `3` has armed the deploy, and just `SPACE` when
///    neither applies. It is a READOUT -- pressing SPACE does whatever the row currently names --
///    and it is never a second binding for an action that also has a key of its own.
///
/// The dimming carries two meanings under one look, which is why a row carries the reason and not
/// just a flag: a build row dims because it is UNAFFORDABLE (there is not enough scrap banked for
/// it), and an action row like `Select Unit` or `Pick Me Up` dims because it DOES NOT APPLY to this
/// selection. Same ink, two causes, and a test can tell them apart only if the model says which.
///
/// A menu is a MODE. Inside it the digits are the verbs; in normal mode they address buildings
/// (`1` recycler, `2` factory, `3` armory) once those exist. RB0 routes no input and knows nothing
/// about the mode -- it hands back rows, and whether they are drawn is the caller's question --
/// so the two meanings of a digit can never be conflated in one place.
namespace protekt::game {

/// What the menu is about: the thing that is selected.
///
/// Only the subjects whose menus have been READ off the reference (the user's own BattleZone II
/// captures, 2026-10-24) exist here. A thing with no verbs of its own -- a lone deployed turret --
/// still gets a menu, but it collapses to the state row alone (see `menu_rows`). A subject that is
/// not listed is `None`: no menu at all, which is what a selection-less frame must get.
enum class MenuSubject : std::uint8_t {
    None = 0,   ///< nothing is selected: no menu, and no rows
    Recycler,   ///< a recycler / matriarch that is not yet deployed
    Harvester,  ///< a harvester / scavenger
    Deployment, ///< a DEPLOYED recycler: the build list (and `~ DROP OFF`)
    Turret,     ///< a lone deployed turret: the state row alone
    /// A selected VEHICLE -- a tank, hover tank, mortar, rocket or scout. The plan's measured shape
    /// (2026-10-26) is ONE table for every vehicle kind: only the state row's default verb differs
    /// between a tank and a scout, and this model leaves that to the caller's target readout.
    Vehicle,
};

/// The command that is currently armed, if any. Arming is a two-key flow: the menu's own row picks
/// the command, and SPACE confirms it -- `3` arms the deploy, the player points at a good spot,
/// SPACE places it. So the armed command is an INPUT to the model (the state row names it) and
/// never a verb of its own.
enum class MenuCommand : std::uint8_t {
    None = 0, ///< nothing armed: SPACE selects, or does nothing
    Deploy,   ///< `3 Deploy` is armed: SPACE deploys the recycler at the ground under the reticle
    Build,    ///< a build row is armed (see `ArmedCommand::item`): SPACE places that item
};

/// What is armed, and -- for a build row -- the item it named.
struct ArmedCommand {
    MenuCommand command{MenuCommand::None};
    /// `MenuCommand::Build` only: the item the pressed build row named, as drawn on that row. The
    /// state row then names what SPACE will place, the same rule as DEPLOY. The user's capture
    /// resolved the key and the state there but not the wording, so the row re-reads the item the
    /// player just pressed rather than inventing a word for it. Ignored for every other command.
    std::string_view item{};
};

/// What is under the cursor, which is what the state row reads when no command is armed.
enum class MenuTarget : std::uint8_t {
    None = 0,   ///< nothing at all
    Ground,     ///< bare ground: a left click would fire, SPACE would not select
    Selectable, ///< something of your own that SPACE can select
    Hostile,    ///< an enemy: the TRIGGER is what a mark there is for, not SPACE
};

/// Everything the rows are a function of. No file, no clock, no sim: the caller derives each field
/// from what it already holds (the selection, `Interaction`'s armed state, the banked scrap and
/// what the cursor is over).
struct MenuInput {
    MenuSubject subject{MenuSubject::None};
    /// Whose selection it is. It picks the build list's item names: the same kind is a Harvester on
    /// one side and a Scavenger on the other, so a single table of strings cannot serve both.
    sim::Side side{sim::Side::Player};
    ArmedCommand armed{};
    MenuTarget target{MenuTarget::None};
    /// Scrap banked, in the loads C3 counts and a queue spends. Weighed against each build row's
    /// own cost; nothing else in the menu reads it.
    int ore{};
    /// The armed command's distance to the ground under the reticle, whole metres (`nav_metres`),
    /// drawn at the state row's right: `SPACE DEPLOY 68m`. Absent -- nothing armed, or the reticle
    /// off the ground -- draws nothing there.
    std::optional<int> metres{};
    /// SPACE would attack the hostile asset the crosshair is on (`attack_target`): the state row
    /// reads `SPACE Attack`. Ignored while a command is armed -- the armed command owns SPACE. A
    /// selection with no menu of its own (a mixed one) still gets that single row, because the
    /// offer is about the crosshair and the weapons, not about the subject.
    bool attack{false};
    /// Whether MORE THAN ONE unit is selected. It decides one row's ink and nothing else: `~
    /// Select Unit` applies to a group and not to a lone unit (the plan's measured dim rule), so a
    /// single selection draws it dimmed. Defaulted, so a caller that does not know still compiles.
    bool group{false};
};

/// WHY a row is dimmed. The three values are exhaustive on purpose: `None` means the row is NOT
/// dimmed, so `enabled` and `reason` can never disagree.
enum class MenuDim : std::uint8_t {
    None = 0,     ///< not dimmed: the row is live
    Unaffordable, ///< a build row whose cost is more than is banked
    DoesNotApply, ///< an action row that this selection cannot do
};

/// One row of the menu, as drawn: the key pressed and the verb it names, side by side.
struct MenuRow {
    /// The KEY COLUMN, exactly as it is drawn: "SPACE", "TAB", "~", "1".."9", "0". A label, not a
    /// scancode -- `input/bindings.hpp` owns keys and this model routes none of them -- and a VIEW
    /// rather than an owned string: every key is a literal or a `BuildItem`'s own key, both static,
    /// so a drawn row owns nothing it could dangle from.
    std::string_view key{};
    /// The VERB COLUMN, exactly as it is drawn. A build row carries its cost here ("SCOUT 45"),
    /// because the cost is drawn on the row: the name and the number are composed once, in the
    /// model, so the renderer draws two columns of text and never formats a price. Owned, because
    /// that one string IS composed -- it is the only row text that does not come from a table.
    /// Empty is a real state: the state row with nothing to name shows the key alone.
    std::string verb{};
    /// False is the dim ink. Exactly `reason != MenuDim::None`.
    bool enabled{true};
    /// Why it is dimmed. `MenuDim::None` exactly when `enabled`.
    MenuDim reason{MenuDim::None};
    /// The RIGHT COLUMN, exactly as it is drawn: a distance in metres ("17m") on the nav chain's
    /// rows (MENU-NAV), in a column of its own past the widest verb. Empty on every other row, and
    /// then nothing is drawn there.
    std::string right{};

    MenuRow() = default;
    MenuRow(std::string_view key_in, std::string verb_in, bool enabled_in, MenuDim reason_in)
        : key(key_in), verb(std::move(verb_in)), enabled(enabled_in), reason(reason_in) {}
    /// `MenuRow{std::string{...}, ...}` reads fine and is a view into a temporary that dies at the
    /// semicolon -- which the compiler will not mention, because the conversion to `string_view` is
    /// as ordinary as anything. Deleted so it is a COMPILE error instead: every key is a literal or
    /// a `BuildItem`'s own key, and both outlive the row.
    MenuRow(std::string&&, std::string, bool, MenuDim) = delete;
};

/// One row of a deployed recycler's build list. The cost is scrap, the same currency the factory
/// spends (each item's `cost`), and it is what affordability is weighed against.
struct BuildItem {
    std::string_view key{};   ///< the key pressed to choose it, "1".."7"
    std::string_view label{}; ///< the item's name as drawn
    int cost{};               ///< scrap it spends when it is built
    /// The unit kind the row builds, or none for an item no `UnitKind` is yet (the Hauler, Builder,
    /// Healer and Service Pod): such a row is drawn and its key does nothing. A row that names a
    /// kind takes its `cost` FROM the sim's spec table, so the price drawn and the price charged
    /// are one number.
    std::optional<sim::UnitKind> kind{};
};

/// A side's build list, in key order. The PLAYER's is the one the reference capture shows, with its
/// costs; the enemy's is EMPTY until RB5 and the R3 roster fill it, because a per-side item name
/// and a per-kind cost are that slice's and inventing a list here would put names in the mouth of a
/// capture that never showed them. A deployment on the enemy side therefore draws no build rows.
[[nodiscard]] std::span<const BuildItem> build_items(sim::Side side) noexcept;

/// The verbs of the rows that ACT today (RB-DIGITS), written once so the table that draws them
/// (`menu.cpp`) and the layer that routes their digit (`Interaction`) cannot drift apart: a row is
/// identified by its verb, because the same KEY names a different verb on another subject (`1` is
/// Follow Me on a recycler and Harvester on a deployment).
inline constexpr std::string_view kVerbFollowMe{"Follow Me"};
inline constexpr std::string_view kVerbHold{"Hold"};
/// `3 Deploy` on an un-deployed recycler, and the state row's verb once it has armed the deploy.
inline constexpr std::string_view kVerbDeploy{"Deploy"};
/// The selected-vehicle rows that act (RECYCLE and its neighbours), written once so the table and
/// the layer that routes a digit cannot drift apart.
inline constexpr std::string_view kVerbGoToNav{"Go to Nav"};
inline constexpr std::string_view kVerbAttack{"Attack"};
inline constexpr std::string_view kVerbRecycle{"Recycle"};
/// Drawn on a vehicle's menu and inert in this slice: the row is the reference's, the behaviour is
/// not built. Written here so the table reads as one list and not a run of literals.
inline constexpr std::string_view kVerbSelectUnit{"Select Unit"};
inline constexpr std::string_view kVerbPickMeUp{"Pick Me Up"};
inline constexpr std::string_view kVerbBailout{"Bailout"};
inline constexpr std::string_view kVerbScavenge{"Scavenge"};
inline constexpr std::string_view kVerbRotateBuildings{"Rotate Buildings"};
inline constexpr std::string_view kVerbDropOff{"Drop Off"};

/// A distance as a row's RIGHT column draws it: whole metres and an `m`, "68m". The one spelling,
/// for the nav chain's rows, the armed deploy's state row and the attack list alike.
[[nodiscard]] std::string metres_text(int metres);

/// The words every left menu shares, written once for the selection menus here and the nav chain
/// (`nav_marks.hpp`): the state row's key, its `Select`, and the `TAB Close Menu` row.
inline constexpr std::string_view kRowKeySpace{"SPACE"};
inline constexpr std::string_view kRowKeyTab{"TAB"};
inline constexpr std::string_view kVerbSelect{"Select"};
inline constexpr std::string_view kVerbCloseMenu{"Close Menu"};

/// The menu a kind of unit owns, or `None` when its menu was never captured. The sim's `Factory`
/// plays BattleZone II's DEPLOYED recycler (it is the base that builds), so `Deployment` is exact
/// and not a stretch. This is the kind half of the mapping from a selection to a subject;
/// `menu_subject_of` is the selection half.
[[nodiscard]] MenuSubject menu_subject_of_kind(sim::UnitKind kind) noexcept;

/// What the command menu is about, from the selection: the ONE rule from selection to subject, for
/// the frame that draws the menu and the layer that routes its digits alike. Ids that are not in
/// the sim, or not alive, are ignored: a selection is the living ones. Every living selected unit
/// must be the SAME kind and that kind must have a captured menu (a Factory, a Recycler, a
/// Scavenger or a Turret); a mixed selection, an empty one and every other kind get
/// `MenuSubject::None`, because a menu is never invented.
[[nodiscard]] MenuSubject menu_subject_of(const sim::Sim& sim,
                                          std::span<const sim::UnitId> selected) noexcept;

/// The menu a player sees for `in`, from the state row down. Empty when nothing is selected, so a
/// layer that draws these rows draws NOTHING in the state a frame without a menu is in.
///
/// The rows are in DRAWN order, which is not key order and not even ascending by key: the state row
/// and `TAB Close Menu` come first, then the selection's own rows in the reference's order, in
/// which `9` and `0` sit below `5` with the gap above them left open.
[[nodiscard]] std::vector<MenuRow> menu_rows(const MenuInput& in);

} // namespace protekt::game
