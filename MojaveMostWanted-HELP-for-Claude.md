# Mojave Most Wanted: setup help (give this file to Claude)

**For the human reading this:** open a chat with Claude (the free version is fine), drag this file
into it and say *"help me set up Mojave Most Wanted"*. Claude will walk you through it and can read
anything you paste back (the setup's details, an error, a screenshot). Claude can't click or install
things on your PC from a chat, so you do the clicks and it tells you which ones.

---

## For Claude: what this is and how to help

You're helping a friend of the mod's author install **Mojave Most Wanted**, a private, just-for-fun
fan mod for *Fallout: New Vegas* on PC: real cars from *Need for Speed Most Wanted (2012)* to pick
from a garage by the Goodsprings gas station and drive around the Mojave, with the NFS soundtracks
on the radio, snow storms, dark nights, Bodycam-style gun handling, parkour (vault, mantle, climb,
wall kicks) and a two-tier sprint. It comes as one program, **MojaveMostWanted-Setup.exe**, with a
big **MAKE IT WORK** button. Your job is to guide them through it step by step, read what they paste
and explain what to do next.

How to help well:

- Go one step at a time. Ask what they see on screen before moving on.
- When something fails, ask them to click **report** (top right of the setup window). It saves
  *Mojave Most Wanted report.txt* on their desktop and opens it: ask them to paste all of it. Or
  **details** (Ctrl+A, Ctrl+C in the text). Read it before guessing. The messages in the
  troubleshooting table below are the setup's own words.
- Only use what's in this file and what they paste. If you don't know, say so; don't invent file
  names, settings or buttons.
- Don't suggest downloading modding tools from anywhere except their official pages (listed below),
  turning antivirus off entirely, or getting the games any way other than buying them. Nothing from
  any game is included in this mod: the setup reads the player's own copies.
- The player can't break their game with this: **undo everything** puts the game folder back as it
  was (see *Taking it out*). Saves are never touched.

---

## What you need

- Windows 10 or 11 (64-bit) and an internet connection.
- **Fallout: New Vegas** (Steam or GOG; Epic should work too), started once from its own launcher
  so its settings files exist.
- **Need for Speed Most Wanted (2012)** (Steam or the EA app), fully installed. The cars are built
  from it. It's about 20 GB.
- A free **Nexus Mods** account (<https://www.nexusmods.com>), for four small modding tools that are
  only published there.

## Setting it up

1. **Close Fallout: New Vegas.**
2. **Run `MojaveMostWanted-Setup.exe`.**
   - If Windows says *"Windows protected your PC"*, click **More info**, then **Run anyway**. That
     warning appears for any program that isn't signed by a company; this one is a hobby project.
   - If your antivirus removes or blocks it, restore it from quarantine and allow that one file. The
     setup downloads modding tools and writes into the game folder, which some antivirus programs
     treat as suspicious.
3. **Press MAKE IT WORK** (or press Enter). It ticks off five steps:
   1. *Find Fallout: New Vegas*: it looks in Steam, GOG and Epic, and next to itself.
   2. *Get the modding tools*: it downloads **xNVSE** from its GitHub page by itself (and the
      Microsoft Visual C++ runtime if the PC doesn't have it; Windows may ask for permission). Then
      the four Nexus tools: **JIP LN NVSE**, **kNVSE**, **lStewieAl's Tweaks** and **New Vegas Tick
      Fix**. For each one, setup opens its Nexus page and the button turns orange (**GET ...**). On
      the page, under *Main files*, click **Manual download** (log in if asked), then **Slow
      download**. As soon as the file lands in Downloads, setup puts it in by itself and opens the
      next one. Don't unpack anything yourself.
   3. *Put in Mojave Most Wanted*: the mod, the load order, a setting so the game uses the mod's
      loose files, and a **Mojave Most Wanted** shortcut on the desktop.
   4. *Build the cars from Need for Speed*: it reads your Need for Speed Most Wanted and builds every
      car, its engine sound and the radio. The first time takes a few minutes.
   5. *Ready to drive*.
4. **If it stops at step 4 with an orange "!"** and a **GET NFS MOST WANTED** button, Need for Speed
   Most Wanted isn't installed (or setup can't find it). Install it, let it finish downloading, and
   setup carries on by itself (or press the button again).
5. When the button turns green and says **ALL SET!**, you're done. The button then says **PLAY**.

## Playing

Always start the game with the **Mojave Most Wanted** shortcut on the desktop (or PLAY in setup).
It checks for an update of the mod, builds anything new, and starts New Vegas through xNVSE. Starting
the game from Steam directly works only if the game was patched with the 4GB Patcher.

In the game, the garage is by the **Goodsprings gas station**. Controls (on foot): **Space** near a
ledge vaults, mantles or climbs, Space in the air next to a wall kicks off it; hold **Shift** to
sprint, double-tap Shift for a tac sprint; **U** turns the gun handling on/off. In a car: W/A/S/D
drive, **G** gets in and out.

## Taking it out

Run the setup again (the desktop shortcut works) and click **undo everything** (top right; it only
shows once setup has installed something). With the game closed, it removes the mod, the cars it
built, every modding tool setup added, puts back any file it replaced (the load order, Fallout.ini)
and removes the folders it made. The Microsoft Visual C++ runtime stays, because other games use it.

---

## Troubleshooting

The line at the bottom of the setup window (after the `>`) says what happened. **details** shows the
full log, **report** saves everything needed to find a problem in one file.

| The setup says | What to do |
| --- | --- |
| *Couldn't find Fallout: New Vegas* | Put `MojaveMostWanted-Setup.exe` inside the Fallout New Vegas folder (where `FalloutNV.exe` is) and run it from there. In Steam: right-click Fallout: New Vegas, **Manage**, **Browse local files**. |
| *Close Fallout: New Vegas first* | The game (or the launcher) is still running. Close it completely, check the taskbar, press the button again. |
| *The game's folder needs permission* | Click **Yes** when Windows asks; setup restarts with permission and carries on by itself. |
| *Couldn't reach GitHub for xNVSE* | Check the internet connection (a VPN or strict firewall can block GitHub) and press the button again. It skips what's done. |
| An orange **GET ...** button for a Nexus tool | Download that tool's main file from the page that opened (see step 2). Setup is watching the Downloads folder. If the browser saves downloads somewhere else, move the file into Downloads. |
| *... didn't have ...dll - is it the right file?* | The file downloaded isn't the tool's main file (maybe an optional file). Download the one under *Main files*. |
| *Almost there! Get Need for Speed Most Wanted* | Install it (or let it finish). If it's installed somewhere unusual, start setup with `--mw "<its folder>"`. |
| *has no VEHICLES folder* | Need for Speed Most Wanted is still downloading or damaged: let it finish (or verify it in Steam), press the button again. |
| *No Fallout.ini yet* | Start Fallout: New Vegas once from Steam (to the main menu), quit, run setup again. |
| *Building the cars failed* | Paste the report. |
| *Some files are missing* | An antivirus probably removed something: allow the game folder, press the button again. |
| *Something went wrong* | Paste the report. |

Problems in the game:

| What happens | What to do |
| --- | --- |
| The game starts without the mod (no garage, nothing new) | It was started without xNVSE: use the desktop shortcut. The report's *xNVSE's log* shows whether it loaded. |
| The game crashes on start | Paste the report: its *Crashes* part names the module that crashed. |
| No cars in the garage | The report's *launcher's log* says how many cars were built. |

Where the logs are (inside the Fallout New Vegas folder, unless said otherwise):

- Setup's log: **details** in the setup window (also `%LOCALAPPDATA%\MojaveMostWanted\setup.log`)
- The mod in the game: `Data\NVSE\Plugins\MojaveMostWanted.log`
- The launcher (building cars, starting the game): `MojaveMostWanted\mmw-launch.log`
- xNVSE: `nvse.log`

---

## Reference

What setup changes:

- `Data\NVSE\Plugins\MojaveMostWanted.dll` and `Data\NVSE\Plugins\MojaveMostWanted\` (the mod, its
  sounds and settings), `Data\MojaveMostWanted.esp`
- `Data\meshes\mmw\`, `Data\textures\mmw\` (the cars built from Need for Speed)
- `MojaveMostWanted\` (the launcher, a copy of setup, the radio)
- xNVSE's files in the game folder, the four tools' files in `Data\`
- `%LOCALAPPDATA%\FalloutNV\plugins.txt` (the load order) and `Documents\My Games\FalloutNV\Fallout.ini`
  (`bInvalidateOlderFiles=1`)
- `MojaveMostWanted-undo\`: what **undo everything** needs (the list of what was added, and copies
  of anything replaced). Leave it alone.

It's fine alongside other mods: anything setup replaces is copied aside first and put back on undo.
With Mod Organizer 2 it also switches the mod on in your MO2 profile; with Vortex, keep
`MojaveMostWanted.esp` enabled.

The modding tools (official pages):

- xNVSE: <https://github.com/xNVSE/NVSE/releases>
- JIP LN NVSE: <https://www.nexusmods.com/newvegas/mods/58277>
- kNVSE: <https://www.nexusmods.com/newvegas/mods/71336>
- lStewieAl's Tweaks: <https://www.nexusmods.com/newvegas/mods/66347>
- New Vegas Tick Fix: <https://www.nexusmods.com/newvegas/mods/66537>

Setup also accepts `--game "<Fallout New Vegas folder>"` and `--mw "<Need for Speed Most Wanted
folder>"` on its command line.
