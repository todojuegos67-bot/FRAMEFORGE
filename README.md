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
| Contador FPS en pantalla (Ctrl+Alt+H) | implementado; sin probar en juego |
| FSR 3.1 (D3D12 + interop D3D11) | compila; en WARP la creación del contexto falla → **sin verificar** |
| XeSS SR (D3D11) | compila; en WARP `xessD3D11CreateContext` falla (requiere GPU Intel) → **sin verificar** |
| Modo IA (profundidad, ONNX Runtime + DirectML) | el modelo carga y infiere en CI (CPU, ~70-130 ms a 266x154); DirectML y efecto en juego **sin verificar** |

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

## Calidad de interpolación medida (v0.2)

Middlebury "other" (12 secuencias reales/sintéticas con fotograma intermedio real `frame10i11`),
t = 0,5, ejecutado en CI con WARP. Media PSNR (RGB) / SSIM (luma):

| Método | PSNR dB | SSIM |
|---|---|---|
| repetir fotograma anterior | 24,75 | 0,7683 |
| mezcla lineal | 28,00 | 0,8146 |
| v0.1 (flujo por bloques 4x4) | 34,32 | 0,9629 |
| v0.2 flujo denso sin respaldo | 34,40 | 0,9652 |
| **v0.2 por defecto** (denso + respaldo de baja confianza 0,40) | **34,36** | **0,9650** |
| v0.2 denso + respaldo agresivo 0,08 | 32,47 | 0,9565 |

La mejora frente a v0.1 en esta prueba es **pequeña** (+0,04 dB, +0,002 SSIM). El respaldo
("no deformar si la confianza es baja") baja algo el PSNR pero evita ghosting grosero; la copia 1:1
de píxeles estáticos no cambia estas cifras. No se ha medido en capturas de juegos reales.
Los datos de Middlebury solo se descargan en CI para evaluar; no se redistribuyen.

## Uso

Ver [`packaging/LEEME.txt`](packaging/LEEME.txt) (se incluye en el zip).

## Compilar

```
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\frameforge_tests.exe
```

## Hoja de ruta

1. Medir en GPUs reales (latencia, coste en ms por paso, FSR 3.1/XeSS/DirectML).
2. Evaluar con capturas reales de juegos (HUD, partículas, transparencias).
3. Refinar el flujo con bordes de profundidad (no hecho) y SR neuronal (no hecho).

## Licencias de terceros

Ver [`THIRD_PARTY.txt`](THIRD_PARTY.txt) y la carpeta `licenses/` del zip: Dear ImGui (MIT), AMD FSR 1 (MIT),
FidelityFX SDK v2.3.0 (cabeceras MIT; **DLLs firmadas con licencia binaria de AMD, no MIT**), Intel XeSS
(Intel Simplified Software License: redistribución binaria permitida sin modificar), ONNX Runtime (MIT),
DirectML (licencia redistribuible de Microsoft), Depth-Anything-V2-Small (Apache-2.0, se descarga al primer
uso y se verifica con SHA-256; no se incluye).

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
to work with than inside an engine. v0.2 adds FSR 3.1 (D3D12 interop) and XeSS SR (D3D11, Intel GPUs) fed with our optical-flow vectors and constant or AI-estimated depth; neither could be verified in CI (WARP).

**Measured (Middlebury, 12 sequences, t=0.5):** v0.1 34.32 dB / 0.9629 SSIM; v0.2 default 34.36 dB / 0.9650; repeat-previous 24.75 / 0.7683. The gain over v0.1 is small.

Usage: see `packaging/LEEME.txt` (Spanish). Run the game windowed/borderless, pick it in the list (or press
Ctrl+Alt+F on it), choose a multiplier and an upscaler. Settings: `frameforge.ini`; log: `frameforge.log`.
