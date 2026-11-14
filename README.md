# TooManyNotificationsRemade

SKSE plugin for Skyrim. When the HUD notification list is full (`Messages.MAX_SHOWN` in `hudmenu.swf`), the oldest notification fades out right away so new ones show without delay. The patch is applied at runtime, so `hudmenu.swf` is not overridden.

## Build with ClibDT

1. Run `ClibDT.exe setup --root <your dev folder>` once, if you haven't already.
2. Import this project:
   ```
   ClibDT.exe import <path to this folder>
   ```
3. Compile it:
   ```
   ClibDT.exe compile TooManyNotificationRemade
   ```
   Use `--target se`, `ae` or `vr` to build for one game version only, and `--debug` for a debug build.

You can also do steps 2 and 3 from the ClibDT window (Compile page).

The built `TooManyNotificationRemade.dll` goes in `Data/SKSE/Plugins`.
