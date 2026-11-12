# BodyMorph VR

Body variety for Fallout 4 VR 1.2.72 (an F4SE plugin written from scratch, GPL-3.0-or-later). Every NPC gets one of the **BodySlide presets you have installed**
as their body: the plugin reads BodySlide's own preset files, deforms the body and the clothes with the BodySlide `.tri` morphs while the game builds the NPC,
and saves each NPC's body with your game. It also reads BodyGen files (`templates.ini` / `morphs.ini`) and implements the `BodyGen.*` Papyrus functions of LooksMenu,
which has no VR version.

## Install (3 steps)
1. Install the zip with Mod Organizer 2 (a short installer opens).
   Requires: Fallout 4 VR 1.2.72, F4SE VR, VR Address Library for F4SEVR, BodySlide, a slider body (CBBE; BodyTalk 4 for men).
2. Once, in BodySlide: preset **"CBBE Zeroed Sliders"**, tick **"Build Morphs"**, **Batch Build** your body and outfits.
   This writes the `.tri` files that this mod deforms (one per mesh, with every slider in it). Without them nothing changes; the game shows a notice if they are missing.
3. Play. NPCs get a body the first time the game builds them, and keep it when you save and load.

BodyTalk note: its flat-game hand meshes (`MaleHands.nif`, `1stPersonMaleHands.nif`, `1stPersonMaleBody.nif`) make VR hands disappear. Hide those 3 files in MO2.

## Adding your own bodies
Install any BodySlide preset (a file in `Tools\BodySlide\SliderPresets`). That is all: nothing to build, nothing to convert. At the next game start it is in the draw.
- How often a preset comes up: `[PresetWeights]` in `BodyMorphVR.ini` (`Preset name=3`; `0` = never). Presets without a line are weighted by themselves:
  the mildest third of your presets 3, the middle 2, the most extreme 1.
- Presets named "(Outfit)", "Zeroed" and "CBBE Vanilla" are left out (`sExclude`). A preset made for another body (its sliders are not in your `.tri`) is left out too.
- The log lists every preset, its weight and its share: `Documents\My Games\Fallout4VR\F4SE\BodyMorphVR.log`, lines `[presets]`.

## Files
- `F4SE\Plugins\BodyMorphVR.ini`: settings (every key is commented). Test keys are off by default.
- Optional: `Data\F4SE\Plugins\F4EE\BodyGen\Loose\*_templates.ini / *_morphs.ini` (BodyGen files). A line that names one NPC (FormID) or a plugin wins over the presets;
  the installer can add Silhouette's files so that named people get their own body. Examples: `F4SE\Plugins\BodyMorphVR_examples`.

## Limits
Only meshes with a `.tri` (built with Build Morphs) change; the vanilla nude body has no sliders. The body must be built with zeroed sliders
(otherwise set `sFemaleBuiltWith` / `sMaleBuiltWith`). The player is not morphed. A live change can show a few seconds late.

## Credits
ousnius and Caliente (BodySlide, its preset / slider set files and the `.tri` format), ReidenXerx (Silhouette: BodyGen format and BodySlide behaviour documentation, and the optional data files, GPL-3.0),
TheBottomhoodofSteel (BodyTalk), ArthurHub (CommonLibF4VR for VR), the F4SE team. Developer notes: `docs/DEVELOPMENT.md`.
