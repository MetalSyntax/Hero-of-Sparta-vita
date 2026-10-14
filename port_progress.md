# Registro de Progreso — Hero of Sparta (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-10-08)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Hero Of Sparta HD Free_1.1.4.apk` copiado y extraído.
- ABI detectada: armeabi (elegida: armeabi).
- GLES detectado: AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx))

## Fase 2: Decompilación (Completada — 2026-10-08)
- jadx: corrido, resultados en decompiled/apk_jadx/.
- Ghidra (.so): corrido (out_ghidra.c de 42 MB para libheroofsparta + 2.4 MB para libStormGLOFT).

## Fase 3: Análisis del Motor Real (Completada — 2026-10-10)
- **Símbolos con nm/objdump (VITASDK):** `libheroofsparta.so` = 2503 exports, los 18
  `Java_*` del plan, SIN `JNI_OnLoad`. `libStormGLOFT.so` = 1104 exports, CON
  `JNI_OnLoad`, SIN `Java_*`. NEEDED confirma GLES1 (`libGLESv1_CM.so`).
- **Storm no se carga:** su `JNI_OnLoad` (Ghidra `out_ghidra.c:11154`) hace dlopen del
  .so del juego + hook inline (por PID) de glTexImage2D/glViewport/glScissor/
  glGetError = anti-tamper/tracer, no renderer. Idéntica conclusión que el port
  hermano **Dungeon-Hunter-2-vita** (misma familia Gameloft 2012: mismos natives
  GLMediaPlayer/GLResLoader/Device/GameRenderer/Hero + Storm acompañante).
- **Motor hermano reutilizado:** patrones de DH2 adoptados (tablas FalsoJNI con
  `jda_alloc`, mezclador SceAudioOut en hilo propio, file-logging con flush en
  WARN+, salida START+SELECT, `so_sym_or_warn`). Específico de DH2 NO heredado:
  hooks boost/SWP (HoS: 0 instrucciones `swp`), DRM `ALicenseCheck` (HoS no tiene),
  hacks HUDControls (propios de DH2, en HoS se verá en consola).
- **Ciclo de vida real** (GameRenderer.java + pseudo-C): `nativeGetJNIEnv` cachea
  `mEnv`; `GLMediaPlayer/GLResLoader/HeroOfSparta.nativeInit` cachean ~40 method IDs;
  `GameRenderer.nativeInit(1,w,h)` → `appInit()` (g_appPath =
  `/mnt/sdcard/gameloft/games/hos/`); `nativeRender` → `appUpdate`;
  `nativeResize` → viewport; `nativePause/Resume` → appPause/Resume;
  `nativeOnTouch(id,tipo,x,y)` (1=down,2=move,0=up); `SetOnKeyDown/Up`;
  `nativeCanInterrupt` → flag. Totales audio: 96 sonidos + 16 músicas.
- **Assets:** `AFILE::FOpen` manda todo lo no-`.sav`/`.dat` por JNI a
  `GLResLoader.getResourceFull/Bytes/Length(String)` (con chunks `%s00%d`).
  Saves (`Setting.dat`, `Spartan*.dat`) van por fopen directo.
- **Sin RegisterNatives** (no hay `JNI_OnLoad` en el .so del juego): los 18
  `Java_*` se resuelven por nombre con `so_symbol`.
- **Ghidra pendiente de nada:** pseudo-C ya usado para las 3 funciones `nativeInit`
  + `AFILE::FOpen/CacheInfo/CacheChunkData` + `appInit/appUpdate`.

## Fase 4: Bootstrap del Loader (Completada — 2026-10-10)
- `lib/falso_jni/` restaurado (venía vacío: submódulo sin inicializar; copiado del
  hermano DH2, archivos genéricos sin referencias al juego).
- `lib/stb_vorbis/` vendido (`stb_vorbis.c` v1.22, public domain) para decodificar
  los .ogg en runtime sin dependencias extra.
- `source/dynlib.c`: agregados los 17 símbolos que faltaban (helpers `__aeabi_f*`,
  `__aeabi_d2f/d2iz`, `__dso_handle`, `__assert2`). Verificado: 121/121 undefined
  cubiertos (`comm` vacío).
- `source/reimpl/io.c`: `hos_translate_path()` — `/mnt/sdcard/gameloft/games/hos/`
  y `/sdcard/...` → `DATA_PATH hos/`, `/data/data/<paquete>/` → `DATA_PATH saves/`
  (fopen/open/stat/opendir).
- `source/audio.c/h` (nuevo): puente GLMediaPlayer REAL. Decode-on-load con
  stb_vorbis, hilo mezclador SceAudioOut BGM 44100 stereo S16 con resample lineal,
  máquina de estados idéntica a GLMediaPlayer.java (5/6/7/8), `updateSound`
  libera voces terminadas (salvo UI 3,4,5,34), volúmenes separados sfx/música.
  Video = skip (`IsVedioFinished` → 1; `intro.vdo` existe en datos pero sin
  AvPlayer en v1 — se verá en consola si el motor lo exige).
- `source/java.c` (reescrito): 55 métodos registrados — los 22 de GLMediaPlayer que
  el .so busca por GetMethodID, los 3 de GLResLoader con búsqueda multi-raíz
  (res/drawable*, assets/, hos/, ruta directa + log de misses para triage), y
  HeroOfSparta con valores de HeroOfSparta.java (`IsDemo`=0, `getVersionMode`=1,
  `detectPhoneLang`=0/inglés, `Exit`→sceKernelExitProcess, billing/navegador=no-op).
  `generated_jni_stubs.c/.h` quedan como referencia del toolkit (NO compilan;
  `java.c` los reemplaza con implementaciones reales).
- `source/main.c` (reescrito): secuencia `onSurfaceCreated`, `so_sym_or_warn`
  para los 14 entry points, loop con táctil frontal (slot 0, tipos 1/2/0),
  cruceta→DPAD/botones→BACK/MENU, `audio_update_sound()` por frame (réplica del
  `updateSound()` de `onDrawFrame`), salida START+SELECT con `nativePause`/`Done`.
- `CMakeLists.txt`: `SO_PATH` = `ux0:data/heroofsparta/libheroofsparta.so`
  (nombre real), `source/audio.c` agregado, `-Wl,--allow-multiple-definition`
  (igual que Asphalt-6: gana el `egl.c` del reimpl sobre libvitaGL.a).
- `source/utils/logger.c`: log a archivo estándar `logs/heroofsparta_NNN.log`
  (001..999 + `next.idx`), buffer 64 KB, flush en WARN+; `log-standard --fix-dirs`
  OK; `log-trace --ensure-flag` + 307 bloques `[TRACE]` (build OFF por defecto).
- `source/patch.c`: sin cambios (sin DRM/SWP que parchear en v1; triage en consola).

## Fase 5: Build + Empaquetado (Completada — 2026-10-10, en host)
- `psvita-toolkit build --preset debug` OK, `--preset release` OK.
- VPK válido (param.sfo, eboot.bin 607 KB, LiveArea). `livearea --validate`: 5/5 OK.
- `align-check`: limpio (sin ldrd/vld1 ni packing sospechoso).
- Datos reubicados: `ux0_data/hos/` → `ux0_data/heroofsparta/hos/` (el deploy solo
  sube `heroofsparta/`; el pack de 122 MB — oggs + `models/sounds/texturesSPAW/sf/
  gui/clev/TEXT*` + `intro.vdo` — ahora queda bajo `DATA_PATH hos/` como espera
  `g_appPath`). `clean-junk`: 42 `._*` eliminados.

## Fase 6 en adelante: SOLO CONSOLA REAL (en curso)
1. `deploy --vpk` + subir `ux0_data/heroofsparta/` a `ux0:data/heroofsparta/`, correr,
   traer `logs/heroofsparta_001.log` → **un bug a la vez** (skill `so-crash-triage`).
2. Primeras incógnitas esperadas: qué archivo pide `appInit` primero (el pack está
   pero sin verificar contra el motor), si `intro.vdo` bloquea (skip actual),
   mapeo real de `appKeyPressed/Released`, `glViewport` de `nativeResize`
   (Ghidra lo muestra sin args — posible artefacto), rendimiento GLES1.
3. Afinados diferidos a consola: multitouch, botones sintéticos sobre HUD,
   toggles de idioma/volumen persistidos, `PORT_TRACE=ON` si hace falta.

## Bug #1 (2026-10-10): pantalla negra + 28 FPS → 00, sin archivo de log
- Reporte: el juego arranca, pantalla negra, se ven 28 FPS que caen a 00, y no se
  creó ningún `logs/heroofsparta_NNN.log` en consola.
- Hipótesis del log ausente: backend con `fopen/fscanf/setvbuf` de stdio (frágil
  si newlib/SceLibc falla temprano) + en Release solo existen `l_error/l_fatal`,
  así que un cuelgue silencioso no dejaba archivo.
- Fix aplicado (host, sin consola): `source/utils/logger.c` reescrito con
  `sceIo*` directo (open/write/close, sin stdio ni buffers), `sceIoMkdir` sin
  trailing slash, reintento de apertura cada 256 líneas si falló, y cada línea
  baja a disco de inmediato (sin concepto de flush). `main.c` ahora emite banner
  `[boot]` con `l_error` al entrar + marcas de fase tras inits JNI / appInit /
  inicio del loop (visibles también en Release). Debug recompilado OK.
- Siguiente paso en consola: instalar este eboot Debug, correr, traer el log.
  El corte del log dirá en qué fase muere: sin `[boot]` = antes de `main()`;
  sin `fase: appInit OK` = dentro de `appInit` (pedido de asset); con
  `fase: loop iniciado` + `[fps]` cayendo = cuelgue en `nativeRender`.

## Bug #1, análisis del log 001 (2026-10-10)
Log traído de consola (`logs/heroofsparta_001.log`, 43 líneas). Hallazgos:
- Arranque completo: `.so` relocalizado, JNI inicializado, `appInit OK`, loop iniciado.
- `io.c` funciona: el motor abre `texturesSPAW/gui/sprites/fonts/TEXT/a` por
  `fopen` directo (todos OK gracias a la redirección a `hos/`).
- `loadMovie(intro.vdo)` skipeado sin traba; `Spartan.dat`/`Setting.dat`
  inexistentes (normal en primer arranque).
- **Corte limpio tras `fopen(hos/a)`** (384 KB): sin `fclose`, sin líneas `[Java]`,
  sin `[fps]`. El primer `nativeRender()` nunca volvió → cuelgue (no crash: sin
  `.psp2dmp`, pantalla negra con FPS decayendo).
- Estructura de datos confirmada: `ux0_data/heroofsparta/` sube a
  `ux0:data/heroofsparta/` (`hos/` adentro con packs de 384 KB–29 MB + oggs).
- Instrumentación agregada (host): watchdog en `main.c` (contadores
  entered/done; warn cada 5s atascado; crash forzado a los 30s para capturar
  `.psp2dmp` con stacks reales) + marcador `l_error` una-sola-vez en la primera
  llamada JNI→ResLoader. Debug recompilado OK.
- Siguiente paso en consola: instalar este eboot, correr, **esperar ~40s en la
  pantalla negra sin cerrar** (dar tiempo al watchdog), traer `heroofsparta_002.log`
  y el `.psp2dmp` si se generó. Lectura esperada:
  - `[watchdog] sin completar frame` + `primera llamada JNI` ausente = colgado en
    el motor parseando `a` (o deadlock de hilos) → el dump dice la función exacta.
  - `primera llamada JNI` presente + cuelgue = cae en FalsoJNI/stub → revisar
    `jda_alloc` o el dispatch.

## Bug #1, análisis del log 002 (2026-10-10): NO es cuelgue, es carga lenta
- El watchdog funcionó: `entered=2 done=1` al inicio, `entered=28 done=27` al
  final → **26 frames completados**: el motor avanza, con frames de varios
  segundos mientras parsea packs.
- Secuencia: texturesSPAW(10 MB) → gui → sprites(12 MB) → fonts(12 MB) → TEXT →
  a → cameras → l(8 MB) → cdef → **sf abierto ~28 veces seguidas** (115 KB;
  patrón open/close por entrada, superado sin traba) → clev → sprites de nuevo.
- El corte del log 001 en `fopen(a)` era lentitud, no cuelgue.
- Sin marcador JNI todavía: todo el IO hasta ahora es `fopen` directo (el camino
  JNI/ResLoader aún no se ejercita; audio `.ogg` tampoco — llega después).
- Punto actual: relectura de `sprites` (12 MB) con 10s+ sin completar al momento
  de traer el log. Candidatos: descompresión PVR por software
  (`PVRTCDecompress` existe en el .so) o parseo pesado en CPU a 444 MHz.
- Acción: **esperar 3–5 min en pantalla negra sin cerrar**. Si termina → menú.
  Si el watchdog dispara a los 30s → `.psp2dmp` + log 003 con la función exacta
  (dump dirá si es loop de descompresión = lento, o espera infinita = bug).

## Bug #1, análisis del log 003 + lecciones de Asphalt-5 (2026-10-10)
- Log 003 reproduce el patrón: `sprites` (12 MB) tarda ~15-20s entre `fopen` y
  `fclose` (watchdog 5/10/15s intercalados); luego avanza (fonts/TEXT/a/cameras/
  l/cdef/sf×28/clev/sprites). Cada pack cuesta segundos; ** throughput ~0.7 MB/s**.
- Revisado el hermano **Asphalt-5-Vita** (misma época Gameloft): sus cuellos
  fueron (a) relecturas de chunks de 1 MB cientos de veces → caché RAM de 48
  slots, y (b) decode Vorbis bloqueante de segundos → decode en hilo de fondo.
  Aquí el patrón es distinto (una lectura por pack, sin relecturas), así que la
  caché RAM **no** aplica todavía; el decode en fondo queda anotado para cuando
  el log muestre stalls en `loadMusic` (músicas ~1 MB → posible hitch de ~1s).
- Instrumentación agregada (host, Debug recompilado OK): `fread/fseek/ftell`
  del .so ahora pasan por `fread_soloader/fseek_soloader/ftell_soloader`
  (`source/reimpl/io.c`, cableados en `dynlib.c` en ambas ramas): miden cada
  lectura con `sceKernelGetProcessTimeLow` y suben a `l_warn` las >200ms.
  El próximo log dirá si los segundos están en el read (ux0/bridge lento) o en
  el parseo del motor entre reads (p. ej. `PVRTCDecompress` por software).
- Siguiente paso en consola: instalar este eboot, correr, traer
  `heroofsparta_004.log` (rota por `next.idx`).

## Bug #1, análisis del log 004: IO rápido, motor lento (2026-10-10)
- `fread_soloader` con timing discrimina sin lugar a dudas:
  - Bulk a ~10 MB/s (524 KB en 48ms, 2 MB en 193ms). **ux0/bridge sanos.**
  - Primer byte de cada archivo ~11-17ms (cold). Pequeños de µs.
  - Los avisos del watchdog (5/10/15s) caen en huecos SIN `fread`: los segundos
    están en código del motor entre lecturas (parseo/decode de cada chunk:
    p. ej. 262 KB leídos en 24ms y luego segundos de CPU antes del siguiente).
  - Lecturas duplicadas del mismo tamaño seguidas (65588×2, 262196×2,
    524340×2): el motor relee chunks (fseek+re-read, normal en este formato).
- Conclusión: pantalla negra = primer arranque procesando ~40 MB de packs en
  CPU (candidatos: `PVRTCDecompress`/LZMA por software + subidas GL). Progresa
  (log 002: 26 frames completados), solo necesita minutos, no hay deadlock.
- Fix de seguridad (host, Debug OK): watchdog ahora es progress-aware — cuenta
  `g_io_ops` (incrementado en cada `fread`); el crash forzado solo dispara tras
  60s sin frames NI lecturas (antes: 30s sin frames, que podía matar una carga
  legítima lenta). Avisos cada 5s + "progreso reanudado" incluidos.
- Siguiente paso en consola: instalar este eboot, **dejar 5-10 min en pantalla
  negra**, traer `heroofsparta_005.log`. Si `done`/`io` crecen → solo esperar.
  Si watchdog dispara → dump con la función exacta del atasco real.

## Bug #2 (2026-10-10): pantalla negra con el motor vivo (log 005) — resolución HD hardcodeada
- Log 005: carga avanza (texturesSPAW/gui/sprites/fonts/TEXT/a/cameras), sin crash, pero
  la pantalla sigue negra.
- Pseudo-C: el motor está hecho para tablets Honeycomb 1280x752. `GLRenderer::Init`,
  `Game` (estado -3) y el reset de escena hacen `glViewport(0,0,0x500,0x2f0)`;
  `CameraMgr::SetOrthoProjection` hace `glOrthof(0,1280,752,0)`. `s_windowWidth` solo se
  usa para elegir `gui_nexus` (==800) vs `gui`. El port le pasaba 960x544 y no reescalaba.
- Hermanos: Asphalt-5 (GLES1 Gameloft) reescala viewport/scissor del espacio del motor a
  la superficie real y usa `vglInitExtended(…, 12 MB, MSAA_NONE)` (igual GT Racing).
  Este port tenía el default del boilerplate (6 MB, MSAA 4X).
- Fix (host, Debug OK): `glViewport_soloader`/`glScissor_soloader` en `glutil.c`
  (1280x752 → 960x544), `nativeInit/nativeResize` reciben 1280x752, táctil escalado a
  1280x752, vitaGL 12 MB sin MSAA, `glLightx` real (antes `ret0`).
- ABI float descartada: `libvitaGL.a` sin `Tag_ABI_VFP_args` (softfp, igual que el loader).
- Siguiente paso en consola: instalar, correr, traer log 006 + captura. Si sigue negro,
  siguiente sospechoso: soporte `GL_FIXED` en `glVertexPointer` de vitaGL (el motor usa
  16.16 en todo) → instrumentar tipos de los `gl*Pointer`.

## Bug #2, log 006 (2026-10-10): sigue negro → vitaGL vendorizado desde fuente
- Log 006: mismo patrón que 005 (carga sf×28 → clev → sprites, sin warnings ni crash),
  pantalla negra con los fixes de viewport aplicados.
- `lib/vitagl/` vendorizado: copia del árbol de **Asphalt-5-Vita** (hermano GLES1
  Gameloft que sí dibuja; incluye su fix de `GIT_HASH` para árboles sin .git), sin
  `.git`/`samples`/objetos. `Makefile` → `vitagl.mk` (el toolkit no copia archivos
  `Makefile` al dir temporal de build; mismo truco que N.O.V.A-2).
- `CMakeLists.txt`: target `vitaGL_build` + `IMPORTED` lib, flags de A5
  `SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2`,
  stamp que fuerza rebuild al cambiar flags.
- `dynlib.c`: quitadas 5 entradas `egl*` (las daba el vitaGL de vdpm; el .so no
  importa ninguna EGL — `nm -D` = 0).
- Confirmado en la fuente: vitaGL sí soporta `GL_FIXED` en los `gl*Pointer` (`ffp.c`).
- Build Debug OK (eboot 625 KB). Siguiente: probar en consola → log 007 + captura.

## Bug #2, log 007 (2026-10-10): sigue negro con vitaGL vendorizado → instrumentación GL
- Log 007 idéntico a 005/006: corta en la 2.ª lectura de `sprites`, sin watchdog (o sea,
  los frames se completan), sin más llamadas Java (ni `playMusic`). No sabemos si dibuja.
- Instrumentación (host, Debug OK) en `glutil.c`, cableada en `dynlib.c`:
  `glDrawArrays/glDrawElements/glClear` cuentan; `glTexImage2D` loguea las primeras 16
  (formato/tipo/tamaño) + `glGetError` tras cada subida; primeros 8 `glViewport`;
  primer `glClearColorx`; `gl_heartbeat()` cada 3s en `main.c` (frames/draws/clears/
  texups/glGetError). En Debug, `glClearColorx` negro → violeta oscuro.
- Lectura esperada del log 008 + lo que se ve:
  - Violeta en pantalla + draws>0 → el motor dibuja pero no se ve (texturas/estado GL).
  - Violeta + draws=0 → el motor está en un estado sin render (espera de algo).
  - Negro (sin violeta) con clears>0 → la presentación de vitaGL no llega a pantalla.

## Bug #2, log 008 (2026-10-10): violeta (swap OK), 0 draws, logger mudo tras la carga
- Usuario: pantalla violeta (el clear de diagnóstico llega a pantalla → vitaGL presenta
  bien) a ~16 FPS, sin nada encima.
- Log: heartbeats con 0 draws y 1 clear/frame durante la carga (en Android la carga
  corría tapada por `intro.vdo`), 102 texturas subidas. El log deja de escribirse ~1 min
  después del arranque (FTP: mtime 05:01, sin `.psp2dmp` nuevo), en la 2.ª carga de
  `sprites`.
- Los `GL_INVALID_ENUM` tras `glTexImage2D` son falsos: el error es del
  `glDisable(GL_GENERATE_MIPMAP)` que `Texture::Load` hace después de cada subida
  (pseudo-C `out_ghidra.c:29419`).
- `Texture::Load` decodifica TODO PVRTC (formatos 0x18/0x19 → 0x8C00-0x8C03) por software
  con `PVRTCDecompress` → RGBA8 → `glTexImage2D`. Es el CPU-bound de la carga.
- Fix (host, Debug OK): hook `_Z15PVRTCDecompressPKviiiPh` (patch.c) solo anota
  src/2bpp/w/h/out; `glTexImage2D_soloader` reconoce el buffer y sube el bloque comprimido
  con `glCompressedTexImage2D(GL_COMPRESSED_RGBA_PVRTC_{2,4}BPPV1_IMG)`. Watchdog con
  prioridad mayor que main (no lo interrumpía mientras main estaba ocupado en CPU).
- Siguiente: log 009. Ver líneas `[gl] PVRTC nativo … -> 0x0`, si la carga termina
  rápido y si aparecen heartbeats con draws>0.

## Hito (2026-10-10, log 009): ¡llega al menú y al ingame! — Bug #3: ritmo, bajones, carga
- PVRTC nativo confirmado (`[gl] PVRTC nativo … -> 0x0`), carga inicial en segundos.
  Menú a 60 FPS, ingame 40-55 FPS (hasta ~400 draws/frame), táctil funciona.
- Juego acelerado sobre 30 FPS: `Main::Render` suma +1 al contador de lógica por frame
  (solo `animTime` usa reloj real); Android capaba a 45 ms/frame en `onDrawFrame`.
  Fix: `vsync_interval = 2` de vitaGL (`gl_init`) → 30 FPS clavados. (3 = 20 FPS, más
  fiel al original si 30 sigue rápido.)
- Bajones: (a) cada SFX se decodificaba entero del .ogg al pedirlo y se liberaba al
  terminar (31.ogg dos veces; 59.ogg 308k frames en pleno juego), con el mutex del
  mezclador tomado; (b) `m_4.ogg` (2.9M frames) congeló 5s (watchdog); (c) log síncrono de
  cada `fread` en Debug. Fix en `audio.c`: precarga de todos los SFX en hilo de fondo
  (prioridad < main) y PCM cacheado para siempre; decodificación fuera del mutex; música
  en streaming dentro del mezclador (`stb_vorbis_get_samples_short_interleaved`, solo si
  viene a 44100, si no decodifica entera); mezclador con prioridad > main y stack 64 KB
  (stb usa alloca). `io.c`: solo se loguean los fread >200 ms.
- Pantalla de carga: sacado el violeta de diagnóstico (vuelve el negro del motor) y se
  dibuja `splash.png` (la de `SplashActivity`, 800x480, pillarbox) hasta el primer draw
  del motor (`gl_draw_splash`, guarda y restaura el estado GL). Se lee de
  `ux0:data/heroofsparta/splash.png` (copiado a `ux0_data/`, subido por FTP).
  `lib/stb/stb_image.h` vendorizado (de Asphalt-5). El video `intro.vdo` (MP4 29 MB)
  sigue sin reproducirse (pendiente: sceAvPlayer).

## Bug #4 (2026-10-10): controles físicos + HUD táctil al 1% + logo de carga
- CIRCLE pausaba: `appKeyPressed` solo escribe `l_pause` y `appUpdate` trata 4 (BACK)
  y 82 (MENU) como pausa; ninguna otra tecla hace nada en el motor. Se quitó todo el
  mapeo de teclas salvo START→MENU.
- Jugar = toques sintéticos (`main.c`, IDs 5/6/7; el motor asigna 10 slots por ID):
  cruceta / stick izq / stick der → arrastre del joystick desde su centro
  `(gui+0x18+125, gui+0x28+545)` (de `Main::ProcessJoystick`, radio de captura 100,
  arrastre de 60); CROSS → espada (1127,516), CIRCLE → escudo (1016,639), medidos
  sobre la captura `screenshots/be/2026-10-10/2026-10-10-011136.jpg`.
- HUD al 1% (`patch.c`): hooks con `SO_CONTINUE` a `GameGUI::PaintGraphItem` (nivel 3,
  ítems con y>420 y x>880) y `Main::RenderJoystick`, bajando el alfa global
  `GameGUI+0x94` solo durante ese dibujo. Log `[hud] item N en (x,y)` una vez por ítem
  para ajustar la región.
- Carga: `splash.png` eliminado (local y en la Vita); ahora `gi_gameloft_logo.png`
  (267x196) centrado a tamaño nativo sobre negro, desde `res/drawable/`.

## Bug #4b (2026-10-10, log 010): "pegado" en el tutorial de focused kill
- No era cuelgue: el tutorial espera el toque del botón de focused kill, que
  `GameGUI::ShowFatalityButton` muestra como ítems 3/20 (gráfico, en (919,440)) y 3/12
  (zona táctil). El 20 caía en la región al 1% → invisible.
- Fix: la región al 1% excluye 20 (focused kill) y 24/25 (saltar, `ShowJumpButton`).
  TRIANGLE → toque en el centro de la zona 12 leída en vivo (`hud_area_center`, patch.c).
  Log `[hud] zona N: (x,y) WxH tecla K` lista todas las zonas táctiles del HUD (para
  mapear saltar y verificar espada/escudo).

## Bug #5 (2026-10-10, log 011): atascada en el cuadro "green crystal"; HUD visible otra vez
- Log de zonas táctiles nivel 3: 0 (-1000,-1000) tecla 24 (saltar, fuera de pantalla),
  1 (1176,42), 2 (914,435) 132x125 tecla 25 = focused kill, 3 (15,137) = templo,
  4 (955,580) tecla 0 = escudo, 5 (260,147) 753x584 = área central, 6 (1060,430)
  tecla 16 = espada, 7 (10,415) 250x250 = joystick, 68 = área de diálogo.
  El ítem 12 que usaba TRIANGLE NO es zona táctil → TRIANGLE no hacía nada.
- Cuadro de texto: `GameGUI::UpdateInfoBox` cierra según estado (+0x1c8) / tipo (+0x1f0):
  por tap-release en zona 0 del nivel GUI 13 (CheckActions 0x10/0x20, cualquier slot),
  por tiempo, o por una condición del jugador (tipo 5: MC+0x260, tipo 6: estado 0x3a).
- Cambios (pedido del usuario): fuera el HUD al 1% (sin hooks PaintGraphItem/
  RenderJoystick). Pad por zonas reales leídas en vivo (`hud_area_center(gui, nivel,
  zona)`): CROSS = espada (zona 6) o, con cuadro abierto, su ✕ (nivel 13 zona 0);
  CIRCLE = escudo (4); TRIANGLE = focused kill (2). Toques sintéticos con IDs 100+.
  Panel frontal multitáctil real: un puntero por dedo (id & 63), down/move/up como
  `onTouchEvent`. Log `[hud] cuadro de texto: estado E tipo T` + zonas del nivel 13.

## Release v1.0 (2026-10-13): performance, saves verificados, VPK publicable
- **Performance (cambio real en código):** `fread_soloader` (`source/reimpl/io.c`)
  ya no mide tiempo con `sceKernelGetProcessTimeLow` en Release (guard con
  `#ifdef DEBUG_SOLOADER`; el contador `g_io_ops` del watchdog sigue siempre).
  Verificado en el eboot Release: 0 ocurrencias de strings de debug
  (`cargado`, `fopen redirect`, warnings del watchdog) — `l_debug/l_info/l_warn`
  compilan a cero, así que jugar = sin log por frame. Base ya existente: 30 FPS
  fijos (`vsync_interval=2`), PVRTC nativo a GPU, relojes al máximo (444/222/222),
  SFX precargados + música en streaming, vitaGL 12 MB sin MSAA + DRAW_SPEEDHACK=2.
- **Saves verificados ✔:** `io.c` redirige `/data/data/<paquete>/*.dat` → `saves/`
  (creada al arrancar por el logger + por el deploy); `fwrite/fclose` por
  SceLibcBridge. Log 011 confirma lecturas (`Spartan.dat`, `Setting.dat` en `rb`)
  y escrituras OK (`Spartan0.dat` + `Spartan.dat` en `wb`, `fopen` non-null +
  `fclose` 0, sin `fopen(...): (nil)`). Progreso persiste entre sesiones.
- **Release:** `psvita-toolkit build --preset release` OK → `build/heroofsparta.vpk`
  (1.1 MB, `PORT_TRACE=OFF`). Notas de publicación en `RELEASE_v1.0.md`.
- **Issues:** se declara que el **único issue considerado es el de los botones
  físicos (WIP)** — README y release notes actualizados en ese sentido.
