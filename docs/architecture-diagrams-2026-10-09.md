# LithoLattice / LithoKraft architecture diagrams (2026-10-09)

These diagrams were drawn from HEAD `bd3bba0` of `D:\LithoLattice` and **refreshed for the SDK split at `2f33e6e`** (sections 1a, 1b, 2b, 2m). The engine now holds no game content; LithoKraft's blocks, signal sim, components, appearance and SFX live in `LithoKraft/src/content` (`lithokraft_content`). Module edges come from the real `#include "litholattice/<module>/..."` lines and the `target_link_libraries` lines in each CMakeLists. Class names and methods come from the public headers. They show the main members only, not every one.

Notation: `*--` means owns (by value or unique_ptr), `o--` means references or shares (reference, raw pointer or shared_ptr), and `<|--` means inherits.

---

## 1. Module dependency graph

### 1a. Engine modules (`litholattice::`, CMake targets `litholattice_*`)

```mermaid
flowchart BT
  core["core<br/>math, json, log, time,<br/>JobPool + JobBatch, TripleBuffer, SpscQueue,<br/>HighResTimerScope"]
  procgen["procgen<br/>noise, graphs, pipeline"] --> core
  ui["ui<br/>widgets, UiRoot, Theme"] --> core
  assets["assets<br/>packs, textures, models, shaders"] --> core
  audio["audio<br/>AudioEngine, Synth"] --> core
  input["input<br/>InputSystem, ActionMap"] --> core
  script["script<br/>ScriptEngine (AngelScript)"] --> core
  world["world<br/>BlockRegistry (instance), chunks, dimensions"] --> procgen
  world --> core
  sim["sim<br/>SimThread, SimConfig,<br/>IWorldSimulation, clock domains"] --> world
  platform["platform<br/>Window, GLFW input backend"] --> input
  render["render<br/>IRenderer, mesher + IBlockAppearance, GL 3.3"] --> world
  render --> ui
  render --> assets
  render_vk["render_vk<br/>Vulkan backend"] --> render
```

### 1b. Game layer and third-party libraries

```mermaid
flowchart BT
  subgraph Engine["LithoLattice engine (src/)"]
    E_render[render / render_vk]
    E_sim[sim]
    E_world[world]
    E_ui[ui]
    E_input[input]
    E_platform[platform]
    E_audio[audio]
    E_script[script]
    E_assets[assets]
  end
  subgraph Game["LithoKraft (LithoKraft/)"]
    G_content["lithokraft_content (the first mod)<br/>blocks registry, SignalSim + components,<br/>SignalWorldSimulation, LithoKraftAppearance, SFX"]
    G_game["lithokraft_game<br/>Game, GameUi, Player, ResourceManager, SoundDirector,<br/>Dimensions, ScriptEditor, actions + input contexts"]
    G_app["lithokraft (exe)<br/>main, selftest, noise_preview"]
  end
  T["litholattice_tests<br/>engine only, fixture game"] --> Engine
  G_app --> G_game
  G_game --> G_content
  G_content --> Engine
  G_game --> Engine
  E_platform -.-> glfw[(GLFW 3.4)]
  E_render -.-> vkh[(Vulkan-Headers + glslang<br/>loader loaded at runtime)]
  E_script -.-> as[(AngelScript 2.38)]
  E_audio -.-> ma[(miniaudio 0.11.25)]
  E_assets -.-> stb[(stb_image)]
```

Every arrow points from the game toward the engine; no engine code includes anything from LithoKraft, and `litholattice_tests` proves the engine builds and runs with no game linked. The former weak spots (H1, H2: game content inside `world`, `sim`, `audio` and `input`) are fixed by the SDK split, see 2m.

---

## 2. Class diagrams per system

### 2a. World and chunks (`world`)

```mermaid
classDiagram
  class DimensionRegistry {
    +add(unique_ptr~Dimension~) Dimension&
    +addAlias(from, to)
    +find(id) Dimension*
    +all()
  }
  class Dimension {
    +string id, name
    +vector~string~ biomes
    +shared_ptr~ColumnGenerator~ generator
    +SetupFn setup
    +ChunkMap chunks
    +bool loaded
    +load(JobPool&, sizeChunks) Spawn
    +unload()
  }
  class ChunkMap {
    -unordered_map columns_ (key to unique_ptr ChunkColumn)
    +column(cx, cz) ChunkColumn*
    +sub(cx, sy, cz) SubChunk*
    +get(x,y,z) BlockId
    +set(x,y,z,BlockId) bool
    +getState / setState
    +surfaceY(x,z) int
    +insert(unique_ptr~ChunkColumn~)
  }
  class ChunkColumn {
    +int cx, cz
    +array of unique_ptr SubChunk subs
  }
  class SubChunk {
    +BlockId blocks[16^3]
    +CellState state[16^3]
    +at(x,y,z) / set(x,y,z,b)
    +index(x,y,z)$
  }
  class ColumnGenerator {
    +create(json, seed, dimId, err)$ shared_ptr
    +generate(cx, cz) unique_ptr~ChunkColumn~
    +resolveMaterial(name)$
  }
  class BlockRegistry {
    +get()$ const BlockRegistry&
    +def(BlockId) BlockDef
    +has(id, flags) / isWire / isComponent
    +findByTexture(name)
  }
  class BlockDef {
    +name, flags, SimRole role, Shape shape, texture
  }
  class MeshInput {
    +cx, sy, cz, epoch, seq
    +BlockId id[18^3]
    +CellState st[18^3]
  }
  DimensionRegistry *-- "many" Dimension
  Dimension *-- ChunkMap
  Dimension o-- ColumnGenerator : shared_ptr
  ChunkMap *-- "many" ChunkColumn
  ChunkColumn *-- "0..16" SubChunk
  BlockRegistry *-- "many" BlockDef
  ColumnGenerator ..> BlockRegistry : resolves materials
  ChunkMap ..> MeshInput : snapshotForMeshing()
```

### 2b. Sim: engine scheduler and LithoKraft's simulation (`sim` + `lithokraft_content`)

```mermaid
classDiagram
  class SimThread {
    -ChunkMap& chunks_
    -unique_ptr~IWorldSimulation~ sim_
    -vector~ClockDomain~ domains_
    -TripleBuffer~SimStats~ stats_
    -TripleBuffer~RayInput~ ray_
    -HandoffQueue~EditCmd~ edits_
    -HandoffQueue~SimEvent~ events_
    -std::thread thread_
    +SimThread(chunks, blocks, simulation, SimConfig, submitMesh)
    +start() / stop()  (idempotent)
    +setPlayerChunk(cx,cz) / setTickRate(tps)
    +setRay(RayInput) / edit(EditCmd)
    +toggleDomain(index) / requestRemeshAll()
    +stats() SimStats
    +drainEvents(out)
  }
  class IWorldSimulation {
    <<interface>>
    +scan(world)*
    +canPlace(world, x,y,z, block) bool
    +onBlockChanged(world, x,y,z, now)*
    +use(world, x,y,z, dirty) bool
    +command(world, SimCommand, dirty)
    +tick(world, TickArea, dirty)*
    +activeCount() size_t
    +takeEvents(out)
  }
  class SignalWorldSimulation {
    <<lithokraft>>
    -SignalSim signal_
  }
  class SignalSim {
    <<lithokraft>>
    +scan / onBlockChanged / tick / use / keyPulse / takeSounds
  }
  class ClockDomain {
    +name, hz, enabled, step()
  }
  class BehaviorCatalog {
    <<lithokraft>>
    +behavior(BlockId) BlockBehavior&
    +component(BlockId) ComponentBehavior*
  }
  class ComponentBehavior {
    <<abstract>>
    +tick(ComponentInputs, state, mem)* ComponentStep
  }
  SimThread *-- IWorldSimulation
  SimThread *-- "many" ClockDomain
  SimThread o-- ChunkMap : owns access while running
  IWorldSimulation <|-- SignalWorldSimulation
  SignalWorldSimulation *-- SignalSim
  SignalSim ..> BehaviorCatalog
  BehaviorCatalog *-- "many" ComponentBehavior
```

### 2c. Render backends (`render`, `render_vk`)

```mermaid
classDiagram
  class IRenderer {
    <<interface>>
    +name()* / deviceInfo()*
    +textureArray()* ITextureArray&
    +setMaterials(BlockMaterials)*
    +loadShaders(ShaderLibrary)* int
    +uploadChunkMesh(ChunkMesh)*
    +clearChunkMeshes()*
    +renderFrame(FrameParams)*
    +stats()* RenderStats
  }
  class GL33Renderer {
    -GlProgram prog_, hudProg_, lineProg_
    -GlTextureArray textures_
    -map meshes_ (key to vao+vbo)
  }
  class VulkanRenderer {
    -unique_ptr~VulkanDevice~ dev_
    -unique_ptr~Swapchain~ swapchain_
    -unique_ptr~RenderPipelines~
    -unique_ptr~VertexArena~ arena_
    -VkTextureArray
    -FrameSlot frames_[2]
    -map meshes_ (key to arena slice)
    -vector~Retired~ retired_
  }
  class VulkanDevice
  class Swapchain
  class RenderPipelines
  class VertexArena {
    +allocate(bytes) optional~Slice~
    +free(Slice) / data(Slice)
  }
  class Buffer
  class Image
  class DeviceHandle~T~ {
    +move-only RAII handle
  }
  class HudUiRenderer
  class IUiRenderer {
    <<interface>>
  }
  class Mesher {
    +meshSubChunk(MeshInput, BlockMaterials)$ ChunkMesh
  }
  IRenderer <|.. GL33Renderer
  IRenderer <|.. VulkanRenderer
  VulkanRenderer *-- VulkanDevice
  VulkanRenderer *-- Swapchain
  VulkanRenderer *-- RenderPipelines
  VulkanRenderer *-- VertexArena
  VertexArena *-- "many" Buffer
  Buffer ..> DeviceHandle
  Image ..> DeviceHandle
  IUiRenderer <|.. HudUiRenderer
  Mesher ..> IRenderer : ChunkMesh feeds uploadChunkMesh
```

`createVulkanRenderer(desc, err)` returns `unique_ptr<IRenderer>` or nullptr, and the app falls back to GL 3.3. The concrete renderer classes live in .cpp files, so the rest of the code sees only `IRenderer`.

### 2d. Textures and assets (`assets` + render texture arrays)

```mermaid
classDiagram
  class PackStack {
    +push(ResourcePack)
    +owner(rel) ResourcePack*
  }
  class ResourcePack {
    +open(dir, err)$ optional
    +name() / dir() / manifest()
    +has(rel) / path(rel)
  }
  class ResourceSet {
    +TextureRegistry textures
    +BlockModelSet models
  }
  class TextureRegistry {
    +add(name, Image, origin, err) optional~TextureId~
    +addPalette(name, Image, origin) PaletteId
    +find(name) / id(name)
  }
  class Image
  class BlockModelSet
  class ShaderLibrary {
    +find(name) ShaderSource*
  }
  class BlockMaterials {
    +face(BlockId, FaceSlot, active) FaceMaterial
    +bindings() / animations() / swatch(id)
  }
  class ITextureArray {
    <<interface>>
    +upload(TextureRegistry)* bool
    +layerCount()* / tileSize()*
  }
  class GlTextureArray
  class VkTextureArray
  class ResourceManager["lithokraft::ResourceManager"] {
    +reload()
    +apply(IRenderer&)
    +materials() shared_ptr~BlockMaterials~
    -mutex
  }
  PackStack *-- "many" ResourcePack
  ResourceSet *-- TextureRegistry
  ResourceSet *-- BlockModelSet
  TextureRegistry ..> Image
  ITextureArray <|.. GlTextureArray
  ITextureArray <|.. VkTextureArray
  ResourceManager *-- PackStack
  ResourceManager *-- ResourceSet
  ResourceManager *-- ShaderLibrary
  ResourceManager o-- BlockMaterials : shared_ptr (mesh jobs hold a copy)
  ResourceManager ..> ITextureArray : apply() uploads
```

Pack order: engine base < game pack < art pack < mods.

### 2e. Input (`input` + `platform`)

```mermaid
classDiagram
  class InputSystem {
    +InputSystem(ActionMap)
    +update(IInputBackend&)
    +processFrame(span~InputEvent~)
    +push(ctx) / pop(ctx) / active(ctx)
    +pressed(a) / held(a) / released(a)
    +value(a) / axis(a, dt)
    +text() / cursorX() / cursorY()
  }
  class ActionMap {
    +defaults()$
    +bindings(a) / setBindings / addBinding
    +analog() AnalogSettings
    +conflicts() / toJson()
  }
  class Binding
  class DeviceState
  class IInputBackend {
    <<interface>>
    +name()*
    +poll(vector~InputEvent~&)*
  }
  class GlfwInputBackend
  class ReplayInputBackend {
    +push(e) / advanceTo(t) / finished()
  }
  class Window {
    +create(WindowDesc)$ unique_ptr
    +input() IInputBackend&
    +shouldClose / swap / setTitle
    +setCursorCaptured / framebufferSize
    +clipboardText / setClipboardText
  }
  class Action {
    <<enum X-macro>>
    Break, Place, Hotbar1..9, ToggleEUV...
  }
  InputSystem *-- ActionMap
  InputSystem *-- DeviceState
  ActionMap *-- "many" Binding
  IInputBackend <|.. GlfwInputBackend
  IInputBackend <|.. ReplayInputBackend
  Window *-- GlfwInputBackend
  InputSystem ..> IInputBackend : polls each frame
  ActionMap ..> Action
```

### 2f. Audio (`audio`)

```mermaid
classDiagram
  class AudioEngine {
    +AudioEngine(sampleRate)
    +post(AudioCommand) bool
    +render(out, frames, ch)
    +setEnabled(bool)
    -SpscQueue~AudioCommand~ queue_ (256)
    -Synth music_, effects_
    -atomic~bool~ enabled_
  }
  class AudioCommand {
    +noteOn(bus, voice, NoteSpec)$
    +noteOff(bus, voice)$
    +playSfx(sfx, gain, strength)$
    +masterGain(g)$
  }
  class Synth {
    +sid3()$ / dmg()$ SynthPreset
    +noteOn(voice, NoteSpec) / noteOff(voice)
    +mixInto(span~float~)
  }
  class Voice
  class Envelope
  class Lfsr
  class VoiceAllocator
  class AudioOutput {
    <<interface>>
    +description()*
  }
  class MiniaudioOutput
  class SoundDirector["lithokraft::SoundDirector"] {
    +play(events, listenerPos)
    +stopAll()
  }
  AudioEngine *-- "2" Synth
  AudioEngine ..> AudioCommand
  Synth *-- "many" Voice
  Synth *-- VoiceAllocator
  Voice *-- Envelope
  Voice *-- Lfsr
  AudioOutput <|.. MiniaudioOutput
  MiniaudioOutput o-- AudioEngine : calls render()
  SoundDirector o-- AudioEngine : post()
```

### 2g. UI, part 1: core (`ui`)

```mermaid
classDiagram
  class UiRoot {
    +UiRoot(Theme)
    +root() Widget&
    +layout(viewport)
    +paint(IUiRenderer&)
    +update(dt)
    +handleEvent(UiEvent) bool
    +setScriptHost(IScriptHost*)
    +setClipboard(IClipboard*)
  }
  class Widget {
    +add~T~(args) T&
    +addChild(unique_ptr) / removeChild / clearChildren
    +find(id) / findAs~T~(id)
    +parent() / children() / ui()
    +setId / setStyleClass
    -vector of unique_ptr Widget children_
    -Layout layout
  }
  class Theme
  class Style
  class Painter
  class IUiRenderer {
    <<interface>>
    +fillRect* / drawText* / drawImage*
    +pushClip* / popClip*
  }
  class IClipboard {
    <<interface>>
    +text()* / setText()*
  }
  class MemoryClipboard
  class IScriptHost {
    <<interface>>
    +callHandler(decl, widgetId)*
  }
  class UiScriptBindings["lithokraft::UiScriptBindings"]
  class HudUiRenderer["render::HudUiRenderer"]
  UiRoot *-- Widget : root
  UiRoot *-- Theme
  Theme *-- "many" Style
  Widget *-- "many" Widget
  UiRoot o-- IScriptHost
  UiRoot o-- IClipboard
  IClipboard <|.. MemoryClipboard
  IScriptHost <|.. UiScriptBindings
  IUiRenderer <|.. HudUiRenderer
  Painter o-- IUiRenderer
```

### 2h. UI, part 2: widget catalogue

```mermaid
classDiagram
  direction LR
  class Widget
  Widget <|-- Panel
  Widget <|-- Label
  Widget <|-- Button
  Widget <|-- Image
  Widget <|-- Checkbox
  Widget <|-- ProgressBar
  Widget <|-- Slider
  Widget <|-- NumberField
  Widget <|-- Slot
  Widget <|-- StackPanel
  Widget <|-- GridPanel
  Widget <|-- ScrollBar
  Widget <|-- ScrollView
  Widget <|-- Splitter
  Widget <|-- TabView
  Widget <|-- ListView
  Widget <|-- TreeView
  Widget <|-- Dropdown
  Widget <|-- Dialog
  Widget <|-- Tooltip
  Widget <|-- Menu
  Widget <|-- MenuBar
  Widget <|-- TextEditBase
  TextEditBase <|-- TextBox
  TextEditBase <|-- TextEditor
  TextEditBase *-- TextBuffer
```

`widget_factory.cpp` builds these trees from the pack's UI JSON.

### 2i. Procgen, part 1: pipeline stages (`procgen`)

```mermaid
classDiagram
  class GeneratorPipeline {
    +Builder(shared_ptr~NoiseGraph~)
    +generate(ChunkInfo) ColumnBuffer
  }
  class StageContext
  class ColumnBuffer {
    +get / set / state / setState
    +layerEmpty(y)
  }
  class IClimateStage {
    <<interface>>
    +sample(ctx)*
  }
  class IBiomeSelector {
    <<interface>>
    +select(climate)*
  }
  class ITerrainStage {
    <<interface>>
    +generate(ctx)*
  }
  class ISurfaceRule {
    <<interface>>
    +apply(ctx)*
  }
  class ICarver {
    <<interface>>
    +carve(ctx)*
  }
  class IFeature {
    <<interface>>
    +place(ctx, rng)*
  }
  class FeatureRegistry {
    +add(name, unique_ptr~IFeature~) / contains
  }
  IClimateStage <|.. GraphClimate
  IBiomeSelector <|.. MultiNoiseBiomeSelector
  ITerrainStage <|.. HeightmapTerrain
  ITerrainStage <|.. DensityTerrain
  ISurfaceRule <|.. LayeredSurface
  ICarver <|.. WormCaveCarver
  ICarver <|.. RavineCarver
  IFeature <|.. OreVeinFeature
  IFeature <|.. DungeonSlot
  GeneratorPipeline *-- IClimateStage
  GeneratorPipeline *-- IBiomeSelector
  GeneratorPipeline *-- ITerrainStage
  GeneratorPipeline *-- ISurfaceRule
  GeneratorPipeline *-- "many" ICarver
  GeneratorPipeline *-- "many" IFeature
  GeneratorPipeline ..> StageContext
  StageContext *-- ColumnBuffer
  FeatureRegistry *-- "many" IFeature
```

### 2j. Procgen, part 2: noise graphs

```mermaid
classDiagram
  class GraphDesc {
    +vector~NodeDesc~ nodes
    +outputs
    +fromJson(json, out, err)$ / parse(text, out, err)$
    +toJson()
  }
  class NodeDesc {
    +name, type, params
  }
  class NoiseGraph {
    +compile(GraphDesc, seed, err)$ shared_ptr~const NoiseGraph~
    +outputIndex(name) / outputNames()
  }
  class GraphContext {
    +bind(PointsView)
    +points()
    -CacheEntry cache
  }
  class INoise {
    <<interface>>
    +sample2* / sample3* / sample4*
    +range()
  }
  class OpenSimplex2
  class Rng
  GraphDesc *-- "many" NodeDesc
  NoiseGraph ..> GraphDesc : compiled from
  NoiseGraph *-- "many" INoise : nodes
  INoise <|.. OpenSimplex2
  GraphContext o-- NoiseGraph : evaluates (per thread)
  GeneratorPipeline o-- NoiseGraph : shared_ptr
  class GeneratorPipeline
```

`loadGenerator(json, seed, resolveMaterial, err)` (generator_loader.cpp) builds a GeneratorPipeline from the worldgen JSON in the game pack.

### 2k. Script (`script`) and its game bindings

```mermaid
classDiagram
  class ScriptEngine {
    +init() bool
    +shutdown()
    +raw() asIScriptEngine*
    +loadDirectory(dir) / loadSource(name, code) / reload()
    +callHook(decl) bool
    +callWithString(decl, arg) bool
    +buildErrors() / logCount() / lastLog()
    -asIScriptEngine* engine_
    -asIScriptModule* module_
    -run(fn, what, arg)
  }
  class asIScriptEngine {
    <<AngelScript>>
  }
  class UiScriptBindings["lithokraft::UiScriptBindings"] {
    +registerApi()
    +callHandler(decl, widgetId)
  }
  class ScriptEditor["lithokraft::ScriptEditor (F10)"]
  class IScriptHost {
    <<interface>>
  }
  ScriptEngine *-- asIScriptEngine : ShutDownAndRelease in dtor
  IScriptHost <|.. UiScriptBindings
  UiScriptBindings o-- ScriptEngine
  ScriptEditor o-- ScriptEngine
```

Modules are hot-swapped: the code builds `game.staging`, and only if that compiles does it discard `game` and rename staging to `game`, so a broken edit leaves the old scripts running.

### 2l. Game composition (`lithokraft::Game`)

```mermaid
classDiagram
  class Game {
    +update(dt, w, h)
    +frameParams(w, h) FrameParams
    +switchDimension(id) bool
    -loadActive()
    -uploadMeshes()
    -playSounds()
  }
  Game *-- ScriptEngine
  Game *-- GameUi
  Game *-- UiScriptBindings
  Game *-- ScriptEditor : unique_ptr
  Game *-- SoundDirector
  Game *-- DimensionRegistry
  Game *-- Player
  Game *-- SimThread : unique_ptr (per active dimension)
  Game o-- IRenderer : &
  Game o-- ResourceManager : &
  Game o-- JobPool : &
  Game o-- InputSystem : &
  class main["main.cpp owns (outer to inner)"] {
    Display(Window + IRenderer)
    ResourceManager
    AudioEngine + AudioOutput
    JobPool, InputSystem, WindowClipboard
    Game
  }
  main ..> Game
```

Objects are destroyed in reverse order of construction. `~Game` stops the sim thread and then calls `pool.waitIdle()`. After that the pool, the renderer and the window are destroyed.

---

### 2m. The SDK seams: engine interfaces and registries, LithoKraft implementations

```mermaid
classDiagram
  class BlockRegistry {
    +add(BlockDef) BlockId
    +def(BlockId) BlockDef&
    +findByTexture(name) optional
    +size()
  }
  class BlockDef {
    +name, flags, shape, tex, behavior
    +role, channel, tier (opaque to the engine)
  }
  class IBlockAppearance {
    <<interface>>
    +active(def, state) bool
    +faceLook(def, state, slot, active) FaceLook
    +meshCustom(CustomBlock, MeshWriter&)
    +bindMaterials(def, model, bindings&)
  }
  class ActionSet {
    +name(id) / find(name) / kind(id)
    +mayShareBinding(a, b)
  }
  class ContextRules {
    +name, opaque, wantsText, activeAtStart
    +vector~ActionId~ actions
  }
  class AudioCommand {
    +noteOn(bus, voice, NoteSpec)$
    +noteOff(bus, voice)$
    +masterGain(g)$
  }
  class LithoKraftContent["lithokraft content: blockRegistry, blockAppearance, actionSet, inputContexts, sfxCommand"]
  BlockRegistry *-- "many" BlockDef
  IBlockAppearance <|-- LithoKraftAppearance
  LithoKraftContent ..> BlockRegistry : fills one instance
  LithoKraftContent ..> ActionSet : declares
  LithoKraftContent ..> ContextRules : declares
  LithoKraftContent ..> AudioCommand : builds notes
  LithoKraftContent ..> IWorldSimulation : SignalWorldSimulation
```

The engine knows only `kAir`, the generic block flags (`kFirstGameFlag` onward is the game's), the shapes and generic shader modes. A new game is a new set of these implementations; see `docs/ARCHITECTURE.md` in the repo.

---

## 3. Sequence: one frame and one tick, across threads

### 3a. Main (game/render) thread frame

```mermaid
sequenceDiagram
  autonumber
  participant Main as Main thread (main.cpp loop)
  participant In as InputSystem
  participant G as Game
  participant Sim as SimThread (mailboxes)
  participant Pool as JobPool queue
  participant R as IRenderer (GL33 / Vulkan)
  participant Snd as SoundDirector → AudioEngine

  Main->>In: update(window.input()) - poll OS events, resolve actions
  Main->>G: update(dt, w, h)
  G->>G: UI update, script hooks, player.update(input)
  G->>Sim: edit(EditCmd) → HandoffQueue
  G->>Sim: setPlayerChunk / setRay → atomic / TripleBuffer
  G->>G: uploadMeshes(): drain meshOut_, drop stale epoch/seq
  G->>R: uploadChunkMesh(mesh) × ≤64 per frame
  G->>Sim: drainSounds()
  G->>Snd: play(events, playerPos) → SpscQueue post
  Main->>G: frameParams(w, h)
  G->>Sim: stats() - TripleBuffer read (highlight, HUD)
  G->>G: ui.paint(hud)
  Main->>R: renderFrame(FrameParams)
  Note over R: Vulkan: wait fence → release retired slices → acquire → reset fence → record → submit → present
  Main->>Main: window.swap(), title and perf log every 0.5 s / 1 s
```

### 3b. Sim tick, mesh workers and audio callback (concurrent)

```mermaid
sequenceDiagram
  autonumber
  participant Sim as Sim thread (SimThread::run)
  participant Sig as SignalSim
  participant CM as ChunkMap (sim-owned)
  participant W as JobPool workers
  participant Out as Game::meshOut_ (HandoffQueue)
  participant Main as Main thread
  participant AC as Audio callback thread

  loop every master step (32..512 Hz)
    Sim->>Sim: reconfigure if TPS / EUV changed (atomics)
    Sim->>Sim: applyEdits(ray_.read()): raycast + Break/Place/Use/KeyPulse
    Sim->>CM: set / setState → markDirty(sub + border neighbours)
    loop fixed-timestep catch-up (≤16 steps, 100 ms lag cap)
      Sim->>Sim: clock domains step (EUV)
      Sim->>Sig: tick(chunks, playerChunk, radius) every worldDiv steps
      Sig->>CM: torches → components → wires → lamps, dirty callbacks
    end
    Sim->>CM: flushDirty(): snapshotForMeshing() per dirty sub-chunk
    Sim->>W: submitMesh(shared_ptr MeshInput) → pool.submit
    Sim->>Main: publishSounds() → HandoffQueue of SoundEvent
    Sim->>Main: stats_.publish() (TripleBuffer)
    Sim->>Sim: waitUntil(next step): sleep, then yield-spin
  end

  par mesh jobs
    W->>W: meshSubChunk(MeshInput, BlockMaterials snapshot)
    W->>Out: push(ChunkMesh)
  and audio
    AC->>AC: AudioEngine::render(): drain SpscQueue, mix Synths (no locks / allocs)
  end
  Main->>Out: drainInto() next frame (see 3a step 8)
```

### 3c. Dimension load and switch (main thread, one time)

```mermaid
sequenceDiagram
  participant G as Game (main)
  participant Sim as SimThread
  participant R as IRenderer
  participant D as Dimension
  participant W as JobPool workers
  G->>Sim: sim_.reset() - stop and join; main owns the chunks again
  G->>R: clearChunkMeshes()
  G->>D: old.unload()
  G->>D: next.load(pool, size)
  D->>W: submit generate(cx, cz) per column
  W-->>D: HandoffQueue of unique_ptr ChunkColumn
  D->>D: busy-wait until all done, insert, setup() → Spawn
  G->>G: ++epoch, submit initial mesh snapshots
  G->>Sim: make_unique SimThread(chunks, ...) + start() - sim owns the chunks
```
