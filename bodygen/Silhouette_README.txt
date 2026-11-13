Silhouette BodyGen files
========================
Silhouette_templates.ini and Silhouette_morphs.ini are part of "Silhouette" 0.5.5 by ReidenXerx / Dudu'sButt
(https://github.com/ReidenXerx/fo4-silhouette, Nexus Fallout 4 mod 109439), licensed GPL-3.0 (see Silhouette_LICENSE_GPL-3.0.txt).
They are NOT changed here, except in the "x1.5" variant, where the morph values of the female templates are
multiplied by 1.5 (tools/scale_templates.py in the source, github.com/vilaherba-design/BodyMorphVR, does it).
Only these two data files are used: Silhouette's DLL, ESP and scripts are for the flat game and are not part of this mod.
Their values are written for bodies built in BodySlide with zeroed sliders ("CBBE Zeroed Sliders" + Build Morphs).
Rules for plugins you do not have are ignored (the log lists them).
Since BodyMorph VR 0.8.0 only their lines for named people (FormID lines) are used by default: everybody else gets a BodySlide preset
([Presets] bOverGeneralRules=1 in BodyMorphVR.ini; set it to 0 to let Silhouette's "All|..." lines decide instead).
