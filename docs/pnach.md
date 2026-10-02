# PNACH files

The fork changes how PCSX2 finds PNACH files, adds section markers and CRC
lists, and lets the command line supply PNACH files and lines. The
implementation is in `pcsx2/Patch.cpp`.

## Automatic discovery

When the game has a [content alias](content_folders.md#content-aliases), its
only automatic cheat file is the bundle's `<alias>.pnach`.

Otherwise PCSX2 searches the cheats folder and then each
[additional content folder](content_folders.md#additional-content-folders).
In each folder it searches first the folder itself and then its subfolders for
`<serial>_<CRC>*.pnach`, `<CRC>*.pnach`, and `<serial>.pnach`, loading each
file once in that order and in path order within a pattern. Game patch files
use the same patterns in the patches folder only.

## Sections

In cheat files, a section named `[+Name]` is always enabled and `[-Name]` is
always disabled, whatever the game's enabled cheat list says. The marker is
not part of the name. Other named sections follow the enabled list, and
patches outside any section are always enabled. Cheat files apply only while
cheats are enabled. When two sections share a name, the first one loaded is
kept.

A `crc=` line restricts its section to the listed CRCs:

```ini
[Widescreen]
crc=C0659AD1,C071D4C1
patch=1,EE,00100000,word,00000000
```

Each CRC has exactly eight hexadecimal digits. A section without `crc=`
applies to every CRC, and a malformed list makes the section apply to none.

## Command-line PNACH

`-pnach <path>` loads an existing PNACH file. The option may be repeated, and
files load in command-line order. When any `-pnach` file is given, it replaces
automatic loading of cheat files, game patch files, and the bundled patch
archive, and the files follow the
cheat rules above, including the `+` and `-` markers and the cheat enable
setting. GameDB patches are unaffected.

`-pnach-line <line>` adds one PNACH command, such as a `patch=` line. The
option may be repeated. The lines form one unnamed cheat group after all
file-based cheats and therefore apply while cheats are enabled. A line that
contains a line break or does not parse as a command is rejected at startup.

## Tests

`tests/ctest/core/patch_tests.cpp` covers the section markers, command-line
files replacing automatic loading in order, ordered discovery across content
folders, alias cheat files, and command-line line validation.
