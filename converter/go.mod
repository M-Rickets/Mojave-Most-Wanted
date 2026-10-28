module mmw

go 1.24.0

toolchain go1.24.7

require (
	github.com/bodgit/sevenzip v1.5.2
	github.com/hajimehoshi/go-mp3 v0.0.0
	golang.org/x/image v0.0.0-00010101000000-000000000000
	golang.org/x/sys v0.40.0
)

require (
	github.com/andybalholm/brotli v1.1.0 // indirect
	github.com/bodgit/plumbing v1.3.0 // indirect
	github.com/bodgit/windows v1.0.1 // indirect
	github.com/hashicorp/errwrap v1.0.0 // indirect
	github.com/hashicorp/go-multierror v1.1.1 // indirect
	github.com/hashicorp/golang-lru/v2 v2.0.7 // indirect
	github.com/klauspost/compress v1.17.9 // indirect
	github.com/pierrec/lz4/v4 v4.1.21 // indirect
	github.com/ulikunitz/xz v0.5.12 // indirect
	go4.org v0.0.0-20200411211856-f5505b9728dd // indirect
	golang.org/x/text v0.32.0 // indirect
)

replace github.com/hajimehoshi/go-mp3 => ./third_party/go-mp3

replace gopkg.in/yaml.v3 => ./vendor-only/yaml.v3

replace golang.org/x/image => ./vendor-only/ximage

replace golang.org/x/text => ./vendor-only/xtext

replace golang.org/x/sync => ./vendor-only/xsync

replace golang.org/x/sys => ./vendor-only/xsys

replace go4.org => ./vendor-only/go4

replace gopkg.in/check.v1 => ./vendor-only/check.v1
