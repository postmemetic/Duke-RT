# LIGHTOVR Authoring Guide

`LIGHTOVR` is Duke-RT's text overlay database for authored ray-traced lighting and smoke behavior. This guide covers the file format, load/reload behavior, smoke sources, and the current in-game light edit workflows.

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
- `smokeemitter <id>`
  Map-local, always-active rectangular smoke source plane. It references a global smoke style and emits while its map is active.

Current parser fields by block:

- `actorrule`
  `actorclass`, `shadowreceive`, `shadowcast`, `fullbright`, `emissivestableframes`, `activation`, `tile`, `type`, `color`, `intensity`, `radius`, `range`, `offset`, `nudgefromsurface`, `direction`, `flicker`, `random`, `localspace`
- `muzzleflashrule`
  `color`, `intensity`, `intensityrandom`, `radius`, `radiusrandom`, `delayseconds`, `delayrandomseconds`, `durationseconds`, `durationrandomseconds`, `offset`
- `smokestyle`
  `density`, `extinction`, `albedo`, `anisotropy`, `radius`, `expansionvelocity`, `lifetime`, `densityhalflife`, `risevelocity`, `velocityrandom`, `velocityinherit`, `buoyancy`, `drag`, `turbulence`, `turbulencescale`, `temperature`, `momentumscale`, `coolinghalflife`
- `smokeactorrule`
  `actorclass`, `ownerclass`, `excludeownerclass`, `trigger`, `activation`, `emitterforeground`, `style`, `count`, `offset`, `spawnradius`, `densityscale`, `radiusscale`, `velocitycone`, `velocityscale`, `intervalseconds`, `pulseamount`, `pulseperiodcadences`, `pulsephase`, `starttime`, `startdistance`, `spacing`, `maxsegmentsperframe`
- `smokeeventrule`
  `style`, `count`, `offset`, `offsetrandom`, `spawnradius`, `densityscale`, `radiusscale`, `velocitycone`, `velocityscale`, `normaloffset`, `direction`
- `smokeemitter`
  `style`, `position`, `normal`, `size`, `rotation`, `offset`, `count`, `intervalseconds`, `spawnradius`, `densityscale`, `radiusscale`, `velocityscale`, `velocitycone`, `maxsegmentsperframe`
- `directional`
  `color`, `intensity`, `direction`, `angularsize`, `shadow`
- `light`
  `type`, `anchor`, `offset`, `direction`, `color`, `intensity`, `radius`, `range`, `flicker`
- `actoroverride`
  `actorclass`, `shadowreceive`, `shadowcast`
- `emissivematerialresponse`
  `tile`, `tilerange`, `texture`, `materialresponse`, `materialresponsemin`, `materialresponsemax`, `visibleglowblend`, `glowblend`
- `emissiveoverride`
  `sector`, `wall`, `tile`, `intensityscale`, `reachscale`, `sectorresponse`, `signal sector`, `responseintensity`, `responsemin`, `responsemax`, `responseinputmin`, `responseinputmax`, `responseintensitymin`, `responseintensitymax`, `responsereachmin`, `responsereachmax`, `materialresponse`, `materialresponsemin`, `materialresponsemax`
- `surfacelight`
  `anchor surface`, `position`, `normal`, `size`, `rotation`, `offset`, `sector`, `wall`, `tile`, `fixture texture`, `fixturetexture`, `fixturematerialresponse`, `type`, `color`, `intensity`, `radius`, `sectorresponse`, `signal sector`, `responseintensity`, `responsemin`, `responsemax`, `responseinputmin`, `responseinputmax`, `materialresponsemin`, `materialresponsemax`

Current practical notes:

- duplicate `actorrule` and `muzzleflashrule` ids are global and last-wins
- duplicate `smokestyle`, `smokeactorrule`, and `smokeeventrule` ids are global, case-insensitive, and last-wins; smoke blocks belong at the root of `LIGHTOVR`, not inside a `map` block
- duplicate `smokeemitter` ids are case-insensitive and last-wins within the same map; map emitters reference a global `smokestyle`
- smoke style references and actor class names must resolve; use `lightoverlay_dumpresolved` to diagnose inert rules
- duplicate `directional`, `light`, and `actoroverride` ids are last-wins within the same map
- actor and map analytic overlays are currently consumed as point lights on the PT path even if extra shape fields such as `range` or `direction` are authored
- `light` `anchor position` and `offset` values are authored in Build/world coordinates; the NRI renderer converts them to path-tracing render coordinates internally
- `surfacelight` `position` and `normal` are authored in path-tracing render coordinates because they are captured directly from the aimed PT surface probe
- `smokeemitter` `position` and `normal` are authored in Build/world coordinates; the editor converts its aimed PT surface result before writing the rule
- `actorrule fullbright on` forces matching actor sprite and voxel surfaces onto the PT fullbright material path so they ignore scene lighting and render at full brightness
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

## Smoke Authoring

Smoke authoring separates the medium from the source:

- `smokestyle <id>` defines reusable optical properties, dissipation, and simplified motion.
- `smokeactorrule <id>` emits a style from matching live actors.
- `smokeeventrule <event_id>` emits a style when gameplay publishes a matching event.
- `smokeemitter <id>` emits a style continuously from a rectangular source plane while its containing map is active.

The first three are global blocks directly inside `LIGHTOVR`; `smokeemitter` belongs inside a `map` block. IDs and style references are case-insensitive. Duplicate IDs use the last loaded definition within their scope. Multiple differently named actor rules may target the same actor class, in which case all matching rules emit.

Smoke requires the NRI/PT renderer and `nri_ptsmoke true`. The same setting is exposed as **Smoke** under **Options → Display Options**. LIGHTOVR parsing, compact cadence state, and injection-command scheduling are CPU-side; deposition, sparse density/optical/velocity/temperature fields, simulation, lighting, dissipation, and residency stay on the GPU.

### Authoring Workflow

1. Define a global `smokestyle` with a unique ID. Start by tuning its optical controls (`density`, `extinction`, `albedo`, and `anisotropy`), then its lifetime and motion controls.
2. Choose the source type that matches the effect. Reference the style by ID from that source; smoke styles are not nested inside source rules.
3. Set source placement and cadence independently of the style. For example, use an actor-rule `offset` and `spacing` for a rocket trail, or a map emitter's `position`, `normal`, and `size` for a stationary fire bed.
4. Mount the loose overlay and run `lightoverlay_reload`. Use `lightoverlay_dumpresolved` to confirm that class names and style references resolved.
5. Generate the effect in game, then tune one category at a time: source amount/size, optical thickness, dissipation, and finally motion. Use `nri_ptsmokereset` between comparisons so old smoke does not obscure the result.

| Desired source | Block to use | Typical use |
| --- | --- | --- |
| A live actor | `smokeactorrule <id>` | Rockets, fires, explosions, and other effects whose position or lifetime follows an actor. |
| A gameplay event | `smokeeventrule <event_id>` | Muzzle smoke and impact puffs emitted by a wired weapon or collision event. |
| A fixed map area | `map <mapname> { smokeemitter <id> { ... } }` | Persistent rectangular sources such as vents, fire beds, and environmental haze emitters. |

The source rule controls **where, when, and how much** smoke is injected. The style controls **what that smoke looks like and how it moves or dissipates after injection**. Reusing one style across several source rules keeps their appearance consistent while allowing each source to have different offsets, cadence, density scaling, and radius scaling.

Actor and event rules also own representation and responsiveness policy:

| Field | Default and accepted values | Effect |
| --- | --- | --- |
| `representation <mode>` | `grid`; `grid` or `analytic` | `grid` routes the emission through sparse-grid admission, simulation, and the grid's recovery path. `analytic` selects the dedicated ungridded carrier path for short-lived effects. Omitting the field preserves existing grid behavior. |
| `queuepolicy <mode>` | `retry`; `retry`, `drop`, or `latest` | Controls what happens when the selected representation cannot accept the emission immediately. `retry` retains existing grid work, `drop` discards work that cannot be presented promptly, and `latest` keeps only the newest work for a stable source. Analytic one-shot effects should use `drop`; they must not use delayed replay. |
| `maxlatencyseconds <seconds>` | omitted; minimum `0` | Optional maximum gameplay/presentation age at first publication. Work older than the bound is discarded instead of appearing belatedly. This clock is independent of smoke simulation debt. Omitting the field leaves the current representation's existing latency behavior unchanged. |
| `analyticcarriers <count>` | `1`; `1` through `8` | Fixed carrier quantity for each analytic emission. Authored particle mass is divided exactly across the carriers; the quantity does not vary with frame time or available GPU headroom. Use multiple carriers to avoid collapsing a broad impact into one opaque kernel. |

Representation is source policy rather than style policy. A style can therefore be reused by grid and analytic rules, but their motion is not identical: grid smoke receives deposition, neighbor transport, thermal buoyancy, and grid turbulence, while analytic smoke uses closed-form carrier expansion, fading, and motion. Prefer analytic representation for immediate, short-lived muzzle or impact feedback; use grid representation for persistent plumes, trails, and fire whose transport matters.

### Minimal Actor Smoke Example

This example creates a reusable fire-smoke style and emits it continuously from a visible `DukeFire` actor:

```text
LIGHTOVR
{
    smokestyle "example_fire_smoke"
    {
        density 2.0
        extinction 0.008
        albedo 0.48 0.46 0.44
        anisotropy 0.10
        radius 8.0
        expansionvelocity 8.0
        lifetime 12.0
        densityhalflife 4.0
        risevelocity 20.0
        velocityrandom 8.0
        velocityinherit 0.0
        buoyancy 4.0
        drag 0.20
        turbulence 12.0
        turbulencescale 32.0
        temperature 4.0
        momentumscale 1.0
        coolinghalflife 4.0
    }

    smokeactorrule "example_fire_source"
    {
        actorclass "DukeFire"
        trigger interval
        activation surface
        emitterforeground on
        style "example_fire_smoke"
        count 6
        offset 0.0 0.0 -24.0
        spawnradius 4.0
        densityscale 1.5
        radiusscale 1.5
        velocityscale 0.0
        intervalseconds 0.20
        spacing 0.0
        maxsegmentsperframe 4
    }
}
```

`activation surface` prevents a prewarmed but hidden fire from emitting until its actor surface has actually appeared. The negative third `offset` component moves the source visually upward because actor positions use Build/world coordinates, where Z increases downward. Positive `risevelocity` and `buoyancy` still mean upward smoke motion.

### Smoke Style Fields

Time and half-life values are seconds of smoke simulation time. Distances are engine world units, velocities are world units per second, and unitless values are identified below. Unless a maximum is listed, the parser accepts any finite value at or above the minimum.

| Field | Default and accepted values | Effect |
| --- | --- | --- |
| `density <value>` | `1.0`, minimum `0` | Initial smoke mass/concentration. In grid mode this combines with rule `densityscale` and `count`. More density deposits more field concentration and makes the source optically thicker; it does not add gravity. |
| `extinction <value>` | `0.04`, minimum `0` | Opacity per unit density. Raising extinction thickens smoke without directly increasing deposited mass or momentum. |
| `albedo <r> <g> <b>` | `0.5 0.5 0.5`; each component clamps to `[0,1]` | RGB fraction of extinction that scatters light. Values near `0` absorb and look dark; values near `1` scatter more incident light. This is not an emissive color. |
| `anisotropy <value>` | `0`; parser range `[-0.99,0.99]`, current runtime range `[-0.95,0.95]` | Henyey-Greenstein phase parameter: `0` is isotropic, positive values favor forward scattering, and negative values favor backward scattering. |
| `radius <units>` | `8.0`, minimum `0.001` | Base source/carrier radius before rule `radiusscale`. In grid mode it participates in the deposition support radius. |
| `expansionvelocity <units/s>` | `0`, minimum `0` | Grid mode injects outward radial momentum, widening the plume. Particle compatibility mode grows carrier radius linearly and dilutes density as volume increases. |
| `lifetime <seconds>` | `2.0`, minimum `0.001` | Hard carrier expiration in particle compatibility mode. It is **not** a hard cutoff for the canonical sparse grid; use `densityhalflife` to control grid dissipation. |
| `densityhalflife <seconds>` | `1.0`, minimum `0.001` | Time for density and optical properties to fall by half. This is the primary fade/dissipation control in sparse-grid mode. Longer values retain a plume; shorter values clear it faster. |
| `risevelocity <units/s>` | `0`, minimum `0` | Direct positive-up launch velocity. In grid mode this is initial source momentum; thermal buoyancy supplies continuing rise. |
| `velocityrandom <units/s>` | `0`, minimum `0` | Stochastic launch speed along a direction selected inside the rule's `velocitycone`. With no usable source axis, the direction is spherical. |
| `velocityinherit <scale>` | `0`, minimum `0` | Fraction of the rule's source velocity inherited by the smoke. Use this for projectile momentum or a moving emitter. |
| `buoyancy <scale>` | `0`, minimum `0` | Style thermal-buoyancy strength. In grid mode it combines with `temperature` and the global grid buoyancy setting to accelerate smoke upward. |
| `drag <rate>` | `0`, minimum `0` | Exponential velocity damping. Low drag lets smoke coast and follow injected momentum; high drag rapidly removes organized motion. |
| `turbulence <strength>` | `0`, minimum `0` | Strength of simplified turbulent motion. Grid mode uses a stable world-anchored curl field; particle mode uses changing random acceleration. |
| `turbulencescale <units>` | `0`, minimum `0` | World-space wavelength of grid turbulence. Small values create tight variation; large values create broad bends and rolls. It is grid-oriented and does not change particle-mode random noise scale. |
| `temperature <value>` | `1.0`, minimum `0` | Initial thermal content used by sparse-grid buoyancy. It has no independent optical effect and is grid-oriented. |
| `momentumscale <scale>` | `1.0`, minimum `0` | Additional multiplier on inherited source momentum in sparse-grid mode. It does not multiply radial expansion, rise velocity, or stochastic launch speed. |
| `coolinghalflife <seconds>` | `2.0`, minimum `0.001` | Time for thermal buoyancy to fall by half in sparse-grid mode. Longer values keep a hot plume rising farther. |

Opacity is approximately driven by `density × densityscale × count × extinction`. Scattered brightness and tint additionally depend on `albedo`, lighting, and anisotropy. Increasing `radius`, `radiusscale`, or `spawnradius` spreads the source across more space, so local density may fall even when total deposited mass is unchanged.

`nri_ptsmokerepresentation 0` selects the particle compatibility path, the default `1` selects the authoritative GPU sparse grid, and diagnostic value `2` runs the compare path with both representations. The grid's important differences from particle compatibility mode are:

- `count` scales deposited mass; it is not a literal number of visible particles.
- effective source support is the maximum of `spawnradius`, `radius × radiusscale`, and one grid cell, capped at 16 grid cells; those radii are not added together.
- `lifetime` does not delete grid smoke. Grid smoke decays through `densityhalflife` and is eventually reclaimed after becoming inactive.
- `expansionvelocity` is radial launch momentum rather than a continuously growing sphere.
- `temperature`, `momentumscale`, `coolinghalflife`, and `turbulencescale` primarily describe grid behavior.

### Actor Smoke Rules

`smokeactorrule <id>` tracks each matching live actor independently. Actor, owner, and excluded-owner class matching includes descendants.

| Field | Default and accepted values | Effect |
| --- | --- | --- |
| `actorclass <class>` | Empty; a resolvable class is required | Actor class that owns the source. An unresolved class makes the rule inert. |
| `ownerclass <class>` | Empty | Optional direct-owner requirement. The actor emits only when `GetOwnerActor()` matches this class or a descendant. |
| `excludeownerclass <class>` | Empty | Optional direct-owner exclusion. Matching owners suppress this rule. Useful when a generic carrier class is shared by several effects. |
| `trigger <mode>` | `spawn`; `spawn` or `interval` | `spawn` emits once when eligible. `interval` emits once when eligible and then continues using spatial or timed cadence. |
| `activation <mode>` | `immediate`; `immediate` or `surface` | `immediate` starts when the live actor/rule is first observed. `surface` waits until renderer appearance evidence exists, then latches; use it for hidden or scripted fire actors. |
| `emitterforeground <state>` | `off`; `on` or `off` | With `on`, matching actor sprite/voxel pixels remain in front of smoke. This is a coarse actor mask: it suppresses **all** smoke at those pixels, not only smoke from this source. |
| `representation <mode>` | `grid`; `grid` or `analytic` | Selects the source representation described above. |
| `queuepolicy <mode>` | `retry`; `retry`, `drop`, or `latest` | Selects overload handling described above. |
| `maxlatencyseconds <seconds>` | Omitted; minimum `0` | Optional first-publication freshness bound in gameplay/presentation time. |
| `style <id>` | Empty; a resolvable style is required | `smokestyle` used by emitted commands. An unresolved reference makes the rule inert. |
| `count <integer>` | `1`; `[1,256]` | Particle carriers per source in particle mode; deposited-mass multiplier in grid mode. This is a strong source-density control and directly affects compatibility-mode particle work. |
| `offset <x> <y> <z>` | `0 0 0` | Actor-local right, forward, and Build/world-Z offset. Negative Z moves visually upward; positive Z moves downward. |
| `spawnradius <units>` | `0`, minimum `0` | Initial source spread. Particle centers are randomized inside this sphere; grid mode uses it as one candidate for deposition support. |
| `densityscale <scale>` | `1`, minimum `0` | Per-rule multiplier on style density/mass. |
| `radiusscale <scale>` | `1`, minimum `0` | Per-rule multiplier on style radius. |
| `velocitycone <degrees>` | `0`; `[0,180]` | Half-angle of uniform solid-angle stochastic launch spread around the source velocity. `0` follows the axis; `180` permits a sphere. With a zero axis, launch directions are spherical. |
| `velocityscale <scale>` | `1`, minimum `0` | Scales actor source velocity before style shaping. It supplies an impulse magnitude only when style `velocityinherit` is nonzero; it can still establish the cone axis when inheritance is zero. |
| `intervalseconds <seconds>` | `0.1`, minimum `0.001` | Timed cadence for `trigger interval`, including the stationary fallback. Uses gameplay time, not smoke simulation timescale. |
| `pulseamount <fraction>` | `0`; `[0,1]` | Mean-preserving modulation of emission mass. `0` is an exact identity; `1` ranges from a zero-mass trough to twice the authored `densityscale`. |
| `pulseperiodcadences <integer>` | `1`; `[1,256]` | Number of logical emission crossings in one pulse cycle. Values of `1` are an identity. The duration in seconds is this value multiplied by `intervalseconds` for a stationary timed source. |
| `pulsephase <cycles>` | `0`; wrapped to `[0,1)` | Authored cycle offset. Phase `0` starts the first cadence at the trough; `0.5` starts it at the peak. |
| `starttime <seconds>` | `0`, minimum `0` | Delay from activation before first emission. The actor must remain alive through the delay. Uses gameplay time and is independent of `nri_ptsmoketimescale`. |
| `startdistance <units>` | `0`, minimum `0` | Cumulative actor travel required after activation. Useful for beginning projectile trails away from the player. |
| `spacing <units>` | `0`, minimum `0` | When positive and the actor moves, emits at distance crossings instead of timed cadence. This makes trail density less dependent on projectile speed. Stationary frames fall back to `intervalseconds`. |
| `maxsegmentsperframe <integer>` | `1`; `[1,256]` | Bounds interval/spacing emissions generated in one frame. If more crossings occurred, the newest crossings are kept and older ones are discarded rather than backfilled later. |

`starttime` and `startdistance` advance concurrently after activation. If both are nonzero, both must pass and the later threshold crossing controls the first emission. Suppressed movement and interval time are not replayed as a catch-up burst. Reloading LIGHTOVR clears activation/cadence state, so each surviving actor begins again under the reloaded rule.

Pulse modulation is sampled from the actor's logical cadence ordinal, not the render frame. For a period `N >= 2`, cadence `k` receives `1 - pulseamount * cos(2*pi*((k-1)/N + pulsephase))` times the authored `densityscale`. A complete cycle therefore retains the same average mass as an unmodulated rule. Hitches, retry delay, dormant-source coalescing, and render-frame partitioning do not change the selected pulse phase or its accumulated mass. Changing `intervalseconds` changes the pulse duration while preserving its cadence shape.

Initial grid velocity is approximately:

```text
source velocity × velocityscale × velocityinherit × momentumscale
+ stochastic cone direction × velocityrandom
+ radial direction × expansionvelocity
+ upward risevelocity
```

Thermal buoyancy and turbulence then continue to modify the field while drag damps it and cooling reduces thermal rise.

### Event Smoke Rules

`smokeeventrule <event_id>` emits once for each matching gameplay event. The rule ID is the case-insensitive event ID; merely inventing a name does not create a gameplay producer.

| Field | Default and accepted values | Effect |
| --- | --- | --- |
| `style <id>` | Empty; a resolvable style is required | Smoke style emitted by the event. |
| `representation <mode>` | `grid`; `grid` or `analytic` | Selects the source representation described above. |
| `queuepolicy <mode>` | `retry`; `retry`, `drop`, or `latest` | Selects overload handling described above. Analytic impacts and muzzle puffs normally use `drop`. |
| `maxlatencyseconds <seconds>` | Omitted; minimum `0` | Optional first-publication freshness bound in gameplay/presentation time. |
| `count <integer>` | `1`; `[1,256]` | Particle count or grid deposited-mass multiplier, as described for actor rules. |
| `offset <x> <y> <z>` | `0 0 0` | Event-local right, forward, and producer-supplied third-basis offset. Current Duke weapon producers use positive Build Z for that third vector at level aim, so a negative third offset moves visually upward. With no basis, this offset is ignored. |
| `offsetrandom <right> <forward> <up>` | `0 0 0`; each component is finite and nonnegative | Per-event local-axis jitter half-extents added to `offset`. Each event deterministically samples right, forward, and up within the corresponding `[-extent,+extent]` interval. One sampled origin is shared by the complete event rather than changing per frame; zero leaves the fixed offset unchanged. With no producer basis, this jitter is ignored. |
| `spawnradius <units>` | `0`, minimum `0` | Initial event-source spread/support. |
| `densityscale <scale>` | `1`, minimum `0` | Per-event multiplier on style density/mass. |
| `radiusscale <scale>` | `1`, minimum `0` | Per-event multiplier on style radius. |
| `velocitycone <degrees>` | `0`; `[0,180]` | Half-angle of stochastic launch spread around the selected direction. |
| `velocityscale <units/s>` | `1`, minimum `0` | Magnitude of the event command direction before style `velocityinherit`; also establishes the cone axis for style `velocityrandom`. |
| `normaloffset <units>` | `0`, signed | Moves a surface event along its supplied unit normal. Positive values move out of a hit surface; negative values move into it. Has no effect when the event has no normal. |
| `direction <mode>` | `aim`; `aim`, `normal`, or `incoming` | Selects the command axis. `aim` uses event basis-forward, `normal` uses the outward surface normal, and `incoming` uses travel direction toward the hit. Missing normal/incoming data safely falls back to aim. |

Currently wired Duke event IDs are:

- `duke.pistol.primary`
- `duke.shotgun.primary`
- `duke.chaingun.primary`
- `duke.grower.primary`
- `duke.shrinker.primary`
- `duke.devastator.primary`
- `duke.freezer.primary`
- `duke.flamethrower.primary`
- `duke.rpg.primary`
- `duke.hitscan.impact.wall`
- `duke.hitscan.impact.plane`
- `duke.hitscan.impact.actor`

Muzzle events provide the player weapon basis. Hitscan impact events provide incoming direction and an outward-facing surface normal, making `direction normal` the usual choice for impact smoke.

`velocityscale` alone does not necessarily make smoke faster. It scales the selected command direction; style `velocityinherit` controls how much of that command speed is inherited, while `velocityrandom` supplies stochastic launch speed around the command axis.

### Map Smoke Emitters

`smokeemitter <id>` is an always-active, map-local rectangular source plane. It begins emitting when its map becomes active and continues at the authored interval. Scripted or sector-driven activation is not currently supported.

```text
map "E1L1"
{
    smokeemitter "TrashFireSmoke01"
    {
        style "example_fire_smoke"
        position 1024.0 2048.0 -128.0
        normal 0.0 0.0 -1.0
        size 48.0 32.0
        rotation 0.0
        offset 2.0
        count 8
        intervalseconds 0.15
        spawnradius 2.0
        densityscale 1.0
        radiusscale 1.0
        velocityscale 0.0
        velocitycone 20.0
        maxsegmentsperframe 2
    }
}
```

| Field | Default and accepted values | Effect |
| --- | --- | --- |
| `style <id>` | Empty; a resolvable global style is required | Smoke style emitted by the source. An unresolved reference leaves the emitter inert. |
| `position <x> <y> <z>` | Required | Center of the unoffset source plane in Build/world coordinates. Build Z increases downward. |
| `normal <x> <y> <z>` | Required and must be nonzero | Plane normal and default source-velocity direction. A zero/invalid basis leaves the rule inert. The editor writes a finite normalized, viewer-facing normal after converting the aimed PT surface to Build/world coordinates. |
| `size <width> <length>` | Required; each component clamps to `[1,256]` | Full dimensions of the rectangular source plane. The total pulse mass does not increase with area, so a larger plane spreads the same emission more broadly. The bounded range prevents pathological sparse-grid traversal. |
| `rotation <degrees>` | `0` | Rotates the rectangle around its normal. Zero uses a deterministic world-derived tangent basis, not the surface's texture axes or camera-screen horizontal. |
| `offset <units>` | `0`, signed | Moves the source center along its normal. Positive values move away from the aimed surface; negative values move through it. |
| `count <integer>` | `1`; `[1,256]` | Particle count in compatibility mode or total deposited-mass multiplier in grid mode. This remains total mass per pulse rather than mass per unit area. |
| `intervalseconds <seconds>` | `0.1`, minimum `0.001` | Gameplay-time delay between emission pulses. It is independent of `nri_ptsmoketimescale`. |
| `spawnradius <units>` | `0`, minimum `0` | Adds spherical positional spread and supplies grid-kernel thickness around the plane. |
| `densityscale <scale>` | `1`, minimum `0` | Per-emitter multiplier on style density/total pulse mass. |
| `radiusscale <scale>` | `1`, minimum `0` | Per-emitter multiplier on style radius and grid support. |
| `velocityscale <units/s>` | `0`, minimum `0` | Source-command speed along the plane normal before style `velocityinherit`. The style's rise, random velocity, expansion, buoyancy, and turbulence remain independent. |
| `velocitycone <degrees>` | `0`; `[0,180]` | Half-angle for stochastic style velocity around the plane normal. |
| `maxsegmentsperframe <integer>` | `1`; `[1,256]` | Bounds cadence catch-up after a slow frame. The newest crossings are retained and discarded crossings are not replayed later. |

Grid mode deposits one normalized kernel over the oriented rectangle rather than expanding it into CPU-generated point sources. Enlarging the plane therefore increases the number of sparse cells touched and may cost more GPU work, but it does not increase CPU commands or routine GPU-to-CPU traffic. Particle compatibility mode samples carrier positions uniformly across the rectangle before applying `spawnradius`.

#### Map Smoke Emitter Edit Mode

The map smoke emitter editor stages map-local rectangular emitters against an aimed PT surface. It uses the same loose, writable `LIGHTOVR` selection as the light editors and starts disabled every launch. Only one LIGHTOVR editor mode may be active at a time. Before editing, use `nri_ptactorlighteditwritable` to verify the destination path and keep the writable file under version control or make a backup: commit/delete normalize and replace the complete writable file, so comments, hand formatting, and original ordering are not preserved.

Placement requires an active NRI/PT renderer, a center-screen PT surface hit, and at least one resolved `smokestyle`. Visible preview smoke additionally requires `nri_ptsmoke true`.

Enable it in a live map with:

```text
nri_ptmapsmokeeditmode 1
```

While the mode is enabled:

- `p` places a fresh, uniquely named 32 by 32 default draft on the aimed surface, offset one unit along its viewer-facing normal
- `Ctrl+p` creates a fresh uniquely named clone at the newly aimed surface and copies every active-draft field, including values authored only in text; it does not move or replace the original rule
- left/right arrow decreases/increases rectangle width in 4-unit steps
- down/up arrow decreases/increases rectangle length in 4-unit steps
- mouse wheel down/up rotates the plane around its normal in 15-degree steps
- `<` and `>` decrease/increase its signed height above the surface in 2-unit steps; unshifted `,` and `.` work as well
- `;` and `'` decrease/increase total emission count by one
- `j` and `k` decrease/increase the interval by 0.025 seconds
- `[` and `]` cycle resolved smoke styles
- `o` cycles through persisted emitters on the current map and stages the selected rule for editing
- `Enter` serializes the merged normalized database over the complete writable loose file, then reloads; editing an archive-backed selection creates an effective loose override rather than changing its archive
- `Escape` cancels the active draft without writing; when no draft exists, normal Escape handling continues
- `Delete` discards an uncommitted draft or, without confirmation, removes a selected rule owned by the writable loose overlay; archive/other-source rules cannot be deleted, and removing a loose last-wins rule may reveal an older same-ID definition after reload
- `l` cancels the current draft and reloads `LIGHTOVR` from disk without writing

Every staged adjustment prints the current editor-adjustable style, size, rotation, signed height, count, and interval. While edit mode is enabled, every geometrically valid effective `smokeemitter` on the current map appears as a subdued yellow rectangle. The active new, cloned, or selected draft pulses; when editing a persisted rule, its staged geometry replaces the static outline so size, rotation, and offset changes do not leave a stale rectangle underneath. Cancelling, committing, deleting, or reloading removes the pulsing draft state, while the resulting persisted current-map rectangles remain visible until the mode is disabled.

The outlines are crisp post-scene editor overlays. They are clipped to the 3D viewport and remain visible through smoke and map geometry rather than participating in scene depth, lighting, ray tracing, denoising, or upscaling. Effective rules with unresolved styles are still outlined when their rectangle geometry is valid, which makes a broken authoring location discoverable even though it cannot emit smoke.

The normal smoke runtime also provides emitted smoke as a live preview using the same rectangle geometry as a committed emitter. Previously deposited smoke remains after an adjustment unless it naturally dissipates or `nri_ptsmokereset` is used. Selecting a persisted rule temporarily suppresses its committed instance so the preview does not double its output. `o` currently cycles persisted emitters by rule order rather than selecting one by crosshair.

The hotkeys do not edit `spawnradius`, `densityscale`, `radiusscale`, `velocityscale`, `velocitycone`, `maxsegmentsperframe`, the rule id, or a persisted rule's position/normal. Tune those in text and press `l` (or run `lightoverlay_reload`). To reposition a rule, clone it with `Ctrl+p`, commit the clone, then explicitly select and delete the original loose rule.

Disable the editor with:

```text
nri_ptmapsmokeeditmode 0
```

### Common Smoke Recipes

These source fragments assume that the referenced `example_*_smoke` styles have already been declared.

Use distance spacing for a speed-independent rocket trail and `startdistance` to keep smoke away from the camera:

```text
smokeactorrule "example_rocket_trail"
{
    actorclass "DukeRPG"
    trigger interval
    style "example_trail_smoke"
    count 10
    offset 0.0 -5.0 0.0
    spawnradius 2.0
    densityscale 1.0
    radiusscale 1.0
    velocityscale 0.5
    velocitycone 20.0
    intervalseconds 0.04
    startdistance 96.0
    spacing 4.0
    maxsegmentsperframe 8
}
```

Use a one-shot actor rule for an explosion cloud. `starttime` can hand off visually from a fireball, but the actor must survive long enough to cross the gate:

```text
smokeactorrule "example_explosion_cloud"
{
    actorclass "DukeExplosion2"
    trigger spawn
    starttime 1.0
    style "example_explosion_smoke"
    count 64
    spawnradius 16.0
    densityscale 1.5
    radiusscale 2.0
    velocitycone 180.0
}
```

Use an event rule with forward offset for muzzle smoke:

```text
smokeeventrule "duke.shotgun.primary"
{
    style "example_muzzle_smoke"
    count 20
    offset 0.0 16.0 0.0
    spawnradius 4.0
    densityscale 1.25
    radiusscale 1.25
    velocityscale 3.0
    velocitycone 24.0
    direction aim
}
```

Use outward normal placement and direction so impact smoke does not begin inside a wall:

```text
smokeeventrule "duke.hitscan.impact.wall"
{
    style "example_impact_smoke"
    count 12
    normaloffset 3.0
    spawnradius 2.0
    densityscale 1.0
    radiusscale 1.0
    velocityscale 6.0
    velocitycone 40.0
    direction normal
}
```

Use a long density half-life, low thermal lift, gentle expansion, and broad turbulence for slow ground-level mood smoke that remains responsive to colored lighting:

```text
smokestyle "example_ground_mood_smoke"
{
    density 1.6
    extinction 0.0035
    albedo 0.52 0.52 0.50
    anisotropy 0.05
    radius 12.0
    expansionvelocity 2.5
    lifetime 18.0
    densityhalflife 9.0
    risevelocity 0.5
    velocityrandom 2.5
    velocityinherit 0.25
    buoyancy 0.15
    drag 0.35
    turbulence 5.0
    turbulencescale 56.0
    temperature 0.5
    momentumscale 1.0
    coolinghalflife 2.0
}

map "E1L1"
{
    smokeemitter "example_ground_mood_zone"
    {
        style "example_ground_mood_smoke"
        position 1024.0 2048.0 -128.0
        normal 0.0 0.0 -1.0
        size 96.0 64.0
        offset 4.0
        count 24
        intervalseconds 0.35
        spawnradius 4.0
        velocityscale 2.0
        velocitycone 75.0
        maxsegmentsperframe 2
    }
}
```

The nonzero `velocityscale` supplies an upward normal axis, but `velocityinherit 0.25` keeps that organized motion slow. `velocityrandom` and expansion spread the cloud while broad turbulence prevents a uniform sheet. For a rectangle with twice the area, roughly double `count` to preserve local thickness because count is total pulse mass. Global wind is still shared by every style; keep `nri_ptsmokewindy` near zero (roughly `0` to `2`) when testing a ground-hugging result.

For a less uniform sustained fire column, combine positive `risevelocity`, `temperature`, and `buoyancy` with nonzero `velocityrandom`, `expansionvelocity`, and `turbulence`. Use moderate `drag`, a turbulence wavelength appropriate to the plume width, and a long enough `densityhalflife` for smoke to travel before fading. Global wind can bend every plume; style controls should supply the fire-specific rise and variation.

### Reloading and Smoke Diagnostics

- `lightoverlay_reload`
  Reloads all mounted LIGHTOVR data. Smoke style layout, actor activation/cadence state, and the current smoke simulation epoch reset, so existing smoke disappears and active sources start over.
- `lightoverlay_dump`
  Prints parsed-rule summaries and source locations.
- `lightoverlay_dumpresolved [mapname]`
  Confirms that actor classes, owner filters, styles, and event rules resolved. Check this first when a source emits nothing.
- `lightoverlay_dumpnormalized`
  Prints canonical round-trippable syntax with all smoke defaults made explicit.
- `nri_ptsmoke_test`
  Queues one built-in synthetic smoke injection in front of the player.
- `nri_ptsmoke_test <event_rule_id>`
  Queues a local-player event for a resolved `smokeeventrule`. With a live local player, the synthetic event supplies a deterministic aim basis, incoming direction, and opposing surface normal so `normaloffset` and all direction modes can be tested without manufacturing a gameplay collision.
- `nri_ptsmokestatus`
  Prints current enable/route, representation, command, simulation, lighting, occupancy, and GPU-readback status.
- `nri_ptsmokereset`
  Clears current smoke and resets its simulation epoch.
- `nri_ptsmoketrace 2`
  Enables verbose actor activation/start-time deferral, actor emission position/velocity, map/map-preview emission, and event-match logging. Return it to `0` after diagnosis.
- `nri_ptsmokereadback true`
  Enables additional GPU smoke counters used by status diagnostics. Leave it off for ordinary play.

Common failure checks:

- no smoke at all: confirm the Display Options Smoke toggle or `nri_ptsmoke`, the NRI renderer, and resolved style/class names
- actor smoke begins behind unopened geometry: use `activation surface`
- explosion smoke never appears: reduce `starttime` or use a longer-lived actor/event source
- trail begins too near the player: add `startdistance` or `starttime`; use `spacing` for consistent distance density
- grid smoke ends too slowly: reduce `densityhalflife`; changing `lifetime` affects only particle compatibility mode
- smoke is too dark: raise `albedo` or verify that relevant point, directional, and emissive lighting reaches it; do not treat albedo as self-emission
- plume is a uniform tube: add `velocityrandom`, `expansionvelocity`, and grid turbulence, then balance drag and half-life
- the fire itself is hidden by its plume: use `emitterforeground on`, understanding that this actor mask wins over all smoke at those pixels
- performance or capacity suffers: reduce source `count`, emission frequency, source radius, or the number of simultaneous emitters before increasing global smoke capacities

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

Use `Ctrl+o` to repeat-place a tuned fixture. The repeated rule gets a fresh id, position, normal, sector, wall, tile, and signal sector from the newly aimed surface. It copies the active `surfacelight` settings for size, rotation, offset, fixture texture, fixture material response, light type, color, intensity, radius, sector-response enabled state, response curve fields, and material-response clamps.

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
