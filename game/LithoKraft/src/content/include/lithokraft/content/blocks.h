// LithoKraft's blocks: ids, game flags, sim roles and the registry the game hands to the engine.
// The engine (litholattice::BlockRegistry) only stores these rows; everything they mean is here.
#pragma once
#include <cstdint>
#include "lithokraft/engine.h"
#include "litholattice/world/block.h"

namespace lithokraft {

// Block ids: resolved from the registry at startup (blocks.cpp), keyed by string id
// ("lithokraft:pulsor"). Numeric ids are auto-assigned in registration order -- vanilla
// registration is append-only, so save ids stay stable. Never hardcode a number here;
// to add a block, append one r.add() row in blocks.cpp and one extern const below.
extern const BlockId kWaferSilicon;
extern const BlockId kRedTrace;
extern const BlockId kBlueTrace;
extern const BlockId kRedTorch;   // outputs only into red trace
extern const BlockId kLamp;
extern const BlockId kGrass;
extern const BlockId kDirt;
extern const BlockId kStone;
extern const BlockId kBlueTorch;  // outputs only into blue trace
// Preview-build components (art batch 2). Directional ones are aimed on placement.
extern const BlockId kRepeater;
extern const BlockId kThresholdor;
extern const BlockId kRedirector;
extern const BlockId kPulsor;
extern const BlockId kMemor;
extern const BlockId kFastLatch;
extern const BlockId kSwitch;
extern const BlockId kButton;
extern const BlockId kKeyboard;
extern const BlockId kColorLamp16;
extern const BlockId kColorLamp32;
extern const BlockId kColorLamp64;
extern const BlockId kSiliconSlab;
extern const BlockId kStoneSlab;
extern const BlockId kDirtSlab;
extern const BlockId kToneBlock;
extern const BlockId kRedVia;   // hollow vertical wire tube: links only straight up/down, lossless
extern const BlockId kBlueVia;
extern const BlockId kGlass;    // non-conductive, see-through; cuts wire stairs past it
extern const BlockId kCatchor;
extern const BlockId kFrostedGlass;  // like glass, diffused look
extern const BlockId kCeramic;
extern const BlockId kSignalTransmittor;  ///< wireless signal source (trace -> signal)
extern const BlockId kSignalReceivor;     ///< wireless signal sink (signal -> trace)
extern const BlockId kDish;               ///< long-range receivor; hears channel 0 (the Daemon)
extern const BlockId kOverclockor;
extern const BlockId kPortOR;
extern const BlockId kFusor;  ///< Fusor block (G-L2: this line previously carried garbled
                              ///< copy-paste fragments of five other blocks' doc comments)
// Wafer biome blocks (2026-10-11, procgen pass). Texture keys reuse existing art
// until CloudAtlas does the biome art pass; geometry and placement come first.
extern const BlockId kTraceTrunk;       ///< trace-tree trunk (via-like)
extern const BlockId kTraceBranch;      ///< trace-tree branch
extern const BlockId kTraceTipRed;      ///< glowing red branch tip (emissive via appearance)
extern const BlockId kTraceTipBlue;     ///< glowing blue branch tip (emissive via appearance)
extern const BlockId kCapacitorCan;     ///< capacitor-grove trunk (squat cylinder read)
extern const BlockId kResonatorCrystal; ///< resonator spire crystal
extern const BlockId kDeadSilicon;      ///< dead-zone surface: grey, unpowered
extern const BlockId kFossilTrace;      ///< fossilized dead trace (underground ruins)
extern const BlockId kBrokenVia;        ///< broken via stub (underground ruins)
// Biome surface blocks (2026-10-11, texture stub pass) — per-biome ground palettes.
extern const BlockId kScribeSilicon;    ///< dicing plains surface (die-grid scribe lines)
extern const BlockId kDenseSilicon;     ///< capacitor grove ground (dark dense metallic)
extern const BlockId kFleckedSilicon;   ///< resonator spire ground (quartz-flecked)
// Structure blocks (2026-10-11, procgen structures pass).
extern const BlockId kDeadMemoryCell;   ///< dead memory-bank cell: inert (no sim role), memor-look stub texture
// ChipOR (2026-10-11, workstream 4): the printed chip. kChipOR is the plain solid
// chip; kChipORViewport is the trophy-case variant with a transparent top - peer
// inside and see the miniaturized circuit (see content/chipor_viewport.h).
extern const BlockId kChipOR;           ///< ChipOR: solid printed chip, inert (no sim role)
extern const BlockId kChipORViewport;   ///< ChipOR viewport: transparent top, opaque body, static interior

// Silica (2026-10-11, workstream 7): the Wafer's grass equivalent -- silicon
// fern ground cover -- plus tilled silicon farmland. Ids follow the ChipOR rows.
extern const BlockId kSilicaGrass;      ///< silicon fern ground cover ("silica grass")
extern const BlockId kSilicaTilledSoil; ///< tilled silicon farmland (plantable)

// Structure block (2026-10-12): JD's in-game design tool (Minecraft's structure
// block, renamed). Modes in the cell-state value bits: 0=CORNER, 1=SAVE, 2=LOAD.
// CORNERx2 marks a region; SAVE captures it into the StructureRegistry under a
// name; LOAD places a named structure. Behavior "structure_block".
extern const BlockId kStructureBlock;   ///< structure block: capture/save/load tool

// Certificate table (2026-10-12): JD's certificate-signing station (the
// enchanting-table analogue). Signing will spend the "static" stat and apply
// "Certificate of X" effects to items. THIS PASS IS THE BLOCK ONLY: the
// signing mechanic binds later through the "certificate_table" behavior
// (components/certificate_table.cpp). Behavior "certificate_table".
extern const BlockId kCertificateTable; ///< certificate table: signing station (mechanic wired game-side, B7)
extern const BlockId kEtchOR;           ///< EtchOR: the etch station (block + role; flow in game/etching.cpp, B6)
extern const BlockId kProcgenGrassTuft;   ///< procgen plant crosses (B4): texture-index order 0..6
extern const BlockId kProcgenFern;
extern const BlockId kProcgenDryScrub;
extern const BlockId kProcgenFormulaFlower;
extern const BlockId kProcgenFormulaStar;
extern const BlockId kProcgenFormulaLeaf;  ///< placed by wafer.json groundcover in DicingPlains
extern const BlockId kProcgenFormulaBlob;  ///< placed by wafer.json groundcover in ResonatorSpires
extern const BlockId kFormulaShrub311;     ///< seeded Litho Formula shrubs (JD directive 2026-10-13):
extern const BlockId kFormulaShrub314;     ///< generateShrubSprite seeds 311 / 314, planted at Wafer spawn

// JD materials blocks (2026-10-14, New-Blocks-Materials list): the solid
// block forms of the two interconnect metals (the materials themselves are
// items, see items.h), plus three JD-named blocks. Append-only ids 67-73.
extern const BlockId kCopperBlock;    ///< Copper Block
extern const BlockId kAluminumBlock;  ///< Aluminum Block
extern const BlockId kGravityBlock;   ///< Gravity Block (JD-named; function OPEN -- neutral block, no behavior)
extern const BlockId kSubstrate;      ///< Substrate: the Wafer's low block (its bedrock layer) -- semi-transparent and BREAKABLE (JD, 2026-10-14)
// The insulator family (JD, 2026-10-14): three distinct insulator blocks,
// differentiated by type and color -- Insulatnor, Insulator, Insulatxor.
extern const BlockId kInsulatnor;     ///< Insulatnor (insulator, type 1)
extern const BlockId kInsulator;      ///< Insulator (insulator, type 2)
extern const BlockId kInsulatxor;     ///< Insulatxor (insulator, type 3)

/// Game flags (BlockDef::flags, above the engine's kFirstGameFlag).
constexpr uint32_t kPowerable = uint32_t(kFirstGameFlag) << 0;  // can be powered by wire or a torch below (then turns torches on it off)
constexpr uint32_t kCutsWire = uint32_t(kFirstGameFlag) << 1;   // breaks wire climbing/descending past it
// Nav classification flags (P0, 2026-10-14): read by AiHost's NavRules
// builder (gameNavRules) to feed the engine WalkCache. NO block row carries
// either flag yet -- there is no ladder/vine or fluid content in the game --
// so today they classify nothing; they exist so the first climbable/fluid
// block is a data change, not a code change.
constexpr uint32_t kGameClimbable = uint32_t(kFirstGameFlag) << 2;  // mobs may climb it (ladder/vine class)
constexpr uint32_t kGameFluid = uint32_t(kFirstGameFlag) << 3;      // swimmable fluid cell

/// What a block does in the signal sim (stored in BlockDef::role).
enum class SimRole : uint16_t {
  None, Wire, Torch, Lamp, ColorLamp,
  Repeater, Thresholdor, Redirector, Pulsor, Memor, FastLatch, Switch, Button, Keyboard, Tone, Catchor,
  SignalTransmittor, SignalReceivor, Dish, Overclockor, LithographOR, ChipOR, PortOR, Fusor,
  EtchOR,  // appended 2026-10-13 (wiring pass B6): never insert mid-enum -- values are registry data
};
constexpr SimRole roleOf(const BlockDef& d) { return SimRole(d.role); }
/// Lets content code compare the engine's raw BlockDef::role with a SimRole (found by ADL).
constexpr bool operator==(uint16_t role, SimRole r) { return role == uint16_t(r); }

/// Strongest signal (wire strength, component output).
constexpr int kMaxSignal = 64;

/// Radio channels unlocked by progression (4 -> 16 -> 64). The transmittor / receivor cycle
/// sneak+use through 1..unlocked; channel 0 is the Daemon's and never unlocks.
inline uint8_t& radioChannelsUnlocked() {
  static uint8_t n = 4;
  return n;
}
inline void setRadioChannelsUnlocked(uint8_t n) {
  radioChannelsUnlocked() = n < 1 ? 1 : (n > 64 ? 64 : n);
}
/// Transmittor broadcast power in blocks (link budget: range = power + receivor sensitivity).
constexpr int kTransmittorPower = 256;
/// Dish receivor sensitivity in blocks (link budget: range = power + sensitivity).
constexpr int kDishSensitivity = 1024;
/// Jammer radius in blocks (Overclockor + jammer item).
constexpr int kJammerRadius = 64;      ///< base (Overclockor)
constexpr int kJammerRadiusCopper = 32;  ///< Copper Overclockor
constexpr int kJammerRadiusSilver = 64;  ///< Silver Overclockor
constexpr int kJammerRadiusViolet = 128; ///< Violet Overclockor

/// The LithoKraft block registry (built once, thread-safe to read).
const BlockRegistry& blockRegistry();
/// Mutable registry handle for the seal/hot-swap system. Call single-threaded:
/// seal() at boot after mods load, stageBlockUpdate()/refreshStaged() via the
/// debug hot-swap path (SimThread::runBetweenTicks). Never while sealed.
BlockRegistry& mutableBlockRegistry();
/// Mod API: register a new block, returns its numeric id. Call single-threaded during mod
/// init, before any world loads. Vanilla ids are dense from 1; mod blocks append after.
/// Namespace contract (F-M4): `def.id` must be a well-formed `<namespace>:<path>`
/// id in the registering mod's OWN manifest namespace (namespace
/// [a-z0-9_.-]{1,64}, path [a-z0-9_./-]{1,128}); malformed ids are refused
/// (logged, kAir returned). See blocks.cpp for the full ownership contract
/// and its current enforcement limits.
BlockId registerBlock(BlockDef def);
inline BlockDef blockDef(BlockId b) { return blockRegistry().def(b); }
inline SimRole roleOf(BlockId b) { return roleOf(blockDef(b)); }
inline bool isVia(BlockId b) { return b == kRedVia || b == kBlueVia; }
/// Any wire-network cell (wire or via). Vias only talk vertically to their own colour, so code that
/// reads "power from an adjacent wire" uses isFlatWire.
inline bool isWire(BlockId b) { return roleOf(b) == SimRole::Wire; }
inline bool isFlatWire(BlockId b) { return isWire(b) && !isVia(b); }
/// Wire (not vias) must lie on a solid block or hang under one (a ceiling); anything else floats.
template <class Map> bool wireSupported(const Map& w, int x, int y, int z) {
  return blockDef(w.get(x, y - 1, z)).flags & kSolid || blockDef(w.get(x, y + 1, z)).flags & kSolid;
}
inline bool isComponent(BlockId b) { return roleOf(b) != SimRole::None; }
/// Wire channel of a wire / torch (1 = red, 2 = blue; channels never connect).
inline int wireChannel(const BlockDef& d) { return d.channel; }

}  // namespace lithokraft
