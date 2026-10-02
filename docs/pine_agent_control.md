# PINE agent control

The fork extends PINE protocol version 1 with opcodes `0x10` through `0x1E`.
Opcodes `0x00` through `0x0F` keep their upstream meaning. The server is in
`pcsx2/PINE.cpp` and `pcsx2/PINE.h`.

## Transport and framing

On Windows, PINE listens on TCP `127.0.0.1` at the configured slot. Other
platforms use a Unix socket named `pcsx2.sock` (with `.<slot>` appended when
the slot is not the default `28011`) in `XDG_RUNTIME_DIR`, `TMPDIR` on macOS,
or `/tmp`. The [`-pine-port`](launch_options.md#process-overrides) launch
option changes the slot for one process.

All integers are little-endian. A request is `u32 total_size` (including the
size field) followed by one or more commands, each an opcode byte and its
arguments. One server thread handles one client at a time and executes the
commands of a message in order. A successful reply is
`u32 total_size, u8 0x00` followed by each command's reply payload. If any
command fails, the whole reply is `u32 5, u8 0xFF`; commands earlier in the
same message have already run.

## VM control opcodes

Each of these opcodes fails when no VM is running.

| Opcode | Arguments | Reply payload | Behavior |
| ---: | --- | --- | --- |
| `0x10` | none | none | Reloads patches from disk and clears CPU execution caches, then replies. |
| `0x11` | none | none | Queues a screenshot with the default snapshot name. |
| `0x12` | none | none | Pauses the VM, then replies. |
| `0x13` | none | none | Resumes the VM, then replies. |
| `0x14` | none | none | Clears CPU execution caches, then replies. |
| `0x15` | `u8 pad, u8 button, u16 milliseconds` | none | Presses one DualShock 2 button, waits on the PINE thread, then releases it. |

For `0x15`, `pad` is a unified slot (see [pad slots](#pad-slots)) holding a
DualShock 2, `milliseconds` is `1` through `1000`, and `button` is a digital
input in this order: Up `0`, Right `1`, Down `2`, Left `3`, Triangle `4`,
Circle `5`, Cross `6`, Square `7`, Select `8`, Start `9`, L1 `10`, L2 `11`,
R1 `12`, R2 `13`, L3 `14`, R3 `15`.

## Agent input opcodes

Agent input replaces the complete state of chosen controllers after each host
input poll. Every agent request starts with protocol version `1`, and every
successful reply payload starts with that version byte.

| Opcode | Request after the version | Reply payload after the version |
| ---: | --- | --- |
| `0x16` set states | `u8 count`, `count` records | none |
| `0x17` step | `u32 frames`, `u8 count`, `count` records | `u32 start_frame, u32 end_frame` |
| `0x18` get states | `u8 count`, `count` slot bytes | `u8 count`, `count` × `{u8 slot, u8 controlled, u8 state[18]}` |
| `0x19` release | `u8 count`, `count` slot bytes | none |

A record is `{u8 slot, u8 state[18]}`. Set, step, and get take one through
eight unique slots; release takes zero through eight, where zero releases
every controlled slot. `frames` must be positive. Every slot named by set,
step, or get must hold a DualShock 2.

- **Set states** installs persistent overrides for the listed slots and applies
  them immediately. Other controlled slots keep their states. Overrides stay
  until they are replaced, released, or cleared by a failure.
- **Step** installs the listed states for the duration of the step only, runs
  exactly `frames` frames, and then restores what those slots had before the
  step: a persistent override returns, and a slot that was not controlled is
  released and set to neutral. Slots not listed keep their persistent
  overrides. The reply is sent after the step completes, so the PINE thread is
  blocked for the whole step.
- **Get states** reports, for each requested slot, whether an override
  controls it and the pad's effective state after overrides are applied.
- **Release** removes overrides from the listed slots, ignores listed slots
  that are not controlled, and sets each released slot to neutral. Host input
  is no longer overridden for those slots.

Agent input is refused while an input recording is replaying. It is allowed
with no active recording and while recording, in which case the recording
stores the overridden input.

### Pad slots

Unified slots are `0` = port 1, `1` = port 2, `2`–`4` = multitap 1B–1D, and
`5`–`7` = multitap 2B–2D.

### State format

The 18-byte state is the input recording's DualShock 2 layout:

| Bytes | Content |
| --- | --- |
| `0` | Active-low buttons: bit 0 Select, 1 L3, 2 R3, 3 Start, 4 Up, 5 Right, 6 Down, 7 Left. |
| `1` | Active-low buttons: bit 0 L2, 1 R2, 2 L1, 3 R1, 4 Triangle, 5 Circle, 6 Cross, 7 Square. |
| `2`–`5` | Analog axes `RX, RY, LX, LY`; `0x7F` is centered. |
| `6`–`17` | Pressure for Right, Left, Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2. |

Neutral is `FF FF 7F 7F 7F 7F` followed by twelve zero bytes. Because each
record is a complete state, a step never inherits a button from an earlier
request for the slots it lists.

### Step frame intervals

`start_frame` and `end_frame` are values of the emulator's VBlank frame
counter, and a successful step satisfies
`(end_frame - start_frame) mod 2^32 == frames`.

- **Paused VM.** The step applies its states, frame-advances `frames`
  VBlanks, and reports the counter before advancing and after the advance
  pauses. The previous states are restored after the final frame's input
  poll has been processed, and the VM stays paused.
- **Running VM.** The step starts at the next input poll and covers `frames`
  consecutive polls. It reports the half-open interval from the first polled
  frame to one past the last. The previous states are restored at the
  following poll, before a recording samples it, so the game consumes the
  step's input for the full final frame. The VM keeps running.

### Fail-closed behavior

A failed agent request clears every override, aborts any step in progress,
sets the released slots to neutral, and returns the standard failure reply.
Failures include a malformed or wrongly versioned request, a missing VM, a
replay conflict, a slot without a DualShock 2, a step while another step is
in progress, and a step that cannot start or whose frame advance does not
start.

Overrides are also cleared, any step aborted, and released slots set to
neutral when the client disconnects, the VM resets or shuts down, a replay
starts while overrides are installed, or the VM pauses during a step for any
reason other than a paused step's frame advance completing.

## Replay analysis opcodes

These opcodes inspect a read-only input recording replay. Every request
starts with protocol version `1`, and every successful reply payload starts
with that version byte.

| Opcode | Request after the version | Reply payload after the version | Requires |
| ---: | --- | --- | --- |
| `0x1A` status | none | `u32 replay_frame, u32 total_frames, u32 vblank` | Replay |
| `0x1B` step | `u32 vblanks` | `u32 start_replay_frame, u32 end_replay_frame, u32 start_vblank, u32 end_vblank` | Paused replay |
| `0x1C` screenshot | `u32 length`, path bytes | none | Paused replay |
| `0x1D` shutdown | none | none | Paused replay, or paused [agent replay](#agent-replay) |
| `0x1E` GS dump | `u32 frames`, `u32 length`, path bytes | none | Paused replay |

"Replay" means an active input recording that is replaying and not recording.

- **Step** advances a positive number of VBlanks forward and pauses again. It
  fails when the step would pass the recording's last frame. A successful step
  advances the replay frame by exactly `vblanks`, and the VBlank counter by the
  same amount, except that a step starting at replay frame `0` and VBlank `0`
  advances the VBlank counter by `vblanks - 1`. Any other result is a failure.
- **Screenshot** writes a PNG to an absolute path of 1 through 32,768 bytes
  without NUL characters, using the same encoder and size as savestate
  screenshots, and replies after the file is written.
- **Shutdown** requests a VM shutdown and replies without waiting for it to
  finish.
- **GS dump** takes a positive GS frame count and an absolute path ending in
  `.png` (case-insensitive) with the same length limits. It queues a GS dump and
  replies once the request reaches the GS thread. The dump is written while
  later frames run: the path without `.png` is the base name for the dump
  (`.gs`, `.gs.xz`, or `.gs.zst`, following the GS dump compression setting)
  and for a screenshot. A request made while another snapshot is pending is
  ignored but still replies successfully.

A failed replay-analysis request returns the standard failure reply and does
not change agent input overrides. A replay step in progress is aborted when
the client disconnects or the VM resets or shuts down.

## Agent replay

The [`-agent-replay`](launch_options.md#agent-replay) launch option starts an
input recording replay paused, without a window, for analysis through these
opcodes. Replays launched without it can be analyzed the same way. A replay
that reaches its last frame pauses; resuming it there ends the replay, after
which the other replay-analysis opcodes fail and, in agent replay, shutdown
still succeeds once the VM is paused again.

## Lifecycle limitations

- The PINE thread notices a client disconnect only after an in-flight agent
  or replay step returns.
- Disabling PINE or changing its slot while a step is in progress can
  deadlock. The new setting is applied on the CPU thread, which calls
  `PINEServer::Deinitialize()` and joins the PINE thread while that thread
  waits for the CPU thread to finish the step. Nothing aborts the step before
  the join; aborting and neutralizing agent control first, with the step's
  wait observing the abort, would remove the deadlock. The same wait affects
  any PINE request that is waiting for the CPU thread at that moment.
- A failed VM-control, upstream, unknown, or replay-analysis command does not
  clear installed agent overrides. They remain until released, replaced, or
  cleared by an agent failure, disconnect, reset, or shutdown.

## Tests

- `tests/ctest/core/pine_agent_control_tests.cpp` covers agent opcode values,
  request parsing and rejection, slot lists, override and step-scoped restore
  state, running-step frame sequencing and half-open intervals, the
  recording/replay rule, and the state layout.
- `tests/ctest/core/pine_replay_analysis_tests.cpp` covers replay-analysis
  request parsing, path validation, the read-only replay rule, step bounds,
  and interval matching.
