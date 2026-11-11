Scriptname BodyGen Native Hidden
; BodyMorph VR (GPL-3.0-or-later). Native declarations only: BodyMorphVR.dll implements them (F4SE papyrus interface).
; Same functions and signatures as the BodyGen script Silhouette's scripts call, so they run unchanged.

; Sets the value of a morph (a BodySlide slider name, or any marker name) under a keyword; None = the plain key. 0 removes it.
Function SetMorph(Actor akActor, Bool isFemale, String morph, Keyword akKeyword, Float value) Native Global
; The value stored under exactly that keyword (None = the plain key); 0.0 when there is none.
Float Function GetMorph(Actor akActor, Bool isFemale, String morph, Keyword akKeyword) Native Global
Function RemoveMorphsByName(Actor akActor, Bool isFemale, String morph) Native Global
Function RemoveMorphsByKeyword(Actor akActor, Bool isFemale, Keyword akKeyword) Native Global
Function RemoveAllMorphs(Actor akActor, Bool isFemale) Native Global
Keyword[] Function GetKeywords(Actor akActor, Bool isFemale, String morph) Native Global
String[] Function GetMorphs(Actor akActor, Bool isFemale) Native Global
; Clears the actor's morphs and runs BodyGen again; applies them when update is True.
Function RegenerateMorphs(Actor akActor, Bool update) Native Global
; Applies the actor's stored morphs to its body now.
Function UpdateMorphs(Actor akActor) Native Global
Function ClearAll() Native Global
; Skin overrides are not supported by BodyMorph VR: both return False.
Bool Function SetSkinOverride(Actor akActor, String id) Native Global
Bool Function RemoveSkinOverride(Actor akActor) Native Global
