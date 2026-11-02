#!/bin/sh
# Builds MojaveMostWanted-Setup.exe: the plugin, the.esp and the launcher packed as its payload (our own files only).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-"$ROOT/out"}
rm -rf "$OUT/payload"
mkdir -p "$OUT/payload/Data/NVSE/Plugins" "$OUT/payload/MojaveMostWanted"
cd "$ROOT" && python3 tools/gen.py >/dev/null && sh plugin/build.sh >/dev/null
python3 tools/gen_esp.py "$OUT/payload/Data/MojaveMostWanted.esp" >/dev/null
cp plugin/build/MojaveMostWanted.dll "$OUT/payload/Data/NVSE/Plugins/"
cd "$ROOT/converter"
# Each exe's Windows resources
VER=$(sed -n 's/^const appVersion = "\(.*\)"/\1/p' cmd/mmw-setup/main.go)
winres() {
	V4=$(echo "$VER" | tr . ,),0
	{
		echo "1 ICON \"$ROOT/converter/cmd/mmw-setup/setup.ico\""
		[ -n "$4" ] && echo "1 24 \"$4\""
		echo "1 VERSIONINFO FILEVERSION $V4 PRODUCTVERSION $V4 FILEOS 0x40004 FILETYPE 0x1"
		echo 'BEGIN BLOCK "StringFileInfo" BEGIN BLOCK "040904B0" BEGIN'
		echo '  VALUE "CompanyName", "M-Rickets"'
		echo "  VALUE \"FileDescription\", \"$2\""
		echo "  VALUE \"FileVersion\", \"$VER\""
		echo "  VALUE \"InternalName\", \"$3\""
		echo '  VALUE "LegalCopyright", "Copyright (c) 2026 M-Rickets. MIT License."'
		echo "  VALUE \"OriginalFilename\", \"$3\""
		echo '  VALUE "ProductName", "Mojave Enhanced"'
		echo "  VALUE \"ProductVersion\", \"$VER\""
		echo '  VALUE "Comments", "Co-op multiplayer and quality-of-life for Fallout: New Vegas. Source: github.com/M-Rickets/Mojave-Most-Wanted"'
		echo 'END END BLOCK "VarFileInfo" BEGIN VALUE "Translation", 0x409, 1200 END END'
	} > "$OUT/res.rc"
	llvm-rc -no-preprocess -fo "$OUT/res.res" "$OUT/res.rc"
	llvm-cvtres /machine:x64 /out:"$1/rsrc_windows_amd64.syso" "$OUT/res.res" >/dev/null
}
winres cmd/mmw-launch "Mojave Enhanced Launcher" mmw-launch.exe ""
winres cmd/mmw-setup "Mojave Enhanced Setup" "MojaveEnhanced-Setup-$VER.exe" "$ROOT/converter/cmd/mmw-setup/setup.manifest"
export GOFLAGS=-mod=vendor GOPROXY=off
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o "$OUT/payload/MojaveMostWanted/mmw-launch.exe" ./cmd/mmw-launch
( cd "$OUT/payload" && rm -f ../payload.zip && python3 -c "
import zipfile,os
z=zipfile.ZipFile('../payload.zip','w',zipfile.ZIP_DEFLATED)
for d,_,fs in os.walk('.'):
    for f in fs:
        p=os.path.join(d,f)[2:]; z.write(p,p.replace(os.sep,'/'))
z.close()" )
cp "$OUT/payload.zip" cmd/mmw-setup/payload.zip
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags="-s -w -H windowsgui" -o "$OUT/MojaveMostWanted-Setup.exe" ./cmd/mmw-setup
ls -la "$OUT/MojaveMostWanted-Setup.exe"
