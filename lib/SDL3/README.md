# SDL3

Headers and the x86 import library of [SDL](https://github.com/libsdl-org/SDL) 3.4.18, taken
unmodified from `SDL3-devel-3.4.18-VC.zip` of the `release-3.4.18` GitHub release
(sha256 `78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd`).

- `include/SDL3/` - public headers
- `lib/x86/SDL3.lib` - import library for `SDL3.dll`
- `LICENSE.txt` - zlib license

Phobos links `SDL3.dll` with `/DELAYLOAD`, so the game still starts without it; the GPU render
window is then disabled. The matching `SDL3.dll` is in `lib/x86/` of the same package.
