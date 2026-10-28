package car

import (
	"fmt"
	"math"
	"os"
	"path/filepath"

	"mmw/internal/dds"
)

// Paint shop textures shared by every car (textures\mmw\paint).
const PaintEnvMap = `textures\mmw\paint\env.dds`

// PaintEnvMaskGloss is the mask the converted models start with (the Gloss finish); set by WriteSharedPaint.
var PaintEnvMaskGloss = `textures\mmw\paint\m40.dds`

func PaintMaskPath(level uint8) string { return fmt.Sprintf(`textures\mmw\paint\m%02X.dds`, level) }

// WriteSharedPaint writes the reflection cube map and the mask for each finish's reflection level (glossLevel is the
// converted models' default).
func WriteSharedPaint(dataDir string, glossLevel uint8, levels []uint8) error {
	dir := filepath.Join(dataDir, "textures", "mmw", "paint")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "env.dds"), dds.Cube(64, envColour), 0o644); err != nil {
		return err
	}
	for _, l := range append(levels, glossLevel) {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("m%02X.dds", l)), dds.Solid(l, l, l, l), 0o644); err != nil {
			return err
		}
	}
	PaintEnvMaskGloss = PaintMaskPath(glossLevel)
	return nil
}

// envColour is a desert day seen from a car: sky above (z up, the game's world axes), a bright horizon, sand and rock
// below, with a few soft clouds so chrome has something to show.
func envColour(x, y, z float64) [3]uint8 {
	mix := func(a, b [3]float64, t float64) [3]float64 {
		t = math.Max(0, math.Min(1, t))
		return [3]float64{a[0] + (b[0]-a[0])*t, a[1] + (b[1]-a[1])*t, a[2] + (b[2]-a[2])*t}
	}
	var c [3]float64
	if z >= 0 {
		c = mix([3]float64{232, 226, 212}, [3]float64{86, 130, 192}, math.Pow(z, 0.55))
		az := math.Atan2(y, x)
		cloud := math.Max(0, math.Sin(az*3+1.3)*math.Sin(az*5+0.4)) * math.Max(0, 1-math.Abs(z-0.3)*3)
		c = mix(c, [3]float64{245, 245, 245}, cloud*0.5)
	} else {
		c = mix([3]float64{168, 142, 108}, [3]float64{58, 50, 42}, math.Pow(-z, 0.45))
	}
	return [3]uint8{uint8(c[0]), uint8(c[1]), uint8(c[2])}
}
