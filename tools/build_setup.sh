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
