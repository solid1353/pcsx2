# Content folders and aliases

The fork lets PCSX2 read game settings, cheat PNACH files, and memory cards
from folders outside its data directory, and lets several game identities share
one alias. The implementation is in `pcsx2/Pcsx2Config.cpp`
(`EmuFolders`), `pcsx2/VMManager.cpp`, `pcsx2/Patch.cpp`, and
`pcsx2/SIO/Memcard/MemoryCardFile.cpp`.

## Additional content folders

`PCSX2.ini` lists the folders as repeated `AdditionalContentFolders` entries in
`[Folders]`; the Folders settings page edits the same list. Relative entries
resolve from the data directory, and duplicates are ignored.

For game settings, cheat PNACH files, and memory cards, PCSX2 searches the
primary folder first and then each additional folder in order. Game patch
PNACH files and input recordings use only their primary folders.

### Game settings

Without a [content alias](#content-aliases), PCSX2 looks in each folder in turn
for `<serial>.ini`, then `<serial>_<CRC>.ini`, then the same two names in
subfolders. Subfolder matches are taken in path order, with a warning when
there is more than one. If nothing matches, the settings file is
`<GameSettings folder>/<serial>_<CRC>.ini`. A disc without a serial uses
`<CRC>.ini` the same way.

A `<serial>.ini` file applies to every CRC of that serial. Inside it, a
`[CRC.<CRC>.<section>]` section overrides keys of `<section>` for that CRC
only, and the per-game settings dialog writes its changes to those
CRC sections. CRC sections have no effect in files named with the CRC.

### Memory cards

A configured card name resolves to the first folder that contains it. The card
list shows cards from every folder, with the first copy of each name winning.
Creating a card fails when the name exists in any folder, and new cards are
created in the primary folder. Renaming keeps a card in its folder.

## Content aliases

`[ContentAliases]` in `PCSX2.ini` maps game identities to alias names:

```ini
[ContentAliases]
SLES-55605_C071D4C1 = UN5
SLES-55605 = UN5_Fallback
```

A key is either `<serial>_<CRC>` or a serial alone. Lookups are
case-insensitive, an exact serial-and-CRC key takes precedence over a serial
key, and the later of two identical keys wins. An alias must be a valid file
name; other values are ignored with a warning.

Alias files live directly in an additional content folder's `games/` directory:

| File | Used for |
| --- | --- |
| `<alias>.ini` | Game settings of every identity mapped to the alias. CRC sections are not applied. |
| `<alias>.pnach` | Automatic cheat PNACH of every identity mapped to the alias. |

Each file is looked up separately across the additional content folders, so
`games/<alias>.ini` and `games/<alias>.pnach` may be in different folders.
A file found in more than one folder is refused with an error. A missing file
means no game settings or no automatic cheat file; the alias still works with
no files. The old `games/<alias>/` layout is not supported. Game patch files
and the GameDB still use the detected serial.

### Local identity

A game matched by a serial key alone is identified by its alias; a game matched
by a serial-and-CRC key, or by no key, keeps its detected serial. The identity
names savestates (`<identity> (<CRC>).<slot>`) and debugger settings
(`<identity>_<CRC>.json`), keys playtime, and selects the title shown in the
game list and at boot. The title is the GameDB name for the identity, or the
file name when the GameDB has no entry; a custom title still takes precedence.
The detected serial and CRC remain in use for GameDB settings and patches,
memory-card filters, and achievements, and savestate names keep the detected
CRC.

## Tests

`tests/ctest/core/patch_tests.cpp` covers ordered cheat discovery across
content folders, independent alias file lookup and duplicate refusal, local
identity without files, game settings lookup across folders, and memory card
management across folders.
