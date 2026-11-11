# BodyMorph VR (prototipo)

Plugin F4SE para Fallout 4 VR 1.2.72 que deforma el cuerpo de los NPC con los sliders de BodySlide (archivos `.tri`),
compatible con la interfaz BodyGen (`BodyGen.*` en Papyrus, `F4SE\Plugins\F4EE\BodyGen\...`) para que Silhouette funcione en VR.
Escrito desde cero: no contiene ni se basa en código de LooksMenu.

- `src/TriFile.*`: lector `.tri` (formato tal como lo lee BodySlide, GPL-3.0, ousnius / Caliente).
- `src/TriPath.h`: valor de la etiqueta `BODYTRI` -> ruta del `.tri` de cada prenda.
- `src/MorphList.h`: lista "slider:peso" del INI y sufijo `:N` de los nombres de shape.
- `src/BodyGenIni.h`: el motor de `templates.ini` / `morphs.ini` (análisis, qué línea decide para un NPC, tirada aleatoria), sin tipos del juego y con tests.
- `src/MorphStore.h`: tabla de morphs por NPC (la que escriben las funciones Papyrus `BodyGen.*`) y su serialización para el co-save de F4SE.
- `data/Scripts/BodyGen.pex` (+ `data/Source/Scripts/BodyGen.psc`): script Papyrus con las 12 funciones nativas `BodyGen.*` (las firmas que llaman los scripts de Silhouette); `tools/make_bodygen_pex.py` lo regenera.
- `src/MorphApply.h`: aplicación de morphs sobre una copia del bloque de vértices (half / float).
- `tests/run.sh`: tests sin juego (Linux, g++ C++23).
- Compilación: CommonLibF4VR (ArthurHub @ 9057052) en `lib/CommonLibF4VR`, VS 2022, vcpkg.

Licencia: GPL-3.0-or-later (ver LICENSE).

## 0.7.0 (instalable: asistente FOMOD)

- El zip trae un instalador FOMOD (`fomod/`): nucleo en `00 Core`, y a elegir los ini de Silhouette x2 (valores de mujer duplicados con `tools/scale_templates.py`), los de Silhouette originales, el ejemplo sencillo o ninguno. Los ini de Silhouette (GPL-3.0, ReidenXerx) van con su licencia y creditos.
- Teclas de prueba y laboratorio (F3, F4, F6, F7, F8) apagadas por defecto en el INI. `bFemaleOnly` documentado: solo afecta a F7 y F3/F4.
- `README.md` nuevo, corto, para usuarios; estas notas pasan a `docs/DEVELOPMENT.md`.

## 0.6.3 (estabilidad)

- `[Lab] iScan` e `iPatchHolders` vienen a 0 por defecto. Con ellos activos, cargar una segunda partida en la misma sesion llego a colgar el driver de NVIDIA (registros de dibujo obsoletos). Los cuerpos siguen viendose igual: el clon ya se sube a la GPU por la via normal.
- Validado en juego con los ini de BodyGen de Silhouette (512 plantillas, 217 reglas).

## 0.6.2 (cargar partida: los cuerpos guardados vuelven)

Al cargar una partida el juego construye a los NPC unos segundos ANTES de que F4SE llame al plugin con el co-save, así que un NPC con cuerpo guardado recibía una tirada de BodyGen y la barriga guardada llegaba tarde y no se aplicaba.
Ahora, entre `PreLoadGame` y `PostLoadGame`, el hook solo registra las mallas y anota el actor; al terminar la carga cada uno recibe sus morphs guardados (o la tirada de BodyGen) con una actualización en vivo (línea `[load]` en el log).

## 0.6.1 (arreglo de un cierre del juego)

La prueba de 0.5.0 cerró el juego en el driver de NVIDIA, 30 s después de pulsar F3 varias veces seguidas sobre la misma NPC: el plugin liberaba una malla clonada que ya estaba sustituida, pero el motor
guarda copias del puntero del vertex buffer en sus "draw records" y siguen apuntando a ella hasta que reregistra la geometría. Ahora una malla que ha llegado a estar en pantalla en una actualización en vivo
(Papyrus `UpdateMorphs`, F3/F4) **nunca se libera** (cuesta 0,1 a 0,7 MB cada una) y una actualización con los mismos valores no vuelve a construir nada. F3/F4 respetan `bFemaleOnly` (F3 eligió a Dogmeat).

## 0.6.0 (el motor de BodyGen: templates.ini / morphs.ini)

Nuevo en 0.6.0: un NPC sin morphs guardados los recibe del generador la primera vez que el juego construye su cuerpo (el resultado se guarda por NPC, como en 0.5.0).
- Lee `Data\F4SE\Plugins\F4EE\BodyGen\<Plugin.esp>\templates.ini` y `morphs.ini` de los plugins cargados (en orden de carga) y `Loose\*_templates.ini` / `*_morphs.ini` (orden de bytes).
  Es el mismo sitio y la misma gramática que usa BodyGen de LooksMenu, así que los archivos de Silhouette valen tal cual. Se leen una vez al arrancar el juego.
- Gramática: `Nombre = morph@bajo:alto , morph@valor / otro conjunto` (`/` conjuntos alternativos, `,` morphs que se aplican juntos, `|` morphs alternativos, `:` valor aleatorio entre dos números) y
  `All|Gender|Race`, `Plugin.esp|All|Gender|Race`, `Plugin.esp|FormID|Gender` `= plantilla|plantilla , plantilla`. Gana la última línea que encaja; las líneas `All` y de plugin solo listan NPCs sin plantilla
  (se sigue la cadena de plantillas del NPC); un FormID puede ser una lista nivelada. La raza se compara por su editor id (`HumanRace`).
- `RegenerateMorphs(actor, update)` vacía la tabla del NPC y vuelve a tirar; un NPC que ya tiene morphs de otro mod (p. ej. AAF) no recibe los de BodyGen, igual que en LooksMenu.
- INI nuevo `[BodyGen]`: `bEnable`, `sFolder`, `iLogActors` (líneas `[bodygen]` en el log: actor, cadena de plantillas, línea que decidió, morphs), `iSeed`.
- Ejemplo para probar: `BodyMorphVR_examples\BodyGen\Loose\` (cópialo a `Data\F4SE\Plugins\F4EE\BodyGen\Loose\`): cada mujer humana recibe una barriga aleatoria o más trasero y pecho.
- Comprobación de IDs de la address library antes de usar funciones del juego nuevas: si falta alguno, esa función se apaga con un aviso en el log en vez de cerrar el juego.

## 0.5.0 (BodyGen: morphs guardados por NPC)

Nuevo en 0.5.0:
- Las funciones Papyrus `BodyGen.SetMorph / GetMorph / GetMorphs / GetKeywords / RemoveMorphsByName / RemoveMorphsByKeyword / RemoveAllMorphs / RegenerateMorphs / UpdateMorphs / ClearAll`
  (y `SetSkinOverride / RemoveSkinOverride`, que devuelven False) están implementadas por el plugin. Necesitan `Scripts\BodyGen.pex` (viene en el zip).
- Cada NPC tiene su tabla de morphs (una por sexo; nombre -> valor por palabra clave; el valor efectivo es el máximo). Se guarda en el co-save de F4SE y se reaplica cuando el juego construye el cuerpo.
- `UpdateMorphs` (y `RegenerateMorphs(actor, True)`) rehacen el cuerpo de un NPC que ya está en pantalla, partiendo siempre de la malla original.
- Teclas de prueba: F3 guarda `sStoreMorphs` ([Test] del INI) para el NPC más cercano y lo actualiza; F4 lo vacía.
- Sin morphs guardados, un NPC conserva su cuerpo (salvo `bUseGlobalSliders=1`, el comportamiento de prueba anterior).
- El generador de BodyGen (`templates.ini` / `morphs.ini`) llegó en 0.6.0.

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
