# BodyMorph VR

BodyGen for Fallout 4 VR 1.2.72 (an F4SE plugin written from scratch, GPL-3.0-or-later). It gives every NPC their own body shape:
it deforms the body and the clothes with the BodySlide `.tri` morphs, rolls the bodies from BodyGen files (`templates.ini` / `morphs.ini`),
saves each NPC's body with your game and implements the `BodyGen.*` Papyrus functions of LooksMenu, which has no VR version.

## Install (3 steps)
1. Install the zip with Mod Organizer 2 (it opens a short installer; pick "Silhouette bodies, more visible (x2)" unless you have your own BodyGen files).
   Requires: Fallout 4 VR 1.2.72, F4SE VR, VR Address Library for F4SEVR.
2. In BodySlide choose the preset **"CBBE Zeroed Sliders"**, tick **"Build Morphs"** and run **Batch Build** for your body and outfits.
   This writes the `.tri` files that this mod deforms. Without them nothing changes. For men: BodyTalk 4 (Nude body + clothing), built the same way.
3. Play. NPCs get a body the first time the game builds them, and keep it when you save and load.

BodyTalk note: its flat-game hand meshes (`MaleHands.nif`, `1stPersonMaleHands.nif`, `1stPersonMaleBody.nif`) make VR hands disappear. Hide those 3 files in MO2.

## Files
- `F4SE\Plugins\BodyMorphVR.ini`: settings (every key is commented). Test keys are off by default.
- `Data\F4SE\Plugins\F4EE\BodyGen\Loose\*_templates.ini / *_morphs.ini`: who gets which body.
- Log: `Documents\My Games\Fallout4VR\F4SE\BodyMorphVR.log`.

## Limits
Only meshes with a `.tri` (built with Build Morphs) change; the vanilla nude body has no sliders. The player is not morphed. A live change can show a few seconds late.

## Credits
ousnius and Caliente (BodySlide and the `.tri` format), ReidenXerx (Silhouette: BodyGen format documentation and the included data files, GPL-3.0),
TheBottomhoodofSteel (BodyTalk), ArthurHub (CommonLibF4VR for VR), the F4SE team. Developer notes: `docs/DEVELOPMENT.md`.
