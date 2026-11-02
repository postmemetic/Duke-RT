# Sector Color Authoring Guide

Sector-color profiles and overrides are authored in `LIGHTOVR`. See the [LIGHTOVR authoring guide](LIGHTOVR-AUTHORING.md#lightovr-authoring-and-usage) for file loading and writable-overlay setup, and the [emissive light editor](LIGHTOVR-AUTHORING.md#emissive-light-override-edit-mode) for placing and editing fixtures.

## How sector color controls emitted light

The NRI renderer can transfer environmental palette effects to lights. It blends a replacement emission hue with each source's authored color, preserving linear luminance at a fixed gain. A separate gain controls emitted light output. Texture detail and some original color remain at partial strength. Walls and flats use their original texture colors; illumination supplies the room color, including its reflections on water. Intrinsic actor lights, sunlight, sky palettes, fog and fluid body colors keep their existing meaning.

Run `lightoverlay_generatesectorcolors` to seed Duke environmental profiles for palettes 1 (blue), 2 (red), 7 (amber), and 8 (green) in the writable `LIGHTOVR`. Existing profiles and overrides are preserved on repeated runs. These are initial art-direction values, intended for later tuning. Other palettes remain untouched.

```text
LIGHTOVR
{
    sectorcolor "DukeEnvironmentPalette2"
    {
        palette 2
        color 1.0 0.10 0.025
        strength 0.45
        gain 0.90
        enabled on
        neutralize on
    }
    map "E2L9"
    {
        sectorcolor "Submarine"
        {
            sector 36
            source floor
            color 1.0 0.08 0.02
        }
    }
}
```

Global `sectorcolor` blocks require `palette 1..255` and a positive RGB color when enabled. Map-local blocks require `sector`; optional `color` replaces the profile hue. `source auto|floor|ceiling` defaults to `auto`: prefer a configured floor palette, then a configured non-sky ceiling palette. Explicit `source ceiling` can use a sky ceiling as a signal without stripping its sky texture palette. RGB values are linear and normalized to unit luminance, so brightness remains controlled by the emitter. Different rules targeting the same palette or sector use the last applicable rule in load order.

`strength` is a finite value from 0 to 1: 0 retains authored source color, 1 fully replaces its hue. `gain` is finite and nonnegative: 0 extinguishes the linked emission, 1 retains its luminance, and values below 1 dim it. Omitting both fields preserves the original full-strength/unit-gain behavior. Map overrides inherit each unspecified field independently from the selected palette profile, including when they override RGB. A local numeric correction also applies when its hue is borrowed through a portal. These fields affect visible fixtures and their emitted/reflected light together; `strength 0` still neutralizes the configured surface palette.

For linear source emission `E`, normalized target `H` and Rec709 luminance `Y(E)`, output is `gain * ((1-strength)*E + strength*Y(E)*H)`. This replaces hue progressively rather than multiplying the source by a color filter.

`enabled off` at sector scope disables transfer and restores the sector's original palette rendering. `neutralize off` keeps the original surface palette while still applying the lighting hue. At global scope these fields control that palette profile. Inherited environmental palettes on world actors and voxels are bypassed before they overwrite intrinsic colors. Duke weapons, shrunken hands and scuba HUD retain their sector palette tint. Selective art palettes and explicit intrinsic sprite palettes remain intact.

Add these optional fields to an existing `light`, `surfacelight`, or `emissiveoverride`:

```text
colorresponse auto
colorsignal sector 36
```

`colorresponse off` keeps the emitter's authored hue. `auto` and `on` enable hue transfer. `colorsignal` selects an independent live color source; the existing `signal sector` continues to control brightness. Without an explicit color source, the existing brightness signal is preferred, then the emitter's own sector. Neutral sectors can inherit from an environmental color source through at most two open, unmasked Build portals; closest hop wins, with lower sector number breaking ties. Explicit `colorsignal` never searches neighbors. Map-sector `enabled off` blocks automatic propagation through that sector. Actor emission remains independently colored; these controls govern map emission and placed fixtures.

Both an authored `surfacelight`'s visible fixture and its analytic light use the same policy. Natural map emissive textures use it automatically. This does not create light in a room with no emitter; add or adjust a fixture with the existing tools where needed.

In `nri_ptemissivelighteditmode`, aim at a surface and press **C** to create an editable sector rule without replacing an existing correction. **P** still creates an emitter override, now with a color response field; **L** reloads edits. Console tools:

- `nri_ptsectorcoloredit <sector> auto` restores profile-driven hue, strength and gain for that sector.
- `nri_ptsectorcoloredit <sector> off` disables transfer for that sector.
- `nri_ptsectorcoloredit <sector> <r> <g> <b>` writes a hue override, preserves numeric controls and reloads it.
- `nri_ptsectorcoloredit <sector> strength <0..1>` changes color strength while preserving RGB and gain.
- `nri_ptsectorcoloredit <sector> gain <value>` changes output while preserving RGB and strength.
- `nri_ptsectorcolordump [sector]` reports live palette, chosen signal, portal distance, palette neutralization, strength and gain.
- `nri_ptemissivedump [radius] [limit]` includes each bound emitter's effective hue and color source.
- `nri_ptsectorcolorpreview <sector> <floorpal> <ceilingpal>` changes live palette state while emissive edit mode is enabled. This writes no overlay; reload the save to restore gameplay state.
- `nri_ptsectorcolor false` disables the feature for comparison; `true` restores it.

Policy reload and the master toggle refresh resident materials before palette-aware texture mip generation. Original palette metadata is retained for material matching and fluid classification. Color-only edits update emission without rebuilding geometry. Save/load and gameplay palette switches are read from live sectors, including linked sectors outside the current view.

## Editing a room in game

The automatic blue, red, amber and green profiles use strength `0.45` and gain `0.90`. The deep lava chamber in The Abyss (`E1L5`, sector `316`) has a local `0.35` / `0.80` correction; its neighboring lava stream keeps the global response. Map-local rules let you tune individual areas without changing the entire palette profile.

1. Open the console and run `nri_ptactorlighteditwritable` to see which loose `LIGHTOVR` will receive edits. Then run `nri_ptsectorcolor true` and `nri_ptemissivelighteditmode 1`.
2. Close the console, aim at a wall, floor or ceiling in the area, and press **C**. Open the console again to read the reported `sector=N`. This selects the aimed surface's sector, which can differ from the player's sector or an emitter's linked color source. An existing correction is preserved.
3. Inspect `nri_ptsectorcolordump N`. Its `source` is the chosen hue source; `hops` describes portal borrowing. `nri_ptemissivedump 4096 100` shows nearby emitter bindings. Use the selected sector for a local adjustment, or the reported source to affect emitters borrowing from it. An explicit emitter `colorsignal` or brightness `signal` can direct it elsewhere.
4. Adjust values using the sector number. For example, while in The Abyss:

```text
nri_ptsectorcoloredit 316 strength .35
nri_ptsectorcoloredit 316 gain .80
```

Each command saves and reloads immediately; no separate save action is needed. Strength blends hue into the original emission, and gain scales output. Both affect visible sources and the light they cast. Use `.45` / `.90` for the general response, or intermediate values to taste. RGB can be changed independently, for example `nri_ptsectorcoloredit 316 1 .10 .025`; RGB values are linear and normalized, so use gain to adjust brightness.

`nri_ptsectorcoloredit 316 auto` clears the RGB, strength and gain corrections and enables profile inheritance again. It preserves source selection and `neutralize`. `off` disables transfer for that sector and restores its original surface palette. `nri_ptsectorcolor false` / `true` toggles the whole feature for comparison. Finish with `nri_ptemissivelighteditmode 0`.

Edits persist in the last mounted writable loose `LIGHTOVR`, independently of game saves. Archive sources cannot be edited. The editor serializes the merged database into that file; keep a backup if preserving hand formatting matters. Changes in a testing overlay do not automatically synchronize the shipped `release-overlay/LIGHTOVR`: copy the intended map-sector rules back when keeping them. **L** in edit mode or `lightoverlay_reload` reloads external text edits.
