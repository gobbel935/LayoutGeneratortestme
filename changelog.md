# Changelog

## v1.5.0
- **Weights section**: Added spawn rate multipliers for blocks, pads, rings, and portals, each with a global weight plus subcategories that stack multiplicatively:
  - Blocks: Jump, Fall, Platform, Spider, Breakable
  - Pads: Spider
  - Rings: Spider, Dash, Black
  - Portals: Gravity, Size, Speed, Gamemode
  - Configurable beat/half-beat placement chances and initial tap balance.
- **BPM limit removed**: BPM setting no longer has an upper cap (minimum remains 0.01).
- **Developer features**:
  - `[DEV] Verbose logging` — logs every fish placement with name, ID, position, and tap to the Geode console.
  - `[DEV] Always place` — bypasses beat/half-beat gating and the canPlaceNextFrame guard, placing an object every frame.
  - `[DEV] Disable deduplication` — skips the proximity check that prevents duplicate objects.
  - `[DEV] Disable trail interference check` — skips overlap detection against the player's recent trail.
  - `[DEV] Freeze generation` — pauses object placement while keeping the playtest running; trail is still recorded.
  - `[DEV] Lock tap balance` — overrides the tap balance with a fixed value every frame.
  - `[DEV] Force fish by name` — forces a specific fish to always be selected, bypassing all state/tap/tag checks.
  - `[DEV] Bounds debug markers` — places visible markers at the floor and ceiling bounds every 10 frames.
  - `[DEV] Frame-perfect spikes` — strips the hardcoded ~8x14 anti-grazing buffer that was previously applied to spike placement regardless of the Spike Margin setting, so Spike Margin = 0 can now mean an actual zero-buffer, frame-perfect gap.
  - `[DEV] Spike hitbox width/height override` — lets you dial the buffer below the previous hardcoded 8/14 instead of just removing it outright, for precise tuning down toward 0.
  - `[DEV] TPS-adaptive timers` (+ `[DEV] TPS-adaptive reference rate`) — internal tick timers are rescaled against your actual measured physics tick rate (TPS), not display FPS. The two are decoupled: `LayoutGeneratorLayer::update()` runs once per rendered frame, while the player's physics tick separately and can run faster or slower than your display refresh rate, including past 240 TPS. This is measured directly from how many physics ticks land in `m_queuedTrail` per render frame, so it works at any TPS with no upper bound.
- **Fixes**:
  - Fish ID counter (`m_fishId`) is now reset to 0 on each new build start.
  - `createObject` return value is now null-checked before use in `placeFish`.
  - Tap balance is now clamped to `[-20, 20]` to prevent runaway values.
  - Initial tap balance now reads from the `weight-tap-balance-init` setting instead of being hardcoded to 3.
  - Beat/half-beat placement chances now read from settings instead of being hardcoded to 15/16 and 3/4.

## v1.4.0
- Added jump indicator toggle
- Added experimental gameplay toggle
- Added free camera mode toggle
- Spider trail fix
- Various bug fixes

## v1.3.0
- Added spike boundary system (v2)
- Added debug trail
- Improved tap balance system

## v1.2.0
- Added use-player-clicks mode
- Added object whitelist setting
- Improved gamemode bounds detection

## v1.1.0
- Added BPM sync
- Added block platforms
- Improved placement logic

## v1.0.0
- Initial release
