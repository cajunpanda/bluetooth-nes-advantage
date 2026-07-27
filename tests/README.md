# tests/

Host-side tests for firmware logic that is worth checking without a board in hand. No hardware, no
ESP-IDF, no PlatformIO: a normal host compiler builds the code under test against stubs.

```bash
./tests/run.sh          # build and run everything; non-zero exit on failure
CXX=clang++ ./tests/run.sh
```

Build output lands in `tests/.build/` (gitignored).

## How the code under test gets here

The logic being tested is `static` inside `firmware/main/app_main.cpp` and sits behind ESP-IDF
headers, so `run.sh` lifts the relevant section straight out of that file and the test includes it.
The tests therefore run the shipped state machine rather than a transcription of it, which is the
whole point: edit `app_main.cpp` and the tests see the edit.

The section boundaries are the banner comments already in the source, not line numbers, so they
survive edits above them. If a marker is renamed or removed, `run.sh` fails loudly with the marker it
was looking for instead of testing an empty fragment. Renaming a banner comment means updating the
matching string in `run.sh`.

Anything the extracted section touches is stubbed in the test: a fake clock, the `bt::NesInput`
fields, the `settings::` constants it reads, and `ESP_LOGI`. A fake clock rather than real time keeps
window and pulse boundaries exact instead of timing-dependent.

## What is covered

`chords_test.cpp` covers the chord layer (Select as a shift key, see
[`../docs/FIRMWARE.md`](../docs/FIRMWARE.md)) across all three window modes:

- **Off**: Select is an ordinary button, held while held, and no chord fires.
- **Hold**: Minus withheld for the whole press and pulsed on release; a chord reached long after the
  press still works.
- **Bounded**: a chord inside the window fires and follows the hold; the window closing commits Minus
  so it can be held; a member arriving after the window does *not* chord; a tap shorter than the
  window still pulses on release.

Plus the edge cases the layer exists to get right: a direction already held when Select lands keeps
steering rather than chording, a member landing in the same poll as Select still chords, two taps
with a direction held do not produce a phantom chord, and re-pressing Select inside the release pulse
truncates it rather than leaking Minus into the new press.

Run `tests/.build/chords_test -v` to see the layer's own log lines interleaved with the scenarios.

## Adding a test

Add the `.cpp` next to this file, then in `run.sh` add an `extract` call for the section it needs and
a `run_case` line. Keep each test a plain `main()` returning non-zero on failure; there is no test
framework and nothing here needs one.
