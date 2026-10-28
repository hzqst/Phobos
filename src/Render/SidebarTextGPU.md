# Sidebar cameo text GPU integration

First batch: native queue counts, Factory READY/HOLD and Super NameReadiness.
Optional wrapped UIName and replacement font providers are outside this batch.
Static source: `gamemd-reverse` issue #9, `b63a6fe`,
`docs/zh-CN/sidebar-text-gpu-handoff.md`.

## Data and rendering

- Scoped SimpleText observations copy final UTF-16 strings. The native single-line
  glyph calls supply final positions, packed colors and inclusive clipping bounds.
  No second formatting, production AI, font loading or CPU surface read occurs.
- Each redrawn cameo starts an empty pending run. Item end replaces the old run,
  including empty results; skipped native redraws retain only valid observations.
  List changes, scenario/sidebar invalidation, font metrics, CSF unload and factory
  queue/state fingerprints invalidate observations. Unknown data is omitted.
- GlyphCache gives fonts and glyphs separate typed IDs, with a controlled mapping to
  its own underlying FrameCache storage. It never passes font data to ShapeFrameSource.
  Native 1bpp records expand MSB-first to owned 0/1 masks on demand. Trailing row bits
  are zero. The native fallback is accepted only after matching the inverse X record;
  missing/uninitialized fallback is unsupported. IDs never revive after invalidation.
- Font binding includes object/data/table/bitmap/fallback identities and raster format.
  Load, fallback rebuild and both destructor entries notify before replacement/free.
  Layout metrics invalidate runs without changing unchanged masks. Limits are 8 MiB
  decoded mask storage, 65,536 record slots, 8 fonts, 512 UTF-16 units per print,
  1,024 commands per item and 8 nested scopes. Native records are capped at 64 MiB.
- Atlas requests explicitly distinguish indexed sprites from masks. Each CPU cache
  must outlive the atlas. Invalidations remove only the corresponding cache's frames;
  page retirement still waits for submission fences. Staging owns the copied bytes.
  Masks do not allocate palettes. Shader mode 4 maps nonzero mask values to full
  coverage and uses the command's packed foreground color.
- Counts and readiness follow flash and precede GCLOCK2. Paused Factory HOLD follows
  GCLOCK2. This distinction comes from native callsites, never the text contents.
  Original FullShadow flags do not add a shadow on this path. Black text backgrounds
  use alpha 176/256, approximating the native `dst * 80 / 256` with the accepted
  alpha-blending rounding differences. GPU clipping always stays within the canvas.

## Hook review (2026-10-28)

Used `discover_hooks.py` and `check_hook_conflicts.py`; reference fallback was Ares
3.0p1 because HookAnalysis.txt was absent. Discovery reported 10 new entries; the
existing 6A99C3 callback-body change was also reviewed. IDA MCP checked the actual
local EXE copy; all displaced windows end on complete instructions and contain no
relative calls/jumps. Every callback returns 0 without changing registers/stack.

| Hook / address | P0 length | P1 conflicts | P2 boundary | P3 inputs | P4 relative |
| --- | --- | --- | --- | --- | --- |
| Strip state 6A99C3 / 9 | PASS | PASS | PASS | PASS | PASS |
| Item end 6AA01C / 5 | PASS | PASS | PASS | PASS | PASS |
| Simple entry 4A5EB0 / 7 | PASS | PASS | PASS | PASS | PASS |
| Simple end 4A5FF2 / 5 | PASS | PASS | PASS | PASS | PASS |
| Glyph 434120 / 5 | PASS | PASS | PASS | PASS | PASS |
| Background 621B85 / 6 | PASS | PASS | PASS | PASS | PASS |
| Fallback rebuild 434700 / 5 | PASS | PASS | PASS | PASS | PASS |
| Destructor 433940 / 6 | PASS | PASS | PASS | PASS | PASS |
| Deleting destructor 434A40 / 6 | PASS | PASS | PASS | PASS | PASS |
| Font loader 433990 / 5 | PASS | PASS | PASS | PASS | PASS |
| CSF unload 734D30 / 5 | PASS | Stacked, verified | PASS | PASS | PASS |

All findings and ABI details:

- Initial candidate 621B80 exactly overlapped Ares `DSurface_FillRectWithColor`.
  Its actual export clamps the input rectangle and skips empty output by returning
  621D26. Moved the observation to 621B85, after Ares and the native prologue.
  ECX/EDX remain rect/surface; entry stack is current ESP + 34h. No overlap remains.
- 734D30 is intentionally stacked with `StringTable_Unload_ClearRawValues`.
  Both discard separate caches and return 0. Runtime trampoline inspection confirmed
  both exports in that order. Ares-only fallback conflict data does not list this
  Phobos hook, so it was checked separately in source and runtime memory.
- All other new hooks had no conflicts in the script. No short-size/NOP exceptions,
  return-address conflicts or relative-instruction relocation were required.
- 6A99C3 uses local doDraw +16, index +3C, Strip +24, ESI/EBP cell origin; ESP is
  the item scope token. 6AA01C has the same stack depth before loop advancement.
- Simple entry uses EDX final string and stack +4 target, +18 flags. Only verified
  Fancy returns 4A6143/4A619A permit reading upstream return at +428h. Accepted callers:
  6A9D80 count, 6A9E3B readiness, 6AA019 post-clock HOLD. Simple end current ESP +38h
  identifies its entry scope. Nonaccepted nested print calls suppress glyph capture.
- Glyph entry ECX is font; stack +4 UTF-16 unit, +8/+C pen X/Y, +10 color.
  Only native SingleLine return 43464D is accepted. Tabs generate no bitmap.
- Instance lifetime entries use ECX font. The static font loader has ECX filename,
  so it conservatively clears all font aliases rather than treating ECX as a font.

## Verification and deployment

- Debug build passed with the two new `.cpp` and two `.h` project items authorized
  by the user. No dependency, CI, YRpp or additional shader project items changed.
- `GlyphCacheTests`: bit order, padding, zero-width glyph, fallback identity,
  invalid/truncated records, budget rejection, reuse without decoding, metrics format
  replacement and same-address invalidation passed with MSVC x86.
- `FrameAtlasGPUTests`: actual D3D12 R8 and forced R32 paths passed, including mixed
  SHP/mask batches, zero/full mask coverage, recolor without upload, black background
  opacity and invalidating one cache without discarding the other's resident frames.
  Existing append, eviction, pinning, cancellation and 300 idle submissions also passed.
- Deployed DLL SHA256: `9482DCE43B1CF8FBECC5A56561E65BB0D12BB0369DC852E26D67AD0399726B72`.
  Backup: `D:/Ra2/Phobos-backup-sidebar-text-20261028-202117`.
  Ordinary-privilege Spawner process 38272 launched directly into a skirmish.
- Runtime memory confirmed all 11 game entry jumps reach the expected Phobos export;
  CSF chain contains both callbacks. Native bound font: 33,910 records, 3-byte stride,
  16 bitmap rows, 49 bytes/record, 17 line height. No surface pixel data was read.
- User completed the requested skirmish check and reported no anomalies. Logs captured
  Chinese `等待`, `就緒`, queue counts including 10/15/20, and Special (type 31) readiness.
  Glyph decoding grew only for newly encountered characters (11 observed glyphs).
  Submissions 9300-11700 show zero additional index/palette uploads with 112 total
  image uploads (101 SHP + 11 glyphs), one page and two palettes; no budget misses.
  Evidence: `D:/Ra2/SidebarText-evidence-20261028-38272/`.

Not yet demonstrated: every localized glyph/fallback, all charge-drain/on-hold super
states, font reload in a second game, and replacement providers. Supporting mapped Chinese glyphs
does not establish complete Chinese font coverage. Unknown font mutation/providers
require separate validation; pointer equality alone is not a lifetime notification.
