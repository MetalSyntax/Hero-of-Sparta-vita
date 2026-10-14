# Hero of Sparta — PS Vita · Release v1.0

Native port of **Hero of Sparta HD Free v1.1.4** (Gameloft, Android) to PS Vita / PS TV.
Title ID: `PSVHOS001` · Version: `01.00` · Build: **Release** (`PORT_TRACE=OFF`, no debug log).

## Download

- `heroofsparta.vpk` (in `build/heroofsparta.vpk`, ~1.1 MB) — contains the Release eboot + LiveArea.
- Game data is **not** included (see Installation).

## Installation

1. Console requirements: CFW (HENkaku / h-encore / Enso, 3.60–3.74), **kubridge** +
   **FdFix** plugins in `ur0:tai/config.txt` (`*KERNEL`), and **libshacccg.suprx**
   in `ur0:data/`.
2. Install `heroofsparta.vpk` with VitaShell.
3. Copy into `ux0:data/heroofsparta/` from your legally obtained APK + Android data:
   - `lib/armeabi/libheroofsparta.so` → `ux0:data/heroofsparta/libheroofsparta.so`
   - `assets/data.save` → `ux0:data/heroofsparta/assets/data.save`
   - `res/*` → `ux0:data/heroofsparta/res/`
   - Android data folder `hos/` → `ux0:data/heroofsparta/hos/`
4. Launch **Hero of Sparta** from the LiveArea. The first load takes a few minutes
   (texture decompression); later loads are faster.

Final layout in `ux0:data/heroofsparta/`:
`libheroofsparta.so`, `hos/`, `res/`, `assets/data.save`, `saves/` (auto-created),
`logs/` (Debug builds only).

## Performance (v1.0)

- Game locked to **30 FPS** (the engine advances its logic per frame; uncapped it
  would run fast-forward). Menu at 60, ingame stable 30; very busy scenes
  (~500 draws/frame) can dip to ~22 FPS.
- PVRTC textures uploaded **natively to the GPU** (no software decoding).
- Vita clocks at max from boot (ARM 444 / BUS 222 / GPU 222).
- Audio (96 SFX + 16 music tracks) preloaded/decoded off the main thread;
  music streams.
- The Release build produces no per-frame logging: verified that the eboot
  contains zero debug strings (`l_debug`/`l_info`/`l_warn` compile to nothing).
  Play with the Release VPK; the Debug build is for diagnostics only.

## Saves — verified ✔

- Android paths are remapped: `/data/data/<package>/*.dat` → `saves/` inside
  `ux0:data/heroofsparta/`. The `saves/` folder is created automatically at boot.
- Confirmed with a real console log (`logs/heroofsparta_011.log`):
  reads of `Spartan.dat`/`Setting.dat` at boot and **correct writes**
  (`Spartan0.dat`, `Spartan.dat` opened `wb`, `fopen` OK + `fclose` OK, no errors).
  Progress persists across sessions.
- If a save ever failed, the Debug log would show it as `fopen(...): (nil)`;
  no such case to date.

## Controls

| Vita input | Action |
|---|---|
| Left / right stick / D-Pad | Drag the virtual joystick |
| Cross | Sword (dismisses the dialog box when one is open) |
| Circle | Shield |
| Triangle | Focused kill |
| Start | Android MENU (pause) |
| START + SELECT | Quit to LiveArea |
| Front touch | Original 1:1 controls, multitouch |

## 🛠️ Known issue (only one tracked)

- **Physical buttons**: currently the **only tracked issue, work in progress**
  (the pad injects touches onto the HUD touch zones read live from the engine;
  if a button ever seems to miss, the Debug log shows exactly where the
  synthetic tap landed). No other open issues for v1.0: the intro video
  (`intro.vdo`) is skipped by design and the HUD is drawn at full opacity
  on purpose.
