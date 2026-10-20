# Gate Cue

An [AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods) community mod for the AlphaTheta
XDJ-AZ. Mod id: `to778899-gate-cue`.

On the **GATE CUE** pad page (press HOT CUE twice), the XDJ-AZ gates only a paused deck: hold
a cue and it plays, let go and it stops. On a playing deck the same pad jumps to the cue and
the deck plays on. With Gate Cue, a set cue pressed on a playing deck is gated too: it plays
while the pad is held, and on release the deck goes back to the cue and waits there, paused.

## How it works

When a set cue is pressed on a playing deck on the GATE CUE page, the mod sends one PLAY press
on the pad bus (the deck pauses where it is), holds the pad back for a few frames, then lets it
through. The deck program's own paused-deck gate does the rest. It uses SDK services only
(frame filters, a Mod Menu row, deck state): no hooks, no audio, no files, no network,
`"firmware": ["any"]`.

It does nothing, and the pads stay native, for: the HOT CUE page (there a paused deck plays on
from the cue, so a pause would only add a gap), an empty pad (it stores a new cue), a paused
deck, two pads at once, the 4-deck view, and a pad mode not chosen since the deck started.

The Mod Menu row **Gate Cue** turns it off; its page shows each deck's mode, page, play state
and set cues, and how many presses it gated.

## Frame layouts

All measured by others, none guessed here: the pad bus layout (PLAY byte 10 bit 0x02, mode
buttons byte 13, pads byte 14, +8 for the right side) from the
[XDJ-AZ emulator](https://github.com/Kyle-Hosman/xdj-az-emu)'s panel map and the AZ-MODS core;
the LED frame (pads at 0x1e, mode buttons at 0x0e, lime = secondary page) from the SDK's
`docs/reference-frames.md`.

## Tested

| Where | What | Result |
|---|---|---|
| macOS, Linux (ubuntu 24.04 arm64) | SDK harness, plain / ASan+UBSan / TSan | pass |
| same | `tests/gate_test.c`: the injected gesture, every native case, a short tap, both decks | pass |
| XDJ-AZ emulator, deck program 2.00 (`ep147-9e9454e357389ece`), AZ-MODS 1.2.4 | an earlier build of 0.1.0 (gating on any HOT CUE page): on GATE CUE with a playing deck, pad held 1.5 s, the cue played while held and stopped on release, deck paused on the cue; control with the mod off, the deck played on. That run also showed the HOT CUE page must be left native, which 0.1.0 does | pass |
| a real XDJ-AZ (firmware 2.0x) | 0.1.0, by the author | works |

## Build

```sh
make test AZ_SDK=path/to/az-mods-sdk    # SDK harness
make unit AZ_SDK_INCLUDE=path/to/az-mods-sdk/include
path/to/az-mods-sdk/tools/azmod-pack build .
```

GPL-3.0-only.
