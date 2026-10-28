// Package help holds what setup's help pills open: FIRST TIME START-UP, WHAT'S IN IT and KEYBINDS.
package help

import (
	"strconv"

	"mmw/cmd/mmw-setup/scene"
)

// The left column's pills (their hit names: scene.HitName)
const (
	FirstTime = "FIRST TIME\nSTART-UP"
	WhatsIn   = "WHAT'S IN IT"
	KeysPill  = "KEYBINDS"
)

// Game is what's on in this game.
type Game struct {
	Full    bool
	Weather bool
}

func text(s string) scene.Line { return scene.Line{Kind: scene.LText, Text: s} }
func head(s string) scene.Line { return scene.Line{Kind: scene.LHead, Text: s} }
func key(k, s string) scene.Line {
	return scene.Line{Kind: scene.LKey, Key: k, Text: s}
}

// Mismatch is the warning every panel about playing together carries.
const Mismatch = "Everyone playing together needs the same mods, in the same order. If yours don't match your friends', playing together breaks: the host's game refuses /join, and a world made with other mods can crash."

// FirstTimeLines: how to get playing together, from the very start
func FirstTimeLines() []scene.Line {
	steps := []string{
		"Press MAKE IT WORK. Setup finds Fallout: New Vegas, gets the free script extenders from their authors' own pages (Nexus Mods needs your click: setup opens each page and takes the file as it lands) and puts Mojave Enhanced in.",
		"Press PLAY once on your own and load a save, so the game is set up. Then quit to the desktop.",
		"Everyone picks the same WORLD (FNV or TTW) and the same mods (YOUR MODS).",
		"The host presses HOST A GAME: a join code is copied. Paste it to your friends (Discord, a text).",
		"Each friend copies the code and presses JOIN A FRIEND (setup reads it from the clipboard).",
		"Everyone presses PLAY. The host presses F6 to let each friend in (F7 turns them away).",
		"When the mods match, friends arrive in the host's world as their own characters (or type /join in chat). /leave goes home with the XP and loot earned there.",
	}
	l := []scene.Line{head("Once")}
	for i, s := range steps {
		if i == 2 {
			l = append(l, head("Playing together"))
		}
		l = append(l, scene.Line{Kind: scene.LStep, Key: strconv.Itoa(i + 1), Text: s})
	}
	return append(l, scene.Line{Kind: scene.LWarn, Text: Mismatch},
		text("Stuck? Press report (top right) and send the file it saves. In game, Ctrl+Alt+End frees the mouse and ends playing together."))
}

// WhatsInLines: what Mojave Enhanced adds
func WhatsInLines(g Game) []scene.Line {
	l := []scene.Line{
		head("Playing together"),
		text("Host or join from here with a join code: good for 12 hours, and the host lets each friend in. Up to three players."),
		text("One shared world, the host's: join as your own character, and /leave takes home your XP, caps and loot (quest items stay in the host's world)."),
		text("Friends look as they really do, in the armour they wear, with their own gun handling."),
		text("Chat (Enter), and conversations within 10 m heard by friends near you."),
		text("Downed, not dead: 20 s for a friend to get you up (E), or a stimpak (1)."),
		text("Trade: look at a friend and press E. Friendly fire: /ff on or /ff off (the host decides)."),
		head("Guns and moving"),
		text("Weapon inertia and sway, hip and aimed recoil, a camera that kicks with each shot, contextual aiming at cover."),
		text("Third person done properly: lean round cover, swap shoulders (X), feet on the ground, free look."),
		text("Sprint and tactical sprint, vault, mantle, climb, wall kicks and a dropkick. F8 puts every part of the feel on sliders."),
	}
	if g.Full {
		l = append(l, head("Driving"), text("Drive the wasteland's own pickups (G), with a radio, your own music, headlights and wipers."))
	}
	if g.Weather {
		l = append(l, head("Weather"), text("Seasons with snow and storms that follow the game's calendar."))
	}
	return append(l,
		head("Stable and smooth"),
		text("The fixes New Vegas needs, each from its author's own page: xNVSE, JIP LN NVSE, kNVSE, lStewieAl's Tweaks, New Vegas Tick Fix and the 4GB Patcher."),
		text("New Vegas or Tale of Two Wastelands (WORLD), each with its own saves; your own mods (YOUR MODS)."),
		text("Updates put themselves in, signed. undo everything takes it all out again."))
}

var groups = []struct{ id, title string }{
	{"foot", "On foot"}, {"gun", "Gun feel"}, {"coop", "Playing together"}, {"drive", "Driving"}, {"world", "The world"},
}

// KeysLines: the keys that do something in this game
func KeysLines(g Game) []scene.Line {
	var l []scene.Line
	shown := func(k Key) bool {
		return k.Group == "light" && g.Full || k.Group == "weather" && g.Weather || k.Group != "light" && k.Group != "weather"
	}
	for _, gr := range groups {
		var ks []scene.Line
		for _, k := range Keys {
			if (k.Group == gr.id || gr.id == "world" && (k.Group == "light" || k.Group == "weather")) && shown(k) {
				ks = append(ks, key(k.Label, k.Help))
			}
		}
		if gr.id == "drive" {
			if !g.Full {
				continue
			}
			ks = append([]scene.Line{key("W A S D", "Drive")}, ks...)
		}
		if len(ks) == 0 {
			continue
		}
		l = append(l, head(gr.title))
		l = append(l, ks...)
		switch gr.id {
		case "foot":
			l = append(l, key("Shift+Space+RMB", "Dropkick (weapon away)"))
		case "coop":
			l = append(l, key("E", "On a friend: trade; on a downed friend: get them up"),
				key("1", "Downed: use a stimpak"),
				key("Ctrl+Alt+End", "Panic: frees the mouse, ends playing together"),
				head("Chat commands"),
				key("/join", "Play in the host's world"),
				key("/leave", "Go home with what you earned"),
				key("/ff on | off", "Friendly fire (the host's rule)"),
				key("/name Name", "The name friends see"),
				key("/ranges", "Show co-op's distances"))
		}
	}
	return l
}

// The right column's pills
const (
	WorldFNV = "FNV"
	WorldTTW = "TTW"
)

// Columns: the help column, and (once the mod is in) the world and where the mods live (label: "GAME FOLDER", "MOD
// ORGANIZER 2", "VORTEX")
func Columns(installed, ttw bool, mods string) (left, right []scene.Side) {
	left = []scene.Side{{Head: "New here?", Pills: []string{FirstTime, WhatsIn, KeysPill}}}
	if installed {
		right = []scene.Side{
			{Head: "World", Pills: []string{WorldFNV, WorldTTW}, On: []bool{!ttw, ttw}, Row: true},
			{Head: "Your mods", Pills: []string{mods}},
		}
	}
	return
}

// ModsView is what the YOUR MODS panel shows: the choices (one filled), what the one picked does, a pill to show setup
// ModOrganizer.exe ("" none)
type ModsView struct {
	Pills     []string
	On        []bool
	Say       string
	PickExe   string
	NoneFound bool
}

// ModsFolderSay: what the game folder (the default) has in it
const ModsFolderSay = "Mods put straight into the game's Data folder are in: by hand, Nexus Mod Manager, FOMM, and Vortex once it has deployed. Mod Organizer 2 keeps its mods apart: pick it above to play with them."

// ModsLines: the YOUR MODS panel
func ModsLines(v ModsView) []scene.Line {
	l := []scene.Line{text("Play together with all your mods: pick where they are, and PLAY starts the game the way your mod manager does.")}
	for i, p := range v.Pills {
		l = append(l, scene.Line{Kind: scene.LPill, Text: p, On: i < len(v.On) && v.On[i]})
	}
	l = append(l, scene.Line{Kind: scene.LGap}, text(v.Say))
	if v.PickExe != "" {
		l = append(l, scene.Line{Kind: scene.LPill, Text: v.PickExe})
	}
	if v.NoneFound {
		l = append(l, text("(No Mod Organizer 2 or Vortex was found for this game.)"))
	}
	return append(l, scene.Line{Kind: scene.LWarn, Text: Mismatch},
		text("The host's report (top right) lists their load order, for friends to match."))
}
