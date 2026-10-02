# Input recording capture

The fork changes how input recordings are opened from the command line and
adds marker capture during replay. The implementation is in
`pcsx2/Recording/` and the option parsing in `pcsx2-qt/QtHost.cpp`.

## Recording paths

`-input-recording <path>` replays a recording and
`-input-recording-create <path>` starts a new power-on recording. An absolute
path is used exactly. A relative path is resolved below the primary
`InputRecordings` folder and must not contain `..`; additional content folders
are not searched. PCSX2 does not create missing parent directories.

A recording that starts from a savestate keeps that savestate in the
directory `<recording>_SaveState` next to the recording.

## Read-only playback

Every replay, from the command line or the menu, opens the recording
read-only with shared read access, so several PCSX2 processes can replay the
same file at once. Record mode cannot be enabled during a replay, and the
recording's header and frame data are never rewritten.

When a replay reaches its last frame, PCSX2 pauses. Resuming at that point
ends the replay. With a capture directory, PCSX2 instead closes the recording
after the final frame and shuts the VM down, which exits a batch-mode
process.

## Marker capture

Capture is enabled by any of these options, which require
`-input-recording` playback (a capture directory without playback is
ignored):

| Option | Effect |
| --- | --- |
| `-input-recording-capture-directory <path>` | Writes captures to this directory and exits when the replay ends. |
| `-input-recording-capture-mode full\|screenshots\|savestates` | Selects the outputs; the default is `full`. |
| `-input-recording-capture-markers <spec>` | Captures only the listed 1-based marker numbers, such as `1,3-5`. |

A marker spec is a comma-separated list of positive numbers and ascending
`first-last` ranges; overlapping and adjacent ranges merge. Capture works only
with power-on recordings; a recording that starts from a savestate fails to
start.

A marker is the rising edge of L3 and R3 held together on port 1 or port 2 in
the replayed input, so holding the chord produces one marker. Markers are
numbered in replay order, including unselected ones, and each capture is named
with its marker number padded to three digits: `001`, `002`, and so on.

| Mode | Output for marker `NNN` |
| --- | --- |
| `full` | `NNN.png` and savestate `sstates/NNN/` |
| `screenshots` | `NNN.png` |
| `savestates` | savestate `sstates/NNN/` |

With a capture directory, PNGs go directly in it and savestates in its
`sstates/` subdirectory. Without one, savestates go to
`<Savestates folder>/<recording name>/NNN` and PNGs to
`<Snapshots folder>/<recording name>/NNN.png`. PCSX2 creates these
directories but does not empty them; an existing savestate with the same
name is replaced.

Savestates are directories containing the state components and a
`Screenshot.png`. In `full` mode the standalone PNG is the same encoded image
as the savestate's `Screenshot.png`. If the memory card is busy, the savestate
is skipped and logged, but a `full` capture still writes its PNG. Screenshots
use the savestate screenshot encoder and size.

## Tests

- `tests/ctest/core/input_recording_tests.cpp` covers concurrent read-only
  playback, the record-mode lock during playback, capture mode and marker
  parsing, marker selection, and capture directories.
- `tests/ctest/core/patch_tests.cpp` (`InputRecordingPath` tests) covers exact
  absolute paths and relative paths below the primary folder.
