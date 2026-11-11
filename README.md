# BodyMorph VR (prototipo)

Plugin F4SE para Fallout 4 VR 1.2.72 que deforma el cuerpo de los NPC con los sliders de BodySlide (archivos `.tri`),
compatible con la interfaz BodyGen (`BodyGen.*` en Papyrus, `F4SE\Plugins\F4EE\BodyGen\...`) para que Silhouette funcione en VR.
Escrito desde cero: no contiene ni se basa en código de LooksMenu.

- `src/TriFile.*`: lector `.tri` (formato tal como lo lee BodySlide, GPL-3.0, ousnius / Caliente).
- `src/TriPath.h`: valor de la etiqueta `BODYTRI` -> ruta del `.tri` de cada prenda.
- `src/MorphList.h`: lista "slider:peso" del INI y sufijo `:N` de los nombres de shape.
- `src/MorphStore.h`: tabla de morphs por NPC (la que escriben las funciones Papyrus `BodyGen.*`) y su serialización para el co-save de F4SE.
- `data/Scripts/BodyGen.pex` (+ `data/Source/Scripts/BodyGen.psc`): script Papyrus con las 12 funciones nativas `BodyGen.*` (las firmas que llaman los scripts de Silhouette); `tools/make_bodygen_pex.py` lo regenera.
- `src/MorphApply.h`: aplicación de morphs sobre una copia del bloque de vértices (half / float).
- `tests/run.sh`: tests sin juego (Linux, g++ C++23).
- Compilación: CommonLibF4VR (ArthurHub @ 9057052) en `lib/CommonLibF4VR`, VS 2022, vcpkg.

Licencia: GPL-3.0-or-later (ver LICENSE).

## 0.5.0 (BodyGen: morphs guardados por NPC)

Nuevo en 0.5.0:
- Las funciones Papyrus `BodyGen.SetMorph / GetMorph / GetMorphs / GetKeywords / RemoveMorphsByName / RemoveMorphsByKeyword / RemoveAllMorphs / RegenerateMorphs / UpdateMorphs / ClearAll`
  (y `SetSkinOverride / RemoveSkinOverride`, que devuelven False) están implementadas por el plugin. Necesitan `Scripts\BodyGen.pex` (viene en el zip).
- Cada NPC tiene su tabla de morphs (una por sexo; nombre -> valor por palabra clave; el valor efectivo es el máximo). Se guarda en el co-save de F4SE y se reaplica cuando el juego construye el cuerpo.
- `UpdateMorphs` (y `RegenerateMorphs(actor, True)`) rehacen el cuerpo de un NPC que ya está en pantalla, partiendo siempre de la malla original.
- Teclas de prueba: F3 guarda `sStoreMorphs` ([Test] del INI) para el NPC más cercano y lo actualiza; F4 lo vacía.
- Sin morphs guardados, un NPC conserva su cuerpo (salvo `bUseGlobalSliders=1`, el comportamiento de prueba anterior).
- Aún no incluye el generador de BodyGen (`templates.ini` / `morphs.ini`): `RegenerateMorphs` solo vacía la tabla.

## Prototipo 0.4.0 (la ropa sigue al cuerpo)

Nuevo en 0.4.0: cada prenda se deforma con el `.tri` que indica su etiqueta `BODYTRI` (BodySlide la escribe al construir con "Build Morphs", en el nodo raíz del nif).
Hay que reconstruir cuerpo y ropa con el preset "CBBE Zeroed Sliders" y "Build Morphs" marcado. Una malla sin `BODYTRI` sigue usando el `.tri` global del cuerpo por nombre de shape.
El log trae una línea `[attach]` por cada colocación de cuerpo o prenda (las primeras `iLogAttaches`): qué se encontró y qué se hizo con cada shape.

## Prototipo 0.3.0 (aplicación automática)

Nuevo en 0.3.0: `[Auto]`. El plugin se engancha a `BipedAnim::AttachSkinnedObject` (el mismo punto que usa Head Body Color Match VR) y deforma el cuerpo
de cada NPC femenina mientras el juego la construye, antes de que se dibuje el primer fotograma. No hace falta ninguna tecla ni escaneo de memoria.
F7/F6 siguen existiendo como herramienta de laboratorio.

F8 = sonda (solo registra), F7 = deforma al NPC femenino más cercano con `sMorphs` (sliders del `.tri` de BodySlide),
F6 = restaura. El clon se crea con las funciones del motor; ver el comentario de cabecera de `src/main.cpp`.
Log: `Documents\My Games\Fallout4VR\F4SE\BodyMorphVR.log`. Ajustes en `Data\F4SE\Plugins\BodyMorphVR.ini`.

La 0.2.6 añade `[Lab]` (`iScan`, `iPatchHolders`): el motor dibuja desde "registros de dibujo" que guardan su propia copia del puntero a la malla
(por eso el cambio se veía segundos o minutos tarde). Tras cada F7/F6 se buscan esas copias en la memoria y se redirigen al clon / al original.
