# Mojave Enhanced

*Fallout: New Vegas*, enhanced: the game made stable and smooth, guns that feel weighty and alive, and co-op with
your friends, all put in by one program with one button.

A free, just-for-fun fan mod. **Nothing from any game is in it**: the modding tools it needs come from their authors'
own pages, and everything in this repository is the mod's own work (plus the open-source libraries listed under
[Licence](#licence)).

## Download

Get **MojaveEnhanced-Setup-&lt;version&gt;.exe** from [Releases](../../releases/latest), run it and press
**MAKE IT WORK**. Play with the **Mojave Enhanced** shortcut it puts on your desktop.

- You need Fallout: New Vegas (Steam or GOG) and a free Nexus Mods account (some of the tools are only on Nexus:
  setup opens each page and takes the file as soon as it's downloaded).
- Updates install themselves. They're signed: setup refuses anything that isn't.
- **undo everything** in setup takes it all out again; your saves stay.
- Stuck? Press **report** in setup and send the file it saves.

## What it does

- **Playing together:** host a game or join a friend's from setup with a join code (up to three players). One shared
  world, the host's: you join as your own character and go home with the XP, caps and loot you earned. Chat, trading,
  friendly fire (the host's rule), and downed-not-dead with 20 seconds for a friend to get you up.
- **Gun handling:** weapon inertia and sway, hip and aimed recoil, a camera that kicks and rolls with each shot, and
  contextual aiming at cover.
- **Third person done properly:** lean round cover, swap shoulders (**X**), feet on the ground, free look.
- **Moving:** sprint and tactical sprint (**Shift**), vault, mantle and climb (**Space**), wall kicks and a dropkick.
- **Your feel:** **F8** puts every part of the gun and body feel on sliders.
- **Stable and smooth:** the fixes New Vegas needs, each from its author's own page: xNVSE, JIP LN NVSE, kNVSE,
  lStewieAl's Tweaks, New Vegas Tick Fix and the 4GB Patcher.
- **Your world, your mods:** New Vegas or Tale of Two Wastelands (if you have it installed), each with its own saves;
  your own mods from the game folder, Vortex or Mod Organizer 2. Everyone playing together needs the same mods in the
  same order.

Setup's **FIRST TIME START-UP**, **WHAT'S IN IT** and **KEYBINDS** buttons explain the rest, with every key that
works in your game.

## Building from source

Everything builds on Linux (or WSL) and cross-compiles for Windows:

| Part | What | Needs |
|---|---|---|
| `plugin/` | the xNVSE plugin, `MojaveMostWanted.dll` (32-bit, no C runtime) | clang, lld-link, llvm-dlltool (LLVM 18) |
| `converter/cmd/mmw-launch` | the launcher setup starts the game with | Go 1.24 |
| `converter/cmd/mmw-setup` | setup itself, with the plugin, launcher and `.esp` inside it | Go 1.24 |
| `sheets/` | the data everything is built from (keys, tuning, ...) | Python 3 |
| `tools/` | `gen.py` (code from the sheets), `gen_esp.py` (the `.esp`), `build_setup.sh` | Python 3 |

```sh
sh tools/build_setup.sh          # everything: out/MojaveMostWanted-Setup.exe
sh plugin/build.sh               # the plugin only: plugin/build/MojaveMostWanted.dll
```

Go builds only from `converter/vendor/` (`GOFLAGS=-mod=vendor`, which `build_setup.sh` sets): the `replace` lines in
`go.mod` name folders that aren't needed.

**Checking a release against this code:** the plugin built from this repository is byte-for-byte the one inside setup
1.0.34 except the two bytes of its build time stamp, and the `.esp` is identical. The Go code of the launcher and setup is
token-for-token what 1.0.34 was built from; Go writes line numbers into its programs, so those differ until the next
release, which is built from this repository as it is.

## Licence

[MIT](LICENSE). Third-party code keeps its own licence: the Go libraries in `converter/vendor/` and
`converter/third_party/` (each with its LICENSE file), minimp3 in `plugin/third_party/` (CC0), and the fonts setup is
lettered in (SIL Open Font License 1.1, `converter/cmd/mmw-setup/scene/FONTS-LICENSE.txt`).

Fallout: New Vegas and the other games and tools named here belong to their owners; this mod isn't affiliated with any
of them.

*(Mojave Enhanced was called Mojave Most Wanted before setup 1.0.15. This repository keeps the old name, and
`updates/` keeps the signed update file, so setups already installed keep finding their updates.)*
