# FrameForge

**ES** · [EN](#english)

FrameForge es una aplicación para Windows, en la línea de Lossless Scaling, que **no inyecta nada en el juego**:
captura la ventana del juego con Windows Graphics Capture (WGC), calcula su propio flujo óptico en la GPU,
escala opcionalmente (FSR 1 EASU+RCAS o bilineal) y presenta fotogramas generados (x2/x3/x4) en una ventana
superpuesta, transparente al ratón y excluida de la captura.

## Estado

| Parte | Estado |
|---|---|
| Compilación MSVC + CMake en GitHub Actions | sí |
| Prueba sin GPU (WARP): flujo, oclusión, máscara HUD, interpolación, FSR1, nº de fotogramas | automatizada en CI |
| Captura WGC, overlay, ritmo y latencia en una GPU real con un juego real | **sin probar** (CI no tiene GPU) |
| FSR 3.1 / XeSS / "Modo IA" | no incluido en esta versión (ver hoja de ruta) |

No se publican cifras de rendimiento ni de calidad porque aún no se han medido en hardware real.

## Cadena de proceso

`captura WGC → recorte al área cliente → pirámide de luminancia → flujo óptico (ida y vuelta) → oclusión + máscara HUD → escalado espacial → generación de fotogramas → overlay`

* **Captura:** WGC sobre la ventana elegida, sin cursor, sin borde (si el sistema lo permite) y
  `MinUpdateInterval` = 1 ms cuando la API existe. Grupo de fotogramas *free-threaded* con 2 búferes.
* **Flujo óptico propio** (sombreadores de cómputo D3D11, `shaders/`): *block matching* jerárquico en
  pirámide (búsqueda completa en el nivel más grueso, candidatos del nivel superior y vecinos en los
  demás), refinamiento a 1, ½ y ¼ de píxel, mediana vectorial 3×3. Se calcula hacia delante y hacia atrás;
  la comprobación de consistencia produce la máscara de oclusión/desoclusión.
* **Máscara HUD:** píxeles que no cambian durante N fotogramas reales y tienen contraste local (bordes,
  texto). Esos píxeles no se deforman: se copian del fotograma real más cercano.
* **Interpolación:** deformación bidireccional con mezcla consciente de oclusiones (en zonas ocultas se usa
  el fotograma que ve el píxel).
* **Presentación:** cadena de intercambio DXGI *flip model* (`FLIP_DISCARD`), *tearing* permitido si el
  sistema lo soporta y está activado (VRR), objeto *waitable* con latencia máxima 1,
  `WDA_EXCLUDEFROMCAPTURE` en el overlay. Cada fotograma real llega al overlay tras sus M-1 intermedios, que
  se reparten según el intervalo de captura medido; el primero sale de inmediato, así que el real se
  retiene como mucho (M-1)/M de un intervalo.
* **Robustez:** cambio de tamaño (se recrea el *pool* y los recursos), minimizado (se oculta el overlay), si
  fallan los sombreadores se pasa a modo de paso directo, si se pierde el dispositivo se detiene la captura.

### AMD FidelityFX FSR 3.1 y XeSS

La CI consulta en cada ejecución la última versión y la licencia de FidelityFX SDK y de XeSS (ver el resumen
del *job*). La interpolación de fotogramas de FSR 3.1 necesita profundidad y vectores de movimiento del motor,
además del *backend* D3D12/Vulkan del SDK; un fotograma capturado no tiene profundidad ni *jitter*. Por eso esta
versión usa el camino propio y solo escaladores espaciales. **Los fotogramas capturados no llevan jitter ni
profundidad, así que un escalador temporal tendría menos información que dentro del motor.**

## Uso

Ver [`packaging/LEEME.txt`](packaging/LEEME.txt) (se incluye en el zip).

## Compilar

```
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\frameforge_tests.exe
```

## Hoja de ruta

1. Medir en GPUs reales (latencia con herramientas externas, coste en ms por paso).
2. FSR 3.1 *upscaling* (D3D12) alimentado con nuestros vectores de movimiento.
3. XeSS SR si la licencia permite redistribuir `libxess.dll`.
4. "Modo IA" opcional: profundidad monocular (ONNX + DirectML) para oclusiones ordenadas por profundidad
   y SR por red neuronal con licencia permisiva. Desactivado por defecto.

## Licencias de terceros

Ver [`THIRD_PARTY.txt`](THIRD_PARTY.txt): Dear ImGui (MIT), AMD FSR 1 (MIT), d3dcompiler_47 (Microsoft).

---

<a name="english"></a>
## English

FrameForge is a Windows app in the spirit of Lossless Scaling that **injects nothing into the game**. It captures
the game window with Windows Graphics Capture, computes its own GPU optical flow (hierarchical block matching on a
pyramid with sub-pixel refinement, forward and backward, consistency-based occlusion masks, a static-HUD mask),
optionally upscales spatially (FSR 1 EASU+RCAS or bilinear), generates x2/x3/x4 frames with occlusion-aware
bidirectional warping, and presents them on a click-through, capture-excluded, flip-model overlay.

**Status:** builds in CI; a headless WARP test checks flow accuracy, occlusion and HUD-mask sanity,
interpolation error against ground truth, FSR1 output and frame counts. Capture, overlay, pacing and latency
on a real GPU with a real game are **untested**. No performance or quality figures are claimed.

**Temporal upscalers:** captured frames are not jittered and have no depth, so FSR 3.1 or XeSS would have less
to work with than inside an engine. This release ships spatial upscalers only.

Usage: see `packaging/LEEME.txt` (Spanish). Run the game windowed/borderless, pick it in the list (or press
Ctrl+Alt+F on it), choose a multiplier and an upscaler. Settings: `frameforge.ini`; log: `frameforge.log`.
