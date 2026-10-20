# Mojave Enhanced: setup help (give this file to Claude)

**For the human reading this:** open a chat with Claude (the free version is fine), drag this file
into it and say *"help me set up Mojave Enhanced"*. Claude will walk you through it and can read
anything you paste back (the setup's details, an error, a screenshot). Claude can't click or install
things on your PC from a chat, so you do the clicks and it tells you which ones.

---

## For Claude: what this is and how to help

You're helping a friend of the mod's author install **Mojave Enhanced** (called *Mojave Most Wanted*
before setup 1.0.15), a private, just-for-fun fan mod for *Fallout: New Vegas* on PC: the game made
stable (the usual engine fixes, set up automatically), weighty gun handling (inertia, sway, recoil,
aiming round cover), a proper third-person camera with shoulder swapping, shots that light up the
world, a sprint and parkour (vault, mantle, climb, wall kicks), a menu to fine-tune all of it, and co-op for up to
three players. It comes as one program,
**MojaveEnhanced-Setup-<version>.exe**, with a big **MAKE IT WORK** button. Your job is to guide them
through it step by step, read what they paste and explain what to do next.

How to help well:

- Go one step at a time. Ask what they see on screen before moving on.
- When something fails, ask them to click **report** (top right of the setup window). It saves
  *Mojave Enhanced report.txt* on their desktop and opens it: ask them to paste all of it. Or
  **details** (Ctrl+A, Ctrl+C in the text). Read it before guessing. The messages in the
  troubleshooting table below are the setup's own words.
- Only use what's in this file and what they paste. If you don't know, say so; don't invent file
  names, settings or buttons.
- Don't suggest downloading modding tools from anywhere except their official pages (listed below),
  turning antivirus off entirely, or getting the game any way other than buying it. Nothing from any
  game is included in this mod.
- The player can't break their game with this: **undo everything** puts the game folder back as it
  was (see *Taking it out*). Saves are never touched.

---

## What you need

- Windows 10 or 11 (64-bit) and an internet connection.
- **Fallout: New Vegas** (Steam or GOG; Epic should work too), started once from its own launcher
  so its settings files exist.
- A free **Nexus Mods** account (<https://www.nexusmods.com>), for four small modding tools that are
  only published there.

## Setting it up

1. **Close Fallout: New Vegas.**
2. **Run `MojaveEnhanced-Setup-<version>.exe`.**
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
   3. *Put in Mojave Enhanced*: the mod, the load order, a setting so the game uses the mod's loose
      files, and a **Mojave Enhanced** shortcut on the desktop.
   4. *Set up vanilla stable (no cars)*: the game's settings for the mod.
   5. *Ready to play*.
4. When the button turns green and says **ALL SET!**, you're done. The button then says **PLAY**.

## Playing

Always start the game with the **Mojave Enhanced** shortcut on the desktop (or PLAY in setup). It
checks for an update of the mod and starts New Vegas through xNVSE. Starting the
game from Steam directly works only if the game was patched with the 4GB Patcher.

Keys in the game:

| Key | What it does |
| --- | --- |
| **Shift** | Hold to sprint; double-tap for a tactical sprint (it runs out and comes back) |
| **Space** | At a ledge: vault, mantle or climb; in the air next to a wall: kick off it |
| **Right-click** | Sprinting, jumping, weapon away: a flying dropkick |
| **G** | Next to one of the game's pickup trucks: drive it (it's yours from then on: G again to get in) |
| **F8** | The weapon feel menu: Up/Down pick a line, Left/Right slide it (hold to go faster, Shift for fine steps), Backspace back to 100%. Saved by itself. |
| **U** | Gun handling on or off (inertia, sway, recoil, contextual aiming) |
| **O** | Smooth aiming on or off |
| **X** | Third person: the camera to the other shoulder |
| **Enter** | Chat, when playing together (`/name Something` changes the name friends see; `/join`, `/leave` and `/ff on` / `/ff off` below) |
| **E** | On a downed friend: get them up |
| **1** | Downed yourself: use a stimpak to get up |
| **F6 / F7** | Hosting: let in / refuse the friend who's asking to join (the game says who) |
| **Ctrl+Alt+End** | Panic key: frees the mouse, ends playing together and closes a frozen game |
| **F10** | A solo multiplayer test: a copy of you from 2 seconds ago |

Aiming near cover leans, rises or dips you to see past it by itself. While playing together, the
game's quick load (F9) is switched off.

## Playing together

Under the checklist in setup:

- **NAME: ...** is the name your friends see. Click it, type, press Enter (Esc keeps the old one).
- **HOST A GAME** opens your game to friends and copies a join code: paste it to them in a chat,
  then press PLAY. Each time you host there's a new code, and it stops working after 12 hours. When a
  friend asks to join, the game says so: **F6** lets them in, **F7** refuses.
- **JOIN A FRIEND**: first copy the join code your friend sent (it starts with `MMW2-`), then press
  JOIN A FRIEND, then PLAY, and wait for the host to let you in.
- **STOP HOSTING** / **LEAVE** play on your own again (also in a running game).

In the game, while playing together (being tested in 1.0.22):

- **/join** in chat takes you into the host's world as your own character; **/leave** takes you back to your
  own game with the XP, caps, items, karma, reputation and places you earned there. If the host leaves, you
  get 60 seconds, then you go home with what you earned. While you're in a friend's world, autosaves and
  quicksave (F5) are off so none of your own save slots get the session. Joining needs the same mods: the game
  says which differ.
- It works from the very start: a guest joining a host still in Doc Mitchell's house stands beside them and
  makes their own character (face, name, SPECIAL, traits, tag skills).
- One shared world: the people around you are the host's, deaths near either of you happen for both, and doors
  open, close and unlock for everyone.
- Friendly fire is on by default; the host types **/ff off** (or **/ff on**) to change it.
- A guest of a lower level than the host gets the host's health and damage (their guns stay as they are);
  everything the guest gains takes the same off the boost, and none of it comes home. A host is never raised.
- What would kill you puts you down instead: a friend has 20 seconds to get you up (**E**), or you use a
  stimpak (**1**). Nobody in time and you die: 20 caps lost (none if you have none), and you're back on your
  feet next to a friend or at the last safe place you stood. The game is never loaded again, so nothing since
  your last save is lost.

Everyone needs the same version: when someone joins on an older one, the game says so (open setup:
it updates itself). Up to three players. Windows may ask once whether New Vegas may use the network:
allow it.

## Taking it out

Run the setup again (the desktop shortcut works) and click **undo everything** (top right; it only
shows once setup has installed something). With the game closed, it removes the mod, every modding
tool setup added, puts back any file it replaced (the load order, Fallout.ini) and removes the folders
it made. The Microsoft Visual C++ runtime stays, because other games use it.

---

## Troubleshooting

The line at the bottom of the setup window (after the `>`) says what happened. **details** shows the
full log, **report** saves everything needed to find a problem in one file.

| The setup says | What to do |
| --- | --- |
| *Couldn't find Fallout: New Vegas* | Put `MojaveEnhanced-Setup-<version>.exe` inside the Fallout New Vegas folder (where `FalloutNV.exe` is) and run it from there. In Steam: right-click Fallout: New Vegas, **Manage**, **Browse local files**. |
| *Close Fallout: New Vegas first* | The game (or the launcher) is still running. Close it completely, check the taskbar, press the button again. |
| *The game's folder needs permission* | Click **Yes** when Windows asks; setup restarts with permission and carries on by itself. |
| *Couldn't reach GitHub for xNVSE* | Check the internet connection (a VPN or strict firewall can block GitHub) and press the button again. It skips what's done. |
| An orange **GET ...** button for a Nexus tool | Download that tool's main file from the page that opened (see step 2). Setup is watching the Downloads folder. If the browser saves downloads somewhere else, move the file into Downloads. |
| *... didn't have ...dll - is it the right file?* | The file downloaded isn't the tool's main file (maybe an optional file). Download the one under *Main files*. |
| *No Fallout.ini yet* | Start Fallout: New Vegas once from Steam (to the main menu), quit, run setup again. |
| *Some files are missing* | An antivirus probably removed something: allow the game folder, press the button again. |
| *Something went wrong* | Paste the report. |

Problems in the game:

| What happens | What to do |
| --- | --- |
| The game starts without the mod (F8 does nothing, no gun handling) | It was started without xNVSE: use the desktop shortcut. The report's *xNVSE's log* shows whether it loaded. |
| The game crashes on start | Paste the report: its *Crashes* part names the module that crashed. |
| A friend can't join | Both on the same version (open setup on both PCs); the host pressed HOST A GAME before PLAY and the friend copied the newest join code. The report's mod log shows what the link saw. |

Where the logs are (inside the Fallout New Vegas folder, unless said otherwise):

- Setup's log: **details** in the setup window (also `%LOCALAPPDATA%\MojaveMostWanted\setup.log`)
- The mod in the game: `Data\NVSE\Plugins\MojaveMostWanted.log`
- The launcher (starting the game): `MojaveMostWanted\mmw-launch.log`
- xNVSE: `nvse.log`

---

## Reference

What setup changes (the mod's own files keep the name *MojaveMostWanted*, so saves and updates from
before the new name keep working):

- `Data\NVSE\Plugins\MojaveMostWanted.dll` and `Data\NVSE\Plugins\MojaveMostWanted\` (the mod, its
  sounds and settings), `Data\MojaveMostWanted.esp`
- `MojaveMostWanted\` (the launcher and a copy of setup)
- xNVSE's files in the game folder, the four tools' files in `Data\`
- `%LOCALAPPDATA%\FalloutNV\plugins.txt` (the load order) and `Documents\My Games\FalloutNV\Fallout.ini`
  (`bInvalidateOlderFiles=1`; while playing together, `bAlwaysActive=1` so the game keeps running
  behind other windows - your own value comes back when you play alone)
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

Setup also accepts `--game "<Fallout New Vegas folder>"` on its command line.
