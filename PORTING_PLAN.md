# Plan de Port — Hero of Sparta (PS Vita)

> Generado por psvita-port-toolkit el 2026-10-08. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** Hero of Sparta
- **Paquete Java:** com.gameloft.android.GAND.GloftSPAW.ML
- **APK original:** `Hero Of Sparta HD Free_1.1.4.apk`
- **TITLEID asignado:** `PSVHOS001`

**¿Motor conocido?** Revisar si algún port hermano (bajo la misma BASE_DIR) comparte motor antes de
reusar su código -- confirmar con símbolos JNI reales, no por analogía superficial.

## 1. Detección automática

- **ABI(s):** armeabi
- **ABI elegida:** armeabi
- **Nota de arquitectura:** Solo armeabi (ARMv6, soft-float) -- Vita lo ejecuta igual (ARMv7 es superset), sin NEON de v7a.
- **Versión de GLES:** AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx))

## 2. .so encontrados (ABI armeabi)

- `heroofsparta_extract/lib/armeabi/libStormGLOFT.so` (807 KB)
- `heroofsparta_extract/lib/armeabi/libheroofsparta.so` (1106 KB)


## 3. Exports JNI (convención `Java_*`)

- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeGetTotalMusics`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeGetTotalSounds`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeInit`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeSetStopOnMusic`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeSetStopOnSound`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GLResLoader_nativeInit`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameGLSurfaceView_nativePause`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameGLSurfaceView_nativeResume`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeDone`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeGetJNIEnv`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeInit`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeRender`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeResize`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeCanInterrupt`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeInit`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeOnTouch`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeSetOnKeyDown`
- `Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeSetOnKeyUp`


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (2026-10-10, con objdump/nm + Ghidra + jadx).
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve, primer build.
- [x] Tabla JNI (FalsoJNI): registrar exports + callbacks hacia "Java".
- [x] Gráficos: wrappers GLES1 en dynlib + ciclo nativo cableado (verificación en consola).
- [x] Input, Audio, Assets, LiveArea/VPK: loop táctil/botones, puente audio real,
      ResLoader multi-raíz, LiveArea validado 5/5, VPK Debug+Release OK.
- [ ] Primer arranque en consola real (un bug a la vez, skill `so-crash-triage`).
- [ ] Pruebas en hardware real hasta gameplay completo.

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## 6. Estándar de logs en consola

El log del juego vive en `<DATA_PATH>logs/<slug>_NNN.log` (`<slug>` = heroofsparta), incremental de
001 a 999 con `next.idx` (nunca `log_<timestamp>.txt`, nunca base 000, nunca ruta hardcodeada:
usar `DATA_PATH`). La subida FTP crea `logs/` y `saves/` en la consola aunque estén vacíos.
Comandos del toolkit (sin IA, deterministas): `psvita-toolkit log-standard --fix-dirs` (audita
y crea los dirs), `psvita-toolkit log-trace --ensure-flag` (inyecta trazas `[TRACE] >> f()` por
función bajo `#ifdef PORT_TRACE`; compilar con `-DPORT_TRACE=ON` para debug, `OFF` para
producción, o `psvita-toolkit log-trace --remove` para quitarlas del árbol).

## Auto-detected lifecycle methods (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you).

- `com.gameloft.android.GAND.GloftSPAW.ML.GLMediaPlayer.nativeInit(int)`
- `com.gameloft.android.GAND.GloftSPAW.ML.GLResLoader.nativeInit(int)`
- `com.gameloft.android.GAND.GloftSPAW.ML.GLUtils.Device.nativeInit(void)`
- `com.gameloft.android.GAND.GloftSPAW.ML.GameRenderer.nativeInit(int, int, int)`
- `com.gameloft.android.GAND.GloftSPAW.ML.GameRenderer.nativeRender(void)`
- `com.gameloft.android.GAND.GloftSPAW.ML.GameRenderer.nativeResize(int, int)`
- `com.gameloft.android.GAND.GloftSPAW.ML.HeroOfSparta.nativeInit(int)`

## 7. Motor confirmado (2026-10-10, ver port_progress.md Fase 3)

- **Familia:** Gameloft era 2012, mismo motor que **Dungeon-Hunter-2-vita**
  (hermano confirmado por exports: GLMediaPlayer/GLResLoader/Device/
  GameRenderer/Hero-natives + libStormGLOFT.so acompañante).
- **Solo se carga `libheroofsparta.so`** (2503 exports, los 18 `Java_*`,
  sin `JNI_OnLoad`). `libStormGLOFT.so` es hook anti-tamper sin exports
  JNI (su `JNI_OnLoad` hace dlopen+hook inline de glTexImage2D/glViewport/
  glScissor/glGetError) y no se carga, igual que en DH2.
- **GLES1** confirmado (NEEDED libGLESv1_CM + `gl*X` fixed-point en undefined).
- **Símbolos:** 121 undefined, 100% cubiertos en `source/dynlib.c`
  (se agregaron 17: `__aeabi_f2d/f2iz/fadd/fcmpeq/.../i2f/lmul/ui2f`,
  `__aeabi_d2f/d2iz`, `__dso_handle`, `__assert2`).
- **Ciclo de vida** (`main.c`, réplica de `GameRenderer.onSurfaceCreated`):
  `nativeGetJNIEnv` → `GLMediaPlayer.nativeInit` → `GLResLoader.nativeInit`
  → `HeroOfSparta.nativeInit(0)` → `GameRenderer.nativeInit(1,960,544)` →
  `nativeResize` → loop `nativeRender` + `audio_update_sound()`.
- **Assets:** todo lo no-`.sav`/`.dat` pasa por JNI → `GLResLoader`
  (`getResourceFull/Bytes/Length(String)`); implementado con búsqueda
  multi-raíz (res/drawable*, assets/, hos/, ruta directa) + log de misses.
- **Audio:** puente `GLMediaPlayer` real (96 sonidos + 16 músicas Ogg Vorbis
  de `hos/`, decodificados con stb_vorbis, mezclador SceAudioOut 44100 stereo).
  Video (`intro.vdo`) = skip (`IsVedioFinished` → 1, sin AvPlayer en v1).
- **Rutas:** `io.c` traduce `/mnt/sdcard/gameloft/games/hos/` → `hos/` y
  `/data/data/<paquete>/` → `saves/`. Datos: `ux0_data/heroofsparta/`
  (`hos/` = pack 122 MB ya reubicado adentro, `res/`, `assets/`,
  `libheroofsparta.so` como `SO_PATH`).
- **Riesgos abiertos (solo consola los resuelve):** qué archivo pide
  `appInit` primero (pack presente pero sin verificar contra el motor),
  mapeo real de `appKeyPressed/Released`, rendimiento GLES1, `nativeResize`
  (Ghidra muestra `glViewport(0,0)` sin args: posible artefacto).
