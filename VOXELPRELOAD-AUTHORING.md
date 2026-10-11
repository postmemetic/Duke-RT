# VOXELPRELOAD Authoring Guide

`VOXELPRELOAD` is Duke-RT's mounted overlay list for warming voxel actor variants during map loading. It is intended for voxel-heavy overlays where important props, enemies, projectiles, explosions, and animated picnum ranges should be prepared before their first runtime appearance.

The preload system has two stages:

- CPU warmup builds durable voxel geometry/material variants and prepares cache entries.
- GPU admission uploads selected variants and builds their BLAS resources, subject to preload budgets and safety pacing.

`VOXELPRELOAD` entries express desired preload priority. They do not mean "synchronously upload everything immediately." Runtime budgets and GPU admission safeguards still apply.

## File Placement

Place a root-level file named `VOXELPRELOAD` in a mounted overlay, for example:

```text
M:\Raze\full-voxel-overlay\VOXELPRELOAD
```

All mounted root-level `VOXELPRELOAD` lumps are scanned when voxel loading requests are collected. Loose overlay files can be mounted with the normal `-file` workflow.

## Basic Structure

A `VOXELPRELOAD` file starts with the marker line, then one or more active sections:

```text
VOXELPRELOAD

global
picrange 1891-1910 priority=force gpu=prefer reason=explosion-tiles

map E1L1
pic 913 priority=force gpu=prefer reason=start-area-fence

game worldtour
actor DukeExplosion2 picrange=1891-1910 priority=force gpu=prefer
```

Supported sections:

- `global`
  Applies whenever the overlay is mounted.
- `game <name>`
  Applies only when the current game filter matches.
- `map <label>`
  Applies only when the current level label matches, such as `E1L1`.

Supported game filters currently include:

```text
duke
worldtour
shareware
nam
namonly
napalm
ww2gi
redneck
redneckrides
blood
shadowwarrior
sw
exhumed
plutopak
```

The parser is line-oriented. Blank lines are ignored. `#` and `//` start comments. Commas, braces, and semicolons are treated as whitespace, so authors may use a loose, readable style.

## Request Directives

### `pic`

Requests one tile picnum.

```text
pic 913 priority=force gpu=prefer reason=e1l1-fence
```

Use this for a known voxel tile that should be available as a specific frame/state.

### `picrange`

Requests every voxel-backed tile in a picnum range. Both forms are accepted:

```text
picrange 2270 2283 priority=force gpu=prefer reason=small-fire-loop
picrange 2310-2323 priority=force gpu=prefer reason=large-fire-loop
```

Ranges are clamped to valid tile ids. Tiles without voxel replacements are skipped.

### `actor`

Requests variants associated with an actor class. Actor names must resolve to engine actor classes.

Request specific picnums:

```text
actor DukeExplosion2 picnums=1891,1892,1893 priority=force gpu=prefer
actor DukeExplosion2 picnums 1891 1892 1893 priority=force gpu=prefer
```

Request a picnum range:

```text
actor DukeExplosion2 picrange=1891-1910 priority=force gpu=prefer
actor DukeExplosion2 picrange 1891 1910 priority=force gpu=prefer
```

Request the actor default sprite/dispictex plus the configured neighboring picnum scan range:

```text
actor DukeRPG allpicnums priority=force gpu=prefer
```

`allpicnums` is useful for actors whose visible frame set is near the actor default picnum. For explicit animation families, authored `picrange` or `picnums` entries are more predictable.

### `texture`

Requests a raw texture id directly:

```text
texture 12345 priority=high gpu=prefer
```

This is mainly a low-level escape hatch. Prefer `pic`, `picrange`, or `actor` for ordinary voxel authoring.

## Options

Options can be written as `name=value` or `name value`.

```text
pic 1696 priority=force gpu=prefer reason=e1l1-air-car
pic 1696 priority force gpu prefer reason e1l1-air-car
```

### `priority`

Supported values:

- `force`
- `high`
- `normal`
- `opportunistic`

Default priority is `high`.

Priority controls request ordering. Map-local requests and stronger priorities are selected before broad opportunistic work.

### `gpu`

Supported values:

- `force`
- `prefer`
- `none`
- `false`
- `0`

If `gpu` is omitted, the request is CPU warmup only. `gpu=prefer` asks loading-screen GPU admission to upload/build the variant when budgets allow. `gpu=force` marks the request as stronger GPU intent, but it is still subject to hard safety checks and admission pacing.

The default loading policy is whitelist-oriented: loading-screen GPU admission favors entries selected by `VOXELPRELOAD` with `gpu=force` or `gpu=prefer`. Non-whitelisted entries can still be built on CPU and admitted later through the runtime queue.

### `reason`

`reason` is accepted as an authoring annotation and is useful in examples and diffs:

```text
picrange 1891-1910 priority=force gpu=prefer reason=spawned-explosion-actor
```

It is not used for matching or priority.

## Practical Examples

Preload a short-lived spawned explosion globally so it does not appear for one frame and then vanish while variants trickle in:

```text
global
actor DukeExplosion2 picrange=1891-1910 priority=force gpu=prefer reason=spawned-explosion-actor
picrange 1891-1910 priority=force gpu=prefer reason=spawned-explosion-tiles
```

Preload weapon projectiles that can first appear on any map:

```text
global
actor DukeRPG allpicnums priority=force gpu=prefer reason=projectile-rpg-actor
pic 2605 priority=force gpu=prefer reason=projectile-rpg-missile
actor DukeFreezeBlast allpicnums priority=force gpu=prefer reason=projectile-freeze-actor
```

Preload map-local hero props and animated fire loops:

```text
map E1L1
pic 1696 priority=force gpu=prefer reason=e1l1-air-car
pic 913 priority=force gpu=prefer reason=e1l1-fence
pic 909 priority=force gpu=prefer reason=e1l1-big-tree
picrange 2270-2283 priority=force gpu=prefer reason=e1l1-small-fire-loop
picrange 2310-2323 priority=force gpu=prefer reason=e1l1-large-fire-loop
```

Preload a large enemy animation family opportunistically for a map where it appears later:

```text
map E2L1
picrange 2908-2938 priority=high gpu=prefer reason=e2l1-large-enemy-variants
picrange 2953-2983 priority=high gpu=prefer reason=e2l1-large-enemy-variants-b
```

## Runtime Controls and Diagnostics

Useful controls while validating authoring:

- `r_voxels`
  Master voxel rendering toggle. If false, `VOXELPRELOAD` requests are not collected.
- `nri_ptloadingvoxellist`
  Enables mounted `VOXELPRELOAD` list scanning. Default is true.
- `nri_ptloadingvoxelcpu`
  Enables CPU-side voxel preload work. Default is true.
- `nri_ptloadingvoxelgpu`
  Enables loading-screen GPU voxel admission. Default is true.
- `nri_ptloadingvoxelgpuwhitelistonly`
  When true, loading-screen GPU admission is restricted to authored GPU candidates. Default is true.
- `nri_ptloadingvoxelpicrange`
  Controls the neighboring picnum scan used by `actor ... allpicnums`. Default is 16.
- `nri_ptloadingtrace`
  Set to `1` or `2` to log preload collection and admission decisions.

Relevant log prefixes:

```text
NRI PT loading voxel preload list:
NRI PT loading voxel preload:
NRI PT loading voxel CPU:
NRI PT voxel admission queue:
NRI PT voxel admission summary:
NRI PT voxel admission entry:
```

Example launch fragment for validation:

```text
+set nri_ptloadingtrace 2 +set nri_voxelstats true +logfile voxelpreload-test.log
```

Expected signs of correct parsing:

- `NRI PT loading voxel preload list` reports at least one source.
- `skipped_syntax`, `skipped_actor`, and `skipped_unsupported` remain low or zero for authored entries.
- Authored lines appear as `action=request`.
- GPU-intended entries show `gpu=force` or `gpu=prefer`.

## Authoring Guidance

- Use `global` for spawned effects, projectiles, and weapon-carried actors that can appear on many maps.
- Use `map <label>` for large static props, map-specific enemy families, and high-visibility animation loops.
- Prefer exact `pic` or tight `picrange` entries for known animation families.
- Use `actor ... allpicnums` only when the actor's relevant variants are close to its default picnum or when broad actor warmup is acceptable.
- Mark only important entries with `gpu=force` or `gpu=prefer`; broad CPU warmup is cheaper and can still help runtime admission.
- Keep `gpu=force` for assets that are visually disruptive when delayed, such as explosions, projectile bodies, start-area hero props, or major animated loops.
- If a line parses but does not seem to load, verify the actor class name and confirm the target picnums actually have voxel replacements.
