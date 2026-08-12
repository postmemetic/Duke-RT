# RT Renderer Debug Commands

Duke-RT's RT backend is the ray tracing renderer path built on NVIDIA NRI. This guide collects the current runtime controls, diagnostics, and repro workflows for the project.

Naming note: the project-facing renderer name is `RT`. The existing code, cvars, commands, source paths, and many log prefixes still use the legacy `nri_` / `NRI` spelling, so commands below are written exactly as they are currently entered in-game. Also, many of them include PT because I was setting out explicitly to build a path tracer, not just standard ray-traced direct lighting and shadows.

## Path Tracing Debug

The RT backend owns the current path-tracing path. Use the in-game console or config to control it.

There is also an in-game control panel for these settings under:

- `Options -> Display Options -> Render Options`

Commands:
- RT is selected automatically on NRI-capable builds.
- `nri_api vulkan` or `nri_api d3d12`
  Selects which graphics API the RT backend uses through NRI on Windows. Restart required.
- Frame generation currently targets:
  `RT + D3D12 + windowed mode`.
  Exclusive fullscreen currently resolves FG off.
  The in-game menu path is:
  `Options -> Display Options -> Render Options`.
  Changing `Frame Generation`, `Frame Gen Provider`, or `Frame Gen Low Latency` while the app is running recreates the active RT swapchain/present path, so a short hitch is expected.
- `nri_ptdebug 0`
  Default lit path-traced output.
- `nri_ptdebug 1`
  Surface normal debug view.
- `nri_ptdebug 2`
  UV debug view.
- `nri_ptdebug 3`
  Material ID debug view.
- `nri_ptdebug 4`
  Primitive ID debug view.
- `nri_ptdebug 5`
  Motion-vector debug view. Miss / no-history pixels show magenta; RGB encodes signed XY motion with nonlinear gain plus magnitude in blue; the top-left corner pulses every frame so presenter freezes are obvious; a small center-sample bar probe shows live `motion.x`, `motion.y`, `motion.z`, and validity.
- `nri_ptdebug 9`
  NRD validation/debug output.
- `nri_ptdebug 10`
  Raw diffuse PT buffer debug view.
- `nri_ptdebug 11`
  Raw specular PT buffer debug view.
- `nri_ptdebug 12`
  Raw hit-distance debug view.
- `nri_ptdebug 16`
  Denoised diffuse NRD output.
- `nri_ptdebug 17`
  Denoised specular NRD output.
- `nri_ptdebug 18`
  Metalness guide view.
- `nri_ptdebug 19`
  Roughness guide view.
- `nri_ptdirectscene 1`
  Forces the old direct-scene primary tracer as a diagnostic fallback instead of the normal AS-backed primary path.
- `nri_ptdirectscene 0`
  Uses the normal BLAS/TLAS-backed primary path in non-bootstrap gameplay.
- `nri_ptportaldepth 2`
  Controls how many explicit PT portal-transfer traversals a ray may take before traversal stops continuing through transfer portals.
- `nri_ptcaps`
  Prints device/API/PT capability details, including ray tracing tier and supported upscalers.
- `nri_ptstatus`
  Prints RT runtime state, including all sorts of comprehensive state such as current resolved upscaler path, frame generation policy/provider/present state, NRD contract details, history state, translated scene stats, frame-boundary timings, swapchain diagnostics, etc.
  The portal section now reports traversal depth plus reflective / transfer / runtime-bound portal counts and how many sector portal planes still remain graph-only.
  The runtime-link section reports whether a render-driven geometry-link overlay is active and additional state around it.
- `nri_ptbuffers`
  Prints the same core PT status plus scene-buffer usage/capacity and per-buffer upload/grow/overwrite counters.

## Scene Texture Cap

This sets how many textures get cached at runtime. Adding more will use more VRAM. The RT scene-texture cap is currently a compile-time limit, not a runtime cvar.

To change it locally:

- update `NRI_MAX_SCENE_TEXTURES` in `source/common/rendering/nri/renderer/nri_renderer.cpp`
- update `MAX_SCENE_TEXTURES` in `source/common/rendering/nri/shaders/Include/Shared.hlsli`
- rebuild the project normally; the RT shader step will regenerate the affected shader during the build

The current startup log/status path will also report the requested scene-texture cap and the relevant NRI-backed descriptor limits:

- `NRI PT scene texture cap: cap=... scene_set=... stage_textures=... limits=set:... set_uas:... stage:... stage_uas:... supported=yes|no`

Symptoms of scene-texture cap overflow:

- actor or mirror-path sprite materials momentarily swap to incorrect textures or fall back to slot `0`
- non-actor materials can lose aux maps instead of swapping cleanly, for example missing normal/metallic/roughness contribution
- `nri_ptstatus` reports a non-zero `NRI PT scene texture overflow: ...`
- `nri_ptstatus` reports non-zero `NRI PT actor overflow: ...` when actor-owned materials are being clamped
- `perf_looptraceframes` emits `PERF pt actor overflow NRI: ...` lines showing original texture indices being clamped to `0` or `UINT32_MAX`

## Frame Generation

Frame generation is exposed in the render options menu:

- `Options -> Display Options -> Render Options -> Frame Generation`

Current scope:

- The implemented FG path is `RT + D3D12 + FSR3`.
- FG is currently intended for windowed mode.
- The current recovery slice only supports PT output mode `SDR` (`nri_ptoutputmode 0`).
- HDR PT output requests currently resolve FG off with an explicit output-contract reason instead of partially inheriting HDR-present behavior.
- Exclusive fullscreen currently resolves FG off with `reason=fullscreen-not-supported`.
- Switching FG on or off while the app is still running is supported on the current path, but it is not seamless. The backend recreates the swapchain/present shell, so expect a short hitch.

Frame generation CVars:

- `nri_framegen 0`
  Disables frame generation and keeps presentation on the native RT swapchain path.
- `nri_framegen 1`
  Requests frame generation. On the supported path this enables the FSR3 proxy-present shell; otherwise the request stays visible in diagnostics and resolves off with a reason.
- `nri_framegenprovider 0`
  Requests no frame-generation provider. This is effectively off.
- `nri_framegenprovider 1`
  Requests the FSR3 frame-generation provider.
- `nri_framegenui 0`
  Requests automatic UI handling. The current implementation resolves `Auto` to `UI Texture`.
- `nri_framegenui 1`
  Uses the HUD-less scene color path. This is the simplest provider input path and excludes a separate UI texture.
- `nri_framegenui 2`
  Uses the split UI-texture path. The 3D scene stays HUD-less for FG input, and HUD/menu content is captured to a dedicated UI surface for composition.
- `nri_framegenui 3`
  Requests present-callback UI handling. In the current implementation this resolves back to `UI Texture`.
- `nri_framegenlatency 0`
  Disables FG low-latency handling. This clears the FG swapchain low-latency request.
- `nri_framegenlatency 1`
  Requests FG low-latency handling. On supported D3D12 runs this enables the low-latency swapchain flag and marker/sleep path.
- `nri_framegenasync 0`
  Disables async FG workload support.
- `nri_framegenasync 1`
  Requests async FG workload support. The request is still shown in policy/status even on systems where the provider resolves it off.

Useful FG diagnostics:

- `nri_ptstatus`
  The `NRI PT framegen policy:` line reports requested vs resolved provider, requested vs resolved PT output mode, resolved FG output contract, API, window mode, low-latency state, async state, UI mode, swapchain readiness, and the current resolve reason.
- `nri_ptstatus`
  The `NRI PT framegen caps:` line mirrors the same output-contract status at device-capability time so unsupported PT output modes can be seen before a live FG dispatch run.
- `nri_ptstatus`
  The `NRI PT framegen present contract:` line reports the resolved swapchain format, resolved/active target formats, DXGI backbuffer format, transfer function, luminance range, HDR paper-white scale, and whether proxy FG is allowed for the current present contract.
- `nri_ptstatus`
  The `NRI PT framegen provider:` line reports provider runtime load state, context creation, proxy-swapchain bridge state, configure/prepare/dispatch counts, reset count, last reset reason, present mode/result, and current provider status reason.
- `nri_ptstatus`
  The `NRI PT framegen present:` line reports whether the current frame is using native present, proxy passthrough, or proxy-generated presentation.
- `nri_ptstatus`
  The `NRI PT framegen inputs:` line reports the current FG frame id plus HUD-less/UI/motion/depth input sizes and frame timing.
- `nri_ptstatus`
  The `NRI PT framegen contract:` line reports the captured motion-vector and depth semantics and whether the current input set still needs provider-side prepare handling.


## Lighting Debug

These commands cover manual analytic lights, sprite/tile light heuristics, emissive tile rules, emissive sampling diagnostics, and sector-light heuristics.

Light-related debug views:

- `nri_ptdebug 21`
  Live raw penumbra input for the split shadow denoiser. This is only meaningful when the shadow-split debug path is active; magenta means that path is unavailable.
- `nri_ptdebug 22`
  Live raw shadow visibility before shadow denoising.
- `nri_ptdebug 23`
  Temporal SIGMA shadow output after denoising.
- `nri_ptdebug 24`
  Direct-lighting buffer before final composition.
- `nri_ptdebug 25`
  Direct self-emission buffer before final composition.
- `nri_ptdebug 26`
  Analytic direct-light-only view. Useful for manual point lights and sprite-tile analytic heuristics.
- `nri_ptdebug 27`
  Self-emission-only view.
- `nri_ptdebug 28`
  Sampled emissive direct-light-only view.
- `nri_ptdebug 29`
  Sector-lighting source view.
- `nri_ptdebug 33`
  Sampled emissive visibility coverage view. Red is occluded, green is visible, blue is rejected or non-contributing.

Analytic light commands:

- `nri_ptlightspawn <r> <g> <b> <intensity> [radius] [offset]`
  Spawns a test point light in front of the local player. Use this to validate analytic direct lighting without changing map content.
- `nri_ptlightlist`
  Lists the currently active manual test lights.
- `nri_ptlightremove <id>`
  Removes one manual test light by id.
- `nri_ptlightclear`
  Clears all manual test lights.
- `nri_ptlightdump [radius] [limit]`
  Dumps nearby scene-light records around the player. This is the broad analytic-light inspection command when you want source, tile, actor, sector, and provenance details.
- `nri_ptlightdebug_nearplayer [radius] [limit]`
  Alias of the nearby scene-light dump. Use it the same way as `nri_ptlightdump`.
- `nri_ptlightclusterdebug`
  Prints the analytic-light clustering state, including tile grid, used indices, and max occupancy.
- `nri_ptlightheuristic_addsprite <tile> <r> <g> <b> <intensity> <radius> [flicker_frames]`
  Adds an analytic light heuristic for a sprite tile. This is useful when a sprite should behave like a simple point or bulb light rather than a textured emissive surface.
- `nri_ptlightheuristic_list`
  Lists the currently active sprite-tile analytic light heuristics.
- `nri_ptlightheuristic_clear`
  Clears all sprite-tile analytic light heuristics.

Debug sphere commands:

- `nri_ptsphere <diameter> <distance> [metalness] [roughness]`
  Spawns a debug sphere along camera forward. The default material is `metalness=1` and `roughness=0.05`, so `nri_ptsphere 128 512` gives you a 100% metal, 95% smooth sphere 512 world units in front of the local player.
- `nri_ptspherespawn <diameter> <distance> [metalness] [roughness]`
  Alias of `nri_ptsphere`.
- `nri_ptspherelongs <segments>`
  Sets the runtime debug sphere longitudinal tessellation. The default is `64`, which contributes to a `64x32` sphere mesh with `3968` triangles per sphere.
- `nri_ptspherelats <segments>`
  Sets the runtime debug sphere latitudinal tessellation. The default is `32`. Valid ranges are `8..256` for longitude and `4..128` for latitude.
- `nri_ptspherelist`
  Lists the active debug spheres, including world and render-space positions plus the current tessellation and triangle count.
- `nri_ptsphereremove <id>`
  Removes one debug sphere by id.
- `nri_ptsphereclear`
  Clears all debug spheres.

Emissive commands:

- `nri_ptemissiveheuristic_addtile <tile> [intensity_scale]`
  Adds an emissive tile rule using base-texture emission.
- `nri_ptemissiveheuristic_addtilemode <tile> <base|glowmap|constant> [intensity_scale] [r g b]`
  Adds an emissive tile rule with an explicit source mode. Use `base` for textured emission, `glowmap` to sample a glowmap, or `constant` for a fixed color emitter.
- `nri_ptemissiveheuristic_list`
  Lists the active emissive tile rules.
- `nri_ptemissiveheuristic_clear`
  Clears all emissive tile rules.
- `nri_ptemissivedump [radius] [limit]`
  Dumps the currently bound emissive primitive candidates near the player. This is the main diagnostic for answering "is this emitter actually in the emissive sampler right now?"
- `nri_ptemissivesamples 1`
  Uses one primary-hit sampled emissive direct-light sample per pixel. This is the fastest mode.
- `nri_ptemissivesamples 4`
  Uses up to four primary-hit sampled emissive direct-light samples per pixel to reduce variance in billboard or neon validation scenes.
- `nri_ptemissivefastshadow 1`
  Uses the cheaper emissive visibility path. This is the default and the preferred performance setting.
- `nri_ptemissivefastshadow 0`
  Disables the cheaper emissive visibility path for comparison and debugging.
- `nri_ptemissiveheuristics 1`
  Enables automatic emissive tagging from fullbright, glow, and glowmap material metadata, plus any explicit emissive tile rules.
- `nri_ptemissiveheuristics 0`
  Disables automatic emissive tagging. Explicit emissive tile rules still remain the main manual authoring path to test with.
- `nri_ptemissiveautoonly 1`
  Restricts emissive sampling to automatically tagged emissive surfaces and ignores explicit tile rules. This is mainly a diagnostic filter.
- `nri_ptemissiveautoonly 0`
  Uses both automatic emissive tagging and explicit tile rules.
- `nri_ptglowscale <scale>`
  Globally scales glow-driven emission strength. This affects auto glow and glowmap materials, plus explicit `glowmap` emissive rules. `1` is default and `0` disables glow-driven emission.
- `nri_ptglowreach <scale>`
  Globally scales glow-driven sampled-light reach without changing the on-surface emissive appearance by the same amount. `1` is default and `0` disables glow-driven sampled light throw.
- `nri_ptglowblend <scale>`
  Scales the visible glowmap overlay blend on the surface itself. `1` preserves the default overlay result, `0` removes the visible glowmap overlay, and values up to `3` exaggerate it without changing sampled-light reach.
- `nri_ptemissiveminsurface <area>`
  Rejects very small emissive candidates below the given surface-area threshold.
- `nri_ptemissiveminpower <power>`
  Rejects very weak emissive candidates below the given power threshold.

Sector-light commands:

- `nri_ptsectorlighting 1`
  Enables sector-light heuristics.
- `nri_ptsectorlighting 0`
  Disables sector-light heuristics.
- `nri_ptsectorlightmultiplier <scale>`
  Globally scales how strongly the sector-lighting term is applied. `0` turns it off, `0.5` halves it, and `1.0` is the default/full strength.
- `nri_ptsectorlight_set <ambientScale> <hemiScale> <fogScale>`
  Updates the sector-light heuristic scales for ambient, hemisphere, and fog contributions.
- `nri_ptsectorlight_filter <pal|-1> <minShade> <maxShade> [lotag]`
  Filters which sectors participate in sector lighting by palette, shade range, and optional `lotag`.
- `nri_ptsectorlight_clear`
  Restores the default sector-light heuristic settings and clears filters and pulse tuning.
- `nri_ptsectorlightdump [radius] [limit]`
  Dumps nearby sector-light records around the player.
- `nri_ptsectorpulseframes <frames>`
  Enables pulsing sector-light modulation over the given frame period.
- `nri_ptsectorpulseamount <amount>`
  Controls how strong the pulsing sector-light modulation is.

Light-debug workflow:

- Use `nri_ptdebug 26` with `nri_ptlightspawn` or `nri_ptlightheuristic_addsprite` to isolate analytic direct lighting.
- Use `nri_ptdebug 28`, `30`, `31`, `32`, and `33` with `nri_ptemissivedump` to inspect emissive sampling, primitive selection, and visibility.
- Use `nri_ptdebug 29` with `nri_ptsectorlightdump` when tuning the sector-light heuristic scales and filters.
- `nri_ptsurfaceprobe 1`
  Logs the surface under the cross-hair every main-view frame, including provenance such as source type, sector, wall, and primitive/material ids.
- `nri_ptsurfaceprobe 2`
  Logs the same center-hit surface details, but only when the identified surface changes.
- `nri_ptsurfaceprobe 0`
  Disables the center-hit surface probe.
- `nri_ptchunkdump [chunk]`
  Dumps the authoritative map-world surfaces for the requested chunk. If no chunk is provided, it uses the last successful `nri_ptsurfaceprobe` chunk hit. The dump includes chunk/local-space ownership, resident-static vs runtime-replaced state, mutation reasons, source portals, and per-surface wall/section provenance.
- `nri_ptchunkcompare [chunk]`
  Compares the authoritative static chunk against a freshly rebuilt live runtime chunk using stable PT surface keys. If no chunk is provided, it uses the last successful `nri_ptsurfaceprobe` chunk hit. The output includes matched vs unmatched surfaces, mean centroid delta, deviation counts, area/normal outliers, the worst surface mismatches, and seam-focused border-wall diagnostics with adjacent chunk replacement state.
- `nri_ptvisiblechunkgate 0/1`
  Controls the visible-chunk gate used for camera-primary direct visibility. The default is `1`. When enabled it skips primary map hits whose owning chunk is outside the current HW-visible chunk set derived from the visible-sector snapshot, the active view-root sectors, and the accumulated wall/flat drawlists for the frame.
- `nri_pt360absencegate 0/1`
  Controls the default-on, exact negative-census gate for co-located map geometry. It applies the current logical-player 360 drawlist census to primary, indirect, and shadow rays while ordinary off-screen and occluded geometry remains resident. Alternate mirror and typed space-transfer ray contexts fail open instead of inheriting player-root absence evidence.
- `nri_pt360actorabsencegate 0/1`
  Controls the default-on actor-occurrence gate using the same current logical-player census. It suppresses live voxel actors and items rendered from a wrong overlapping locality without treating ordinary off-screen or occluded actors as absent.
- To persistently disable the 360 policies for rollback, set both commands to `0` in the console or config. For a one-run diagnostic launch, use `+set nri_pt360absencegate false +set nri_pt360actorabsencegate false`. The mirror-only actor residency correction is independent and remains active when both gates are disabled.
- `nri_ptscenestats 0`
  Disables scene-stat style diagnostics in the log, including `NRI PT scene: ...`, `NRI PT frame resources: ...`, startup-world-correction summaries, and static-scene trace / animated-refresh lines, without affecting `nri_ptstatus`, mutation traces, or the surface probe.
- `nri_ptscenestats 1`
  Re-enables those scene-stat style diagnostics when the corresponding translated-scene or static-scene state changes.
- `nri_ptruntimelinktrace 0`
  Disables event-style runtime-link diagnostics. This is intended for warp/non-euclidean trace logging rather than the normal `nri_ptstatus` snapshot.
- `nri_ptruntimelinktrace 1`
  Enables event-style runtime-link diagnostics. This will log `NRI PT runtime link event: ...` when relevant runtime-link or player warp state changes, `NRI PT runtime tagged sectors: ...` when visible tagged-sector identity changes, `NRI PT runtime nearby controls: ...` for the candidate/player/source sectors and their one-hop neighbors that carry sector tags or effectors, and `NRI PT runtime transport: ...` when the Duke/RR transport system actually moves the player between sectors. These lines include sector ids/tags, visible tagged-sector lists with sector effectors, nearby control-sector lists, `on_warping_sector`, `transporter_hold`, RR geo count, and transport source/transporter/owner sector identity.
- `nri_ptstatus`
  The `NRI PT runtime links:` block now distinguishes RR geo-effect links from transporter-driven links through `geo_effect=yes/no`, `transport=yes/no`, `transport_links=...`, `transport_spaces=...`, `replaced_chunks=...`, and `translated_chunks=...`.
- `nri_pttraceframes 8`
  Emits one bounded gameplay trace block per frame into the log for the next `N` traced frames. This includes frame-boundary timing, swapchain/frame-shell state, 2D texture cache stats, and sky-source transitions such as `NRI PT sky: ... action=reuse-active-cubemap`.
- `nri_ptnudgetrace 1`
  Temporarily logs successful `LIGHTOVR nudgefromsurface` displacements as `NRI PT surface nudge: ...`, including the path used (`source_wall`, `wall_fallback`, or `plane`), matched surface provenance, configured nudge distance, and before/after light positions.
- `nri_ptnudgetrace 0`
  Disables the temporary `nudgefromsurface` success trace.
- `nri_ptreset`
  Forces the scene history to reset on the next frame.
- `nri_ptsanity 1`
  Replaces the main-view scene path with a clear-only sanity frame while keeping the same RT swapchain and 2D presentation path active.
- `nri_ptsanity 0`
  Restores the normal main-view path.
- `nri_ptbootstrap 1`
  Forces the smallest possible in-level RT world path: a fullscreen backend-owned test pattern written by a single compute pass, with scene capture, tracing, denoising, TAA, and upscaling skipped.
- `nri_ptbootstrapmode 1`
  Switches the bootstrap path from the pure fullscreen test pattern to a camera-driven analytic ground plane, still without scene capture or traced geometry.
- `nri_ptbootstrapmode 2`
  Hardcoded triangle intersection in RT space.
- `nri_ptbootstrapmode 3`
  Hardcoded textured quad built from two triangles, still without scene capture.
- `nri_ptbootstrapmode 4`
  Captured scene signature view based on uploaded primitive and vertex buffer contents.
- `nri_ptbootstrapmode 5`
  Raw uploaded vertex scatter in normalized scene space, independent of camera projection.
- `nri_ptbootstrapmode 6`
  Raw uploaded primitive-centroid scatter in normalized scene space.
- `nri_ptbootstrapmode 7`
  Captured scene point cloud projected from uploaded vertices.
- `nri_ptbootstrapmode 8`
  Captured scene primitive centroids projected from uploaded triangle data.
- `nri_ptbootstrapmode 9`
  Captured scene wireframe projected from uploaded triangles.
- `nri_ptbootstrapmode 10`
  First projected uploaded scene triangle only.
- `nri_ptbootstrapmode 11`
  Captured scene geometry with flat unlit primitive shading and no scene texture uploads.
- `nri_ptbootstrapmode 12`
  Captured scene geometry with base-color sampling only.
- `nri_ptbootstrapmode 13`
  Current full PT path from the bootstrap ladder.

Useful log output while debugging:

- `Selecting NRI backend...`
  Confirms the RT backend was selected.
- `NRI device: ...`
- `NRI graphics API: ...`
  Confirms the adapter and API the backend actually initialized.
- `NRI PT scene: walls=... flats=... sprites=... translucent=... models=... mirrors=... skies=... portal_views=... approx_tris=... materials=...`
  Dumps the translated scene contents when the path-tracing bridge sees a change.
  Set `nri_ptscenestats 0` to silence this line and the related scene-stat / static-scene trace lines during mutation-debug runs.
- `NRI PT sky: captured_mode=... source=... action=...`
  Dumps the resolved sky input when sky tracing is active via `nri_pttraceframes`. Useful actions include:
  `reuse-active-cubemap`, `activate-cached-cubemap`, `create-cached-solid`, and `hold-level-cubemap`.
  `hold-level-cubemap` means RT is deliberately keeping the last confirmed cubemap for the current level even though the current view only exposed weaker wall/flat sky capture.
- `NRI upscaler fallback: requested ... is unavailable on ..., using ...`
  Confirms unsupported DLRR / DLSS-SR / NIS requests were downgraded predictably on the active API or adapter.
- `NRI PT fallback: ...`
  Explains why the RT view fell back to the raster path for the current device or scene.
- `NRI PT caps: ...`
- `NRI PT status: ...`
  Console diagnostics emitted by `nri_ptcaps` / `nri_ptstatus`.
- `NRI PT framegen policy: ...`
- `NRI PT framegen provider: ...`
- `NRI PT framegen present: ...`
  Frame-generation diagnostics emitted by `nri_ptstatus`, including request vs resolve state, provider bridge status, and whether the current present path is native, proxy passthrough, or proxy generated.
- `NRI PT surface probe: ... owner=... data_source=... chunk_replaced=...`
  The surface probe now reports whether the sampled hit came from resident static map geometry, runtime mutation overlay, runtime link overlay, or dynamic overlay, along with the current chunk replacement state and mutation reasons.
- `NRI PT surface probe: ... static_tlas_instanced=... static_probe_included=...`
  These fields distinguish whether a replaced chunk is still present in the static TLAS versus still visible to the CPU-side probe geometry. Runtime mutation replacement now excludes replaced static chunks from both paths so stale static hits and traced visibility should stay aligned.

Useful live profiling commands:

- `stat rendertimes`
  Toggles the render-time overlay. With the RT backend active this now includes an `NRI PT:` block with frame-boundary timings such as `Wait`, `Acquire`, `Submit`, and `Present`.
- `bench`
  Writes the current renderer timing summary to `benchmarks.txt`.

Skybox / RT environment repro workflow:

- Preserve the run with `+logfile M:/Raze/tools/logs/<name>.log` so the default `duke-rt.log` does not get overwritten.
- For skybox source-selection issues, prefer `+set nri_pttraceframes 128` or higher. A value like `8` is often too short to catch a later downgrade after moving into another area.
- When the skybox is expected to remain stable for a level, `NRI PT sky: ... action=hold-level-cubemap` is the expected trace if the current view only sees weaker wall/flat sky surfaces.
- If the sky really changes, the trace should show a different cubemap key or an explicit solid-sky activation instead of silent drift.

Example:

```text
nri_api vulkan
restart
nri_ptsanity 1
nri_ptbuffers
nri_ptsurfaceprobe 2
stat rendertimes
nri_ptstatus
nri_ptsanity 0
nri_ptbootstrap 1
nri_ptbootstrapmode 1
nri_ptcaps
nri_ptdebug 1
nri_ptstatus
```

Current scope of the RT debug path:

- The path-traced 3D view now covers opaque walls, floors/ceilings, mirror surfaces, translated portal child views, facing sprites, and voxel-backed model draw items through conservative voxel proxies.
- Menus, HUD, and other 2D presentation still run through the minimal NRI raster path.
- When frame generation is enabled with `nri_framegenui 2`, HUD and menu content are split to a dedicated UI texture so the FG scene input can remain HUD-less.
- Denoising, TAA, NIS, DLSS-SR, and DLRR stay in the same backend path, with automatic fallback when the requested upscaler is unsupported.
- Frame generation currently uses the FSR3 provider path on D3D12 and falls back cleanly when policy or runtime requirements are not met.
- `nri_ptbootstrap 1` intentionally bypasses all of those PT stages so the backend can validate the minimal in-level world presentation path first.
- `nri_ptbootstrapmode 0..13` now form a bootstrap ladder:
  `0` pattern, `1` analytic plane, `2` hardcoded triangle, `3` textured quad, `4` scene signature, `5` raw vertex scatter, `6` raw primitive scatter, `7` scene points, `8` scene centroids, `9` scene wireframe, `10` first scene triangle, `11` captured scene flat shading, `12` captured scene base color, `13` full PT.
- Generic non-voxel `GLDL_MODELS` content still falls back to the raster path with explicit logging instead of silently disappearing from the RT scene.
- Remaining limits are centered on full generic model conversion, translucency, masked portal edge cases, and deeper portal recursion tuning.
