# LIGHTOVR Authoring Guide

`LIGHTOVR` is Duke-RT's text overlay database for authored ray-traced lighting and smoke behavior. This guide covers the shared file format, load/reload behavior, and in-game light edit workflows. See the [smoke authoring guide](SMOKE-AUTHORING.md) for smoke styles and sources, and the [sector color authoring guide](SECTOR-COLOR-AUTHORING.md) for color profiles, emitter bindings, and room-level tuning.

## LIGHTOVR Authoring and Usage

`LIGHTOVR` is the text overlay database for RT-authored lighting and smoke behavior. The parser scans all mounted root-level `LIGHTOVR` lumps, merges them in load order, and uses last-wins replacement for duplicate rule ids within the same scope.

General workflow:

- mount a loose overlay directory with `-file M:\Raze\overlay`
- place a file at `M:\Raze\overlay\LIGHTOVR`
- use `lightoverlay_reload` console command after edits
- inspect the parsed database with `lightoverlay_dump`
- inspect the resolved current-map view with `lightoverlay_dumpresolved [mapname]`
- inspect normalized round-trippable output with `lightoverlay_dumpnormalized`
- export the normalized database with `lightoverlay_export <path>`

Current top-level `LIGHTOVR` blocks:

- `defaults`
  Placeholder root block. It currently exists for future shared defaults, but no fields are consumed yet.
- `actorrule <id>`
  Global actor-bound analytic light rule. This is the main authored point-light path for sprite-driven actors such as fires, rockets, and similar cases.
- `muzzleflashrule <event_id>`
  Global event-driven muzzle-flash rule. This binds a weapon-fire event id to a transient analytic light definition.
- `sectorcolor <id>`
  Global environmental palette profile for emitter hue, blend strength, output gain, and surface palette neutralization.
- `smokestyle <id>`
  Global reusable smoke appearance and simplified-simulation preset.
- `smokeactorrule <id>`
  Global actor-bound smoke source. This binds a smoke style to a live actor class and controls one-shot, timed, or distance-spaced emission.
- `smokeeventrule <event_id>`
  Global event-driven smoke source for wired gameplay events such as muzzle blasts and hitscan impacts.
- `map <mapname> { ... }`
  Map-local scope for persistent placed lights and overrides.

Current map-local blocks:

- `directional <id>`
  One authored directional fill/shadow light for a map.
- `light <id>`
  A placed analytic point light anchored to a Build/world position, sector, or wall.
- `actoroverride <id>`
  Map-local actor shadow override. This adjusts shadow receive/cast policy without creating a light by itself.
- `emissiveoverride <id>`
  Map-local override for emissive surfaces, used to tune intensity/reach scaling and bind an emitter to a sector-light signal.
- `surfacelight <id>`
  Map-local PT-only visible fixture plus associated analytic point light, usually authored by aiming at a surface in emissive light edit mode and pressing `o`.
- `sectorcolor <id>`
  Map-local color-policy override for one sector, inheriting unspecified hue, strength, and gain from its selected palette profile.
- `smokeemitter <id>`
  Map-local, always-active rectangular smoke source plane. It references a global smoke style and emits while its map is active.

Smoke block fields and behavior are documented in the [smoke authoring guide](SMOKE-AUTHORING.md#parser-field-reference). Sector-color profiles and emitter color-response fields are documented in the [sector color authoring guide](SECTOR-COLOR-AUTHORING.md).

Current light parser fields by block:

- `actorrule`
  `actorclass`, `shadowreceive`, `shadowcast`, `lightshadowcast`, `fullbright`, `emissivestableframes`, `activation`, `tile`, `type`, `color`, `intensity`, `radius`, `range`, `offset`, `nudgefromsurface`, `direction`, `flicker`, `random`, `localspace`
- `muzzleflashrule`
  `color`, `intensity`, `intensityrandom`, `radius`, `radiusrandom`, `delayseconds`, `delayrandomseconds`, `durationseconds`, `durationrandomseconds`, `offset`
- `directional`
  `color`, `intensity`, `direction`, `angularsize`, `shadow`
- `light`
  `type`, `anchor`, `offset`, `direction`, `color`, `intensity`, `radius`, `range`, `flicker`, `colorresponse`, `colorsignal sector`
- `actoroverride`
  `actorclass`, `shadowreceive`, `shadowcast`
- `emissivematerialresponse`
  `tile`, `tilerange`, `texture`, `materialresponse`, `materialresponsemin`, `materialresponsemax`, `visibleglowblend`, `glowblend`
- `emissiveoverride`
  `sector`, `wall`, `tile`, `intensityscale`, `reachscale`, `sectorresponse`, `signal sector`, `responseintensity`, `responsemin`, `responsemax`, `responseinputmin`, `responseinputmax`, `responseintensitymin`, `responseintensitymax`, `responsereachmin`, `responsereachmax`, `materialresponse`, `materialresponsemin`, `materialresponsemax`, `colorresponse`, `colorsignal sector`
- `surfacelight`
  `anchor surface`, `position`, `normal`, `size`, `rotation`, `offset`, `sector`, `wall`, `tile`, `fixture texture`, `fixturetexture`, `fixturematerialresponse`, `type`, `color`, `intensity`, `radius`, `sectorresponse`, `signal sector`, `responseintensity`, `responsemin`, `responsemax`, `responseinputmin`, `responseinputmax`, `materialresponsemin`, `materialresponsemax`, `colorresponse`, `colorsignal sector`
- `sectorcolor`
  `palette` (global) or `sector` (map-local), `color`, `strength`, `gain`, `enabled`, `neutralize`, `source`

Current practical notes:

- duplicate `actorrule` and `muzzleflashrule` ids are global and last-wins
- duplicate `directional`, `light`, and `actoroverride` ids are last-wins within the same map
- actor and map analytic overlays are currently consumed as point lights on the PT path even if extra shape fields such as `range` or `direction` are authored
- `light` `anchor position` and `offset` values are authored in Build/world coordinates; the NRI renderer converts them to path-tracing render coordinates internally
- `surfacelight` `position` and `normal` are authored in path-tracing render coordinates because they are captured directly from the aimed PT surface probe
- `actorrule fullbright on` forces matching actor sprite and voxel surfaces onto the PT fullbright material path so they ignore scene lighting and render at full brightness
- `actorrule shadowcast` controls whether the matched actor geometry occludes direct-light shadow rays, while `lightshadowcast` independently controls whether the actor's analytic light traces shadow rays; when `lightshadowcast` is omitted it inherits `shadowcast`, explicit `on` or `default` enables light shadows, and `off` disables them
- pair `shadowcast off` with `lightshadowcast on` for source effects such as projectiles and explosions whose geometry should not self-occlude but whose emitted analytic light should cast shadows
- `actorrule emissivestableframes <count>` delays sampled-emissive surface admission until matching actor geometry has been stable for that many consecutive frames; visual fullbright still applies immediately
- `actorrule activation surface` is the default actor-owned analytic-light behavior and waits for a matching rendered surface before emitting, while `activation immediate` emits as soon as the live actor/rule exists
- `actorrule nudgefromsurface <distance>` moves an actor overlay point light away from nearby map geometry to reduce wall/floor clipping for impact or surface-adjacent effects
- `actorrule random <min> <max>` adds a per-render-frame random intensity offset to the base intensity and is an alternative to `flicker`
- `actorrule localspace <policy>` opts actor overlay placement into a named local-space policy when the renderer recognizes one; omit it for default world-space behavior
- `actoroverride` is applied after `actorrule`, so explicit per-map shadow overrides win
- `emissivematerialresponse` is global and is applied before map-local `emissiveoverride`, so a specific surface override can still opt out or change the clamp

Minimal example:

```text
LIGHTOVR
{
    actorrule "TrashFire"
    {
        actorclass BurningBarrel
        fullbright on
        type point
        color 1.0 0.52 0.18
        intensity 7.5
        radius 192.0
        random -2.5 2.5
    }

    muzzleflashrule "duke.shotgun.primary"
    {
        color 1.0 0.76 0.35
        intensity 18.0
        intensityrandom 0.92 1.08
        radius 224.0
        radiusrandom 0.95 1.05
        delayseconds 0.0
        delayrandomseconds 0.0 0.0
        durationseconds 0.06
        durationrandomseconds -0.01 0.01
        offset 0.0 10.0 0.0
    }

    emissivematerialresponse "SwitchPanels"
    {
        texture "#00707"
        tile 1495
        tilerange 1600 1608
        materialresponsemin 0.0
        materialresponsemax 1.0
    }

    map "E1L1"
    {
        directional "Sun"
        {
            color 1.0 0.97 0.92
            intensity 1.25
            direction 0.3 0.85 -0.4
            angularsize 0.03
            shadow on
        }

        light "LobbyFill"
        {
            type point
            anchor position 1024.0 512.0 -64.0
            color 1.0 0.84 0.6
            intensity 5.0
            radius 256.0
        }

        actoroverride "DeadPigCopNoReceive"
        {
            actorclass PigCop
            shadowreceive off
        }

        emissiveoverride "BathroomSwitchEmitter"
        {
            sector 32
            wall 192
            tile 1287
            intensityscale 1.0
            reachscale 1.0
            sectorresponse on
            signal sector 31
            responseintensity 1.0
            responsemin 0.25
            responsemax 3.0
            responseinputmin 0.20
            responseinputmax 0.65
            materialresponsemin 0.0
            materialresponsemax 1.0
        }

        surfacelight "BathroomPanel01"
        {
            anchor surface
            position 470.69 31.99 -646.64
            normal 0.0 -1.0 0.0
            size 32.0 32.0
            rotation 0.0
            offset 0.5
            sector 171
            wall -1
            tile 1495
            fixture texture "#00707"
            fixturematerialresponse on
            type point
            color 1.0 1.0 1.0
            intensity 4.0
            radius 512.0
            sectorresponse on
            signal sector 171
            responseintensity 16.0
            responsemin 0.05
            responsemax 24.0
        }
    }
}
```

## Sector Color and Emitter Response

`sectorcolor "Name"` defines a palette profile at global scope or a sector override inside `map "E1L1" { ... }`. The name identifies the rule; `palette` or `sector` selects its target.

| Field | Values and effect |
| --- | --- |
| `palette` | Global only, required: integer `1..255`. Selects the environmental palette profile. |
| `sector` | Map-local only, required: nonnegative sector index in that map. |
| `color` | Three finite, nonnegative linear RGB values, not all zero. Required for an enabled global profile; optional locally. Ratios define hue and are normalized for luminance; values above `1` are allowed. Use `gain` for brightness. |
| `strength` | Finite `0..1`: `0` retains original emission color, `1` fully replaces its hue. Global default `1`; local omission inherits the profile. |
| `gain` | Finite number `>= 0`: `0` extinguishes emission, `1` preserves output, values above `1` boost it. Global default `1`; local omission inherits the profile. |
| `enabled` | `on` or `off`, default `on`. Disables/enables this profile or sector response. Local `off` restores original surface palettes and blocks automatic color propagation through the sector. |
| `neutralize` | `on` or `off`, default `on`. `on` removes configured environmental coloring from eligible world surfaces so illumination supplies the color; `off` retains the old palette coloring alongside the lighting response. Both the palette profile and local policy must permit neutralization. Weapon HUD tint is preserved. |
| `source` | Map-local `auto`, `floor`, or `ceiling`; default `auto`. Auto prefers a configured floor palette, then a configured non-sky ceiling palette. Explicit floor/ceiling selects only that surface's palette; ceiling may read a sky palette without removing its sky texture coloring. Accepted but unused globally. |

Local `color`, `strength`, and `gain` inherit independently when omitted. Explicit local RGB works without a matching palette profile; in that case omitted strength/gain default to `1`. Later applicable rules targeting the same palette or sector win; separate sector blocks do not accumulate fields.

Strength and gain affect visible fixtures and their emitted light together. For example, `strength 0.45` blends 45% replacement hue with 55% original emission, then `gain 0.90` scales the result to 90% output. `strength 0` still applies gain and palette neutralization.

`colorresponse` belongs to `light`, `surfacelight`, and `emissiveoverride`:

| Value | Effect |
| --- | --- |
| `auto` | Apply the resolved sector hue, strength, and gain. |
| `on` | Identical to `auto`; does not force full strength or override a disabled sector/feature. Exported as `auto`. |
| `off` | Bypass the sector-color treatment, including its gain. Other brightness controls still apply. |
| Omitted | Automatic by default; omission in a later emitter override preserves an earlier explicit setting. |

Optional `colorsignal sector N` selects a nonnegative sector index independently of the brightness `signal sector`. Without it, color uses the brightness signal sector if specified, otherwise the emitter's own sector. Automatic association can borrow color through up to two open, unmasked portals; fewest hops wins, then lowest sector index. Explicit `colorsignal` never searches neighbors. Intrinsic actor emission keeps its own color.

Reload external edits with `lightoverlay_reload`; inspect effective settings with `nri_ptsectorcolordump N`. See the [sector color authoring guide](SECTOR-COLOR-AUTHORING.md) for complete examples and in-world editing.

## Muzzle Flash Rule Breakdown

`muzzleflashrule <event_id>` defines a transient analytic light that is triggered by a weapon-fire event. The event id is matched case-insensitively against the gameplay emitters wired into the renderer bridge.

- `color <r> <g> <b>`
  Base RGB light color.
- `intensity <value>`
  Base peak intensity before per-shot randomization.
- `intensityrandom <min> <max>`
  Multiplier range applied once per shot to the base intensity.
- `radius <value>`
  Base light radius before per-shot randomization.
- `radiusrandom <min> <max>`
  Multiplier range applied once per shot to the base radius.
- `delayseconds <seconds>`
  Base delay before the flash becomes visible.
- `delayrandomseconds <min> <max>`
  Extra randomized delay range, resolved once per shot.
- `durationseconds <seconds>`
  Base visible lifetime of the flash after activation.
- `durationrandomseconds <min> <max>`
  Extra randomized lifetime range, resolved once per shot. A single authored value in normalized output becomes a symmetric signed range.
- `offset <x> <y> <z>`
  Local event-space offset from the emitted weapon origin.

Current runtime behavior:

- each shot resolves one randomized peak intensity and one randomized radius
- each shot also resolves randomized delay and duration in real seconds
- the light stays off until the resolved delay expires
- when it activates, it starts at the resolved peak intensity
- it then fades to zero over the resolved duration using an expo-out easing curve
- the transient slot topology stays stable so repeated shots do not churn PT light-history topology

Useful muzzle-flash diagnostics:

- `lightoverlay_dumpresolved [mapname]`
  Confirms that the `muzzleflashrule` was parsed and resolved.
- `nri_ptmuzzleflash_test <rule_id>`
  Queues a synthetic muzzle-flash event against a resolved rule id.
- `nri_ptstatus`
  Prints analytic-light counts, including muzzle-slot counts, once PT is active in-level.


## Actor Light Edit Mode

The actor light editor is a runtime helper for authoring global `actorrule` placeholders against live actors. It writes only to a writable loose mounted `LIGHTOVR`; archive-backed sources such as `.pk3`, `.zip`, `.wad`, `.grp`, and similar mounted bundles remain readable but are intentionally not edit targets. Actor and map light edit modes always start disabled on launch and must be enabled at runtime.

Launch with a loose overlay mount so the editor has a file it can rewrite:

```powershell
build\terminal-ninja\raze.exe -nosound -file M:\Raze\overlay +set vid_preferbackend 4 +set nri_api d3d12 +map e1l1
```

Recommended setup:

- create or mount a loose overlay directory, for example `M:\Raze\overlay`
- place the writable file at `M:\Raze\overlay\LIGHTOVR`
- start a live map with the RT backend active
- run `nri_ptactorlighteditwritable` to confirm which mounted `LIGHTOVR` path will be used for writeback
- run `nri_ptactorlighteditmode 1` to enable edit mode

While edit mode is enabled:

- aiming at an actor shows the actor class through the native pickup-style notify path
- `p` prints the current target data; actor hits print class/index/position/sector/state details, and surface hits route through the RT surface-probe status hook
- `o` on an actor creates a placeholder global `actorrule`, writes the normalized database back to the mounted loose `LIGHTOVR`, and reloads overlays immediately
- `l` reloads `LIGHTOVR` without editing the file, useful after changing the file externally
- `nri_ptactorlightedittarget` performs a one-shot target sample and prints the current actor/surface/miss classification

The placeholder rule created by `o` uses the actor class as its id base and starts as a point light with warm color, `intensity 8.0`, `radius 96.0`, and zero offset. Edit the generated rule in the mounted `LIGHTOVR`, then press `l` or run `lightoverlay_reload` to apply the tuned values.

Disable edit mode with:

```text
nri_ptactorlighteditmode 0
```

## Map Light Edit Mode

The map light editor is a runtime helper for placing map-scoped `light` rules at explicit world positions. It uses the same writable loose-mounted `LIGHTOVR` writeback rules as actor light edit mode.

Recommended setup is the same as actor light editing, then run:

```text
nri_ptmaplighteditmode 1
```

While map light edit mode is enabled:

- a white point-light preview floats in front of the local camera
- `[` moves the preview closer to the camera, clamped at distance `0`
- `]` moves the preview farther from the camera
- `p` writes a map-local `light` rule at the preview position, with `type point`, `anchor position`, white color, `intensity 1.0`, `radius 200.0`, and no flicker field, then reloads `LIGHTOVR`
- `o` sets the map's active `directional` rule to the player camera look direction, preserving existing directional color/intensity/shadow/angular-size fields when one already exists
- `l` reloads `LIGHTOVR` without editing the file

Disable edit mode with:

```text
nri_ptmaplighteditmode 0
```

## Emissive Light Override Edit Mode

The emissive light editor writes map-local `emissiveoverride` rules for surfaces that are already active PT emitters. It uses the same writable loose-mounted `LIGHTOVR` writeback rules as the actor and map light editors. The mode is intentionally non-persistent and always starts disabled on launch.

Enable it at runtime with:

```text
nri_ptemissivelighteditmode 1
```

While emissive light edit mode is enabled:

- `p` creates or updates a map-local `emissiveoverride` for the aimed active emitter
- `c` creates a map-local `sectorcolor` override for the aimed sector, preserving an existing override; see the [sector color editing guide](SECTOR-COLOR-AUTHORING.md#editing-a-room-in-game) for tuning controls
- `o` creates a map-local `surfacelight` for the aimed wall, ceiling, or floor, even if the aimed surface is not already emissive
- `Ctrl+o` creates a map-local `surfacelight` at the aimed surface using the most recently placed or edited `surfacelight` as a template
- the generated rule targets the surface with the current sector, wall, and renderer texture id when available
- new rules start with `intensityscale 1.0`, `reachscale 1.0`, `sectorresponse on`, and `signal sector <aimed sector>`
- new rules copy the current `nri_ptsectoremissionintensity`, `nri_ptsectoremissionmin`, and `nri_ptsectoremissionmax` values into `responseintensity`, `responsemin`, and `responsemax`
- new `surfacelight` rules default to `size 32.0 32.0`, `offset 0.5`, white `color 1.0 1.0 1.0`, `intensity 4.0`, and `radius 512.0`
- `l` reloads `LIGHTOVR` without editing the file
- nearby sector-emission response changes produce short notify messages naming the affected sector and whether the response is boosted, dimmed, or neutral
- nearby sector surface-lighting changes also produce notify messages, even when no active emitter is currently bound to that sector
- `nri_ptemissivelighteditnotifyrange` controls the player-relative range for those sector-change notify messages; the default is `2048.0`

Edit the generated `signal sector` when an emitter needs to follow a different sector's switch state, then press `l` to reload.

`surfacelight` rules use the same sector-response fields as `emissiveoverride`, but they create their own PT-only fixture quad and an associated analytic point light. The visible fixture texture defaults to `nri_ptsurfacelighttexture`. The initial dimensions, offset, color, intensity, radius, and sector-response default come from `nri_ptsurfacelightwidth`, `nri_ptsurfacelightheight`, `nri_ptsurfacelightoffset`, `nri_ptsurfacelightred`, `nri_ptsurfacelightgreen`, `nri_ptsurfacelightblue`, `nri_ptsurfacelightintensity`, `nri_ptsurfacelightradius`, and `nri_ptsurfacelightsectorresponse`. The width, height, offset, and color placement cvars are runtime-only editor defaults, so stale config values do not override the built-in 32x32 white fixture default.

Placed `surfacelight` rules can also be edited in-place while `nri_ptemissivelighteditmode 1` is active. Aim at a placed `surfacelight` when possible; if the crosshair probe misses the generated fixture surface, the editor falls back to the most recently placed or successfully edited `surfacelight` on the current map.

Use `Ctrl+o` to repeat-place a tuned fixture. The repeated rule gets a fresh id, position, normal, sector, wall, tile, and signal sector from the newly aimed surface. It copies the active `surfacelight` settings for size, rotation, offset, fixture texture, fixture material response, light type, color, intensity, radius, sector-response enabled state, response curve fields, material-response clamps, and color-response enabled state. Its explicit color source resets to automatic association at the new location.

Surface-light edit hotkeys:

- mouse wheel up/down rotates the fixture around its surface normal in 15 degree steps and writes `rotation`
- left/right arrow decreases/increases `size` X in 4 unit steps
- up/down arrow increases/decreases `size` Y in 4 unit steps
- `,` and `.` decrease/increase `intensity` in 0.5 steps
- `;` and `'` decrease/increase `radius` in 32 unit steps
- `[` and `]` cycle the fixture texture

The surface-light texture cycle is:

```text
#00124, #00701, #00702, #00703, #00707, #00708, #01206, #00705, #00706,
#00704, #00126, #00120, #00121, #00122, #00123, #00127, #00128
```

Each hotkey edit writes the normalized `LIGHTOVR` file, prints the edited rule's current size/rotation/texture/intensity/radius through the native notify path, and reloads overlays immediately.

For switch sectors whose "on" and "off" values are both below the global sector-emission neutral point, add `responseinputmin` and `responseinputmax`. When both fields are present, the raw sector signal maps directly from `responseinputmin` -> `responsemin` to `responseinputmax` -> `responsemax`, instead of using the global neutral response curve. The edit-mode sector-change message prints this authoring value as `signal=...`.

Visible emissive material response is opt-in. For broad texture-based cases, add a global `emissivematerialresponse` rule with any mix of `texture`, `tile`, and `tilerange` selectors. `texture` matches the surface texture name case-insensitively and ignores a filename extension, so `#00707.PNG` can match a probed texture name like `#00707`; `tile` and `tilerange` match renderer texture ids like the existing `emissiveoverride tile` field. `materialresponsemin` and `materialresponsemax` clamp only the material's visible/direct/indirect emission, so a fixture can cast boosted light through `responsemax` while its visible panel stays within an off-to-normal range such as `0.0` to `1.0`; if either clamp is omitted, the corresponding `nri_ptsectoremissionmaterialmin` or `nri_ptsectoremissionmaterialmax` cvar supplies the fallback. `visibleglowblend <scale>` tunes visible glowmap blending for matched materials; `glowblend` is accepted as an input alias, and normalized output writes `visibleglowblend`. Map-local `emissiveoverride` entries are applied after the global texture rule; use `materialresponse off` or per-rule `materialresponsemin`/`materialresponsemax` there when one surface needs different behavior.

Disable edit mode with:

```text
nri_ptemissivelighteditmode 0
```
