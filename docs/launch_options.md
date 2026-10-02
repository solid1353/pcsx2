# Launch options

These command-line options are added or changed by the fork. None of them
changes persistent settings. Options are parsed in `pcsx2-qt/QtHost.cpp`, and
`pcsx2-qt -help` lists them with the upstream options.

Input recording options are described in
[input recording capture](input_recording_capture.md), and `-pnach` and
`-pnach-line` in [PNACH files](pnach.md).

## Process overrides

| Option | Effect |
| --- | --- |
| `-surfaceless` | Runs without creating or activating any window; implies `-nogui` and batch mode. |
| `-mute` | Mutes audio output. |
| `-read-only-settings` | Prevents every settings INI write. |
| `-pine-port <port>` | Uses PINE slot `1024` through `65535` when PINE is enabled. |
| `-centered-window` | Sizes the game window to the presented image without borders, fitted within a 1024×768 frame independent of display scaling, centers it on its screen, and suppresses starting fullscreen. The window follows later aspect-ratio changes. |

## Speed

`-turbo` and `-unlimited` keep their upstream meaning. The fork adds:

| Option | Effect |
| --- | --- |
| `-turbo-for <seconds>` | Uses Turbo for the given time after the game starts, then Normal. |
| `-unlimited-for <seconds>` | Uses Unlimited for the given time after the game starts, then Normal. |
| `-unlimited-for-frames <frames>` | Uses Unlimited for the given number of emulated frames, then Normal, or Turbo when `-turbo` is also given. A VM reset starts the count again. |

A timed mode returns to Normal only if the speed is still that mode. Timed
and frame-counted options cannot be combined with each other or with
`-unlimited`, and the timed options cannot be combined with `-turbo`. When no
speed option is given, the **Start Games in Unlimited Mode** setting
(`StartInUnlimitedMode` in `[Framerate]`) starts games in Unlimited.

## Memory cards

| Option | Effect |
| --- | --- |
| `-memory-card <path>` | Uses an existing card file as the port 1 card, enabled as a file card. Other slots keep their settings. |
| `-memory-card none` | Disconnects every slot, including multitap slots. `none` is case-insensitive; give a path to use a file named `none`. |
| `-discard-memory-card-writes` | Reports every memory card write and erase as successful without changing any card. |
| `-volatile-memory-card` | Keeps file card changes in RAM for the life of the process. |

When `-memory-card` is repeated, the last one applies.

Outside volatile mode, file cards are opened with shared access, so other
processes can open the same file. In discard mode they are opened read-only,
the card checksum is not written back, and the memory card never reports
busy, so savestates, state loads, and shutdown are not blocked by card
activity.

In volatile mode, each file card is read once into RAM and later reads see
the writes and erases made during the process, including after the card is
reopened. A missing or empty card file starts as a blank 8 MB card in RAM, and
raw `.bin` or `.mc2` cards are converted in RAM. Source files are never
written. Folder cards are disconnected with a warning.

`-volatile-memory-card` cannot be combined with
`-discard-memory-card-writes`, and it replaces the discard mode that
`-agent-replay` otherwise selects.

## Agent replay

`-agent-replay` starts an input recording replay for analysis over
[PINE](pine_agent_control.md). It implies `-surfaceless` and batch mode, mutes
audio, enables PINE, prevents settings writes, and discards memory card writes
unless `-volatile-memory-card` is given. The replay starts paused, and the
**Start Games in Unlimited Mode** setting is ignored.

It requires `-input-recording` playback and `-pine-port`, and it cannot be
combined with capture options or speed options. Batch mode exits PCSX2 when
the VM shuts down, for example after the PINE shutdown opcode.

## Tests

`tests/ctest/core/memory_card_tests.cpp` covers volatile cards: RAM-only
writes and erases across reopens, missing cards, raw card conversion, and
disconnected folder cards.
