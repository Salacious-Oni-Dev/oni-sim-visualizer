# Changelog

Notable changes to this repository, newest first. All SDK repositories share one version
number per release; see [Compatibility](https://github.com/Salacious-Oni-Dev/oni-sdk-docs/blob/main/guides/compatibility.md).

## 0.1.0-alpha.2 (2026-10-12)

*Oxygen Not Included: now with actual reasons for the oxygen to be not included.*

- The window is now Qt 6.8.3 (LGPLv3, linked dynamically), its one third-party library. The
  cells are still drawn from the program's own image, nearest-neighbour; the HUD, hover card,
  event log, legend, scale bar, cell grid and flow arrows are drawn on top at screen
  resolution. The panels are docks, every key is also in the menus, and the start screen and
  the F1 reference are Qt windows. The text is the bundled DejaVu fonts, so pictures come out
  the same on every machine.
- `build/` now holds the Qt DLLs, the MinGW runtime DLLs and `platforms/` beside
  `simviz.exe`; `LICENSES/` holds their licences. `--qt-check` confirms the Qt half loads.
- `build.sh` needs Qt 6.8.3 for MinGW and the POSIX-threads mingw-w64; `build.ps1` uses Qt's
  own MinGW 13.1. Both are in the README.
- Playing runs at the simulation's own speed, one tick per displayed frame, and the HUD's
  timing is the whole frame (tick and render). The hover card's extension lines refresh at
  most four times a second while playing, which took the window from about 8 to 25-30
  frames per second on the reference colony.
- After a seek, the HUD shows the random state the next tick will use, not a transient one.
- "could not read corpus" now says why.
- `test.sh`: the UI checks are now sheets of the Qt window, checked byte-identical across two
  processes. Pinned numbers are recorded against oni-sim-replacement 0.1.0-alpha.2.
- The README's two showcase videos are re-recorded on the Qt window and this release's
  `SimDLL.dll`, and it has three screenshots of the window.

## 0.1.0-alpha.1 (2026-10-01)

First public release.

- `simviz.exe`, a standalone viewer for the simulation's state. It is never loaded into the
  game.
- Two sources with the same views and probes: a corpus recorded from your own game
  (`--corpus`), replayed on a `SimDLL.dll` the program loads itself, with stepping backwards
  and forwards; or a running game (`--attach`), read over a socket from the framework's debug
  server, forwards only. Mod 1 starts that server when the game is launched with
  `--oni-debug-inspector`; it has no authentication, so attach only on a machine and network
  you trust.
- Views for element, temperature, mass, phase, the game's flow, liquid and sunlight textures,
  the game's own per-cell validity check, and any per-cell extension property or per-element
  extension attribute, by name.
- A window with a scrub bar, single-tick stepping in both directions, and a start screen that
  asks for a corpus or a `host:port`. Control panels run every readout and experiment, and
  show the command line that reproduces what is on screen.
- `--snapshot` writes a frame to PNG with no window, and `--probe` reads one cell.
- `test.sh`, a check suite, with `tools/attach-stub.py` serving the attach path so it can be
  tested with no game running.
- Builds with mingw-w64 against a built `oni-sim-replacement`, with no third-party libraries.
- For large screens: `--font large`, a 7x13 font that the start screen picks by itself on a
  screen 1440 px tall or more, `--ui-scale` for very high-DPI screens, and `--fullscreen`.
- `build.ps1` builds the program on Windows in PowerShell, without WSL or bash.
