# Frame pacing online (2026-11-27)

Brawl Online board card (Sarah): "i feel like emissary is just not the same. esp when i play online i can max out at like
180 fps ish out of 240 ... it feels laggy, inconsistent ... how can we better guarantee good frame pacing all the time?
dolphin is just so much more usable right now", then "start on 1 and 2 [predict across rollbacks, fall back evenly]
and just generally ... make sure we keep frames as consistent as possible ... optimizations that we've made to dolphin
/ that dolphin has done itself ... if we've applied them to emissary".

Branch `brawlonline/pacing` (off `brawlonline/windows-build` db94c4b). Everything here is presentation: no guest
memory, no simulation, no timing the other player sees.

## What her match showed

Sarah's last online match before the card (v0.7.4, RTX 4070, 240 Hz fixed-refresh screen, Predict at 240, 2.25x with
MSAA 4x, ping ~110 ms, delay 1): 303 rollbacks in 498 frames (61%), re-run frames 2.4 ms, no late frames. The game
kept up; the picture didn't:

1. **Every frame right after a rollback was shown alone.** Predict marked it discontinuous
   (`VI_HLE_NoteRollback` -> `aurora_mark_frame_discontinuous`) and sealed it without inserted frames: one picture for
   a whole game frame. At a rollback every other frame, the picture switched between 240 and 60 Hz several times a
   second.
2. **Each frame showed the moment it was ready.** Deterministic timing (`DetClock`, all of Brawl Online) presents a
   frame at its seal; a frame that ran a rollback's re-run first comes ~3.5-7.5 ms later than one that didn't, and its
   inserted frames, timed from its own image, moved with it. Dolphin holds each frame to a steady cadence instead
   (PresentStats::PresentHoldUntil); measured there (GitHub #43), the hold has 3-7x fewer misplaced frames.
3. **No backstop online.** The slot controller (`report_producer_paced`) only runs in the retrace-paced path, which
   Brawl Online never takes, and it stepped 3 -> 2 -> 1 -> 0: 180 on a 240 Hz fixed screen shows pictures for one, one
   and two refreshes, which moves unevenly.
4. **The presenter's spacing rule pushed the inserted frames late.** An inserted frame waited a refresh +3% after the
   previous present; at 240 on 240 Hz the frames are a refresh apart anyway (16.683 / 4 = 4.171 ms), so each came
   later than the last and the third landed ~3.8 ms before the next frame's own image.

## What changed

- **Present hold** (`aurora_set_paced_present_schedule`, aurora.cpp `PresentHold`; vi.cpp gives the frame's due time,
  `DetClock::TakeFrameDue`, the target of the frame boundary's pacing wait). Dolphin's rule: a window of the last 120
  frames' arrivals after their due times, their 95th percentile without the stalls (a whole game frame past the
  window's median), at most 8 ms; the frame's own image waits until then and its inserted frames follow from there. Only
  a match's paced frames (`DetClock::MatchPacing`); menus show at once. Not on macOS, where frames present on the game's
  own thread (a hold would hold the game; `AURORA_PRESENT_HOLD=1` to measure it there).
- **Predict across rollbacks.** A rollback's re-run frames end in Aurora like any frame (`aurora_end_frame_unpresented`),
  the last of them drawn in full, so their transforms are retired as usual: the frame after a rollback is matched
  against the re-run's last frame, on its own (new) timeline, and carried on by that frame's motion.
  `AURORA_PREDICT_ROLLBACK_HOLD=1` restores the old hold. The frame is still counted (`rollback frames` in `frames:`).
- **Online slot controller** (`gx::report_presentation_health`, from aurora.cpp `end_frame`, one look per presented
  paced frame). A frame misses when its image came over 2 ms past its hold after the game thread waited for the
  previous frame's encode, an inserted frame presented over 2 ms late, inserted frames were dropped for a newer frame,
  or the presenter's queue overflowed. A quarter of a second's frames missing lowers the target one even step, never
  below one inserted frame; restoring is as before (clean windows, doubling backoff). The log line gives the counts
  by reason. `AURORA_PACING_CONTROLLER=0` turns it off.
- **Even steps** (`even_slot_count`, both controllers): on a fixed-refresh screen whose refresh is a whole multiple of
  60 Hz (m refreshes a game frame), only counts where (inserted + 1) divides m, or is above m: 240 Hz goes 240 -> 120
  -> 60. A G-Sync/FreeSync screen (runtime `DisplayRefresh` -> `aurora_set_display_variable_refresh`) and other rates
  take every count.
- **Spacing exactly a refresh** (presenter_main; `AURORA_SLOT_SPACING_PERMILLE`, 1030 = the old rule).
- **Telemetry.** Every match's log (dolphin.log, at the end of its pacing, beside `recomp pacing:`) has a
  `recomp display:` line: presents, game frames and how many showed alone, the frames' spacing (mean, standard
  deviation, largest gap, how many over 2 ms off a game frame), the holds, late and dropped inserted frames, and a
  modelled screen of the window's refresh (each picture from the first refresh after its present, four phases): pictures
  shown a second and the share whose motion was uneven (the next picture moved on over 30% more or less than its time on
  screen). `NSMBW_LOG_FPS=1` adds a `pacing:` line every 2 s; `AURORA_PRESENT_LOG=<file>` writes one line per present
  (steady-clock ns = QPC x 100 on Windows, slot, frame image, game position, late) to join with PresentMon.

## Measurements

Two recomps in a Direct match on this PC (Ryzen 7 9800X3D, RTX 4070, both windows on the primary 240 Hz LG,
1200x675), scripted dash dances and fast falls on both sides for 60 s, P2P through netsim. Each match is its own A/B:
one side old (`AURORA_PRESENT_HOLD=0 AURORA_PREDICT_ROLLBACK_HOLD=1 AURORA_PACING_CONTROLLER=0
AURORA_SLOT_SPACING_PERMILLE=1030`), the other new, sides swapped between matches. `long_haul` (92 ms round trip) with
the Aggressive delay profile is Sarah's match: ~50% of frames roll back. Numbers are each side's `recomp display:` line
over a ~64 s match.

| condition | arm | presents/s | frames alone | frames >2 ms off | uneven | hold avg |
|---|---|---|---|---|---|---|
| long_haul, 240, 1x | old (7 sides) | 213-217 | 425-481 | 857-897 | 10.1-11.2% | - |
| long_haul, 240, 1x | hold + predict, spacing +3% (5 sides) | 230-236 | 50-67 | 178-273 | 4.8-5.9% | 4.0-4.7 ms |
| long_haul, 240, 1x | final (spacing a refresh; 4 sides) | 216-236 | 33-60 | 187-314 | 2.4-3.9% | 4.1-5.2 ms |
| long_haul, 240, 2.25x MSAA 4x | old / new | 212-217 / 231-236 | 468-483 / 57-74 | 841-917 / 183-236 | 10.0-11.2% / 2.4-3.0% | 4.1-4.6 ms |
| bad_wifi, 240 | old / final | 215 / 227 | 514 / 225 | 744 / 221 | 8.9% / 2.7% | 3.0 ms |
| long_haul, 120 | old / final | 112 / 118 | 475 / 70 | 908 / 207 | 16.6% / 5.0% | 4.0 ms |
| long_haul, interpolation off | old / final (hold only) | 60 / 60 | all | 913 / 196 | 17.6% / 5.1% | 4.3 ms |
| lan (no rollbacks), 240 | old / new (+3%) | 227 | 208 / 217 | 65 / 43 | 5.0% / 4.0% | 1.2 ms |

(The 141-222 and 208-217 "alone" frames are on both arms alike: frames Predict shows as drawn for other reasons,
camera jolts and low-match frames.)

The hold's percentile: lateness is bimodal. Without a hold ~80% of frames land within ±0.7 ms of the beat and ~15%
(those whose rollback came after the pacing wait had ended) 3.5-7.5 ms late. At the 80th percentile frames waited 0.8 ms
on average but 829 frames were over 2 ms off and 8.9% of motion uneven, against 178 and 5.0% at the 95th, in the same
match. There is no middle: the cost of steady frames is the hold, ~4 ms more from a frame being ready to its image
being shown, online with rollbacks (~1 ms without).

On the real screen (PresentMon 2.6, both windows on the primary 240 Hz monitor, the test raising them above every
other window; the recomp's per-present log joined to PresentMon's present times, inside the match's QPC marks), two
pairs with the final build, old side vs new side:

| arm | pictures shown/s | presents never shown | uneven motion | screen time in pictures held over 6 ms |
|---|---|---|---|---|
| old | 199-205 | 4.8-7.6% (the frames' own images: 16-27% of them) | 9.9-10.9% | 25-30% |
| new | 222-226 | 4.4-5.4% (the third inserted frame: 12-15% of them) | 2.9-3.4% | 11-13% |

The new side's third inserted frame went unshown 8-15% of the time whatever was tried, more often in the left window
than in the right one in most pairs, whichever arm ran where. Two things tried for it and dropped as no better in paired
runs: keeping a late frame's inserted frames on the hold's grid (skipping those it missed), and moving the hold's
offset earlier only slowly (Dolphin's SmoothEarlyPresentation rule).

Earlier in the day PresentMon's numbers were not usable: another session's two full-screen-sized recomp windows sat on
the same screen and covered the test's, and PresentMon counted their presents as never shown (12-50% for both arms
alike).

**Not seen by any of this: a GPU that can't keep up.** Two recomps at 2.25x MSAA 4x on the one RTX 4070 (each
rendering four pictures a game frame) presented ~233 and ~215 a second on time, but the screen showed ~180 for both
arms: the GPU finished them late. The presenter's own timings (acquire, present) stayed short, so the controller had
nothing to go on. Seeing it needs the GPU's completion times (fences or timestamps), which this does not measure yet.

Correctness: the hold, the spacing and the controller only time presents and choose how many inserted frames to draw;
Predict across rollbacks only changes which transforms the inserted frames come from. `test_direct_set_through_netsim`
(two recomps, bad Wi-Fi, three games, about 650 rollbacks on each side) on the final build: 6,837 confirmed checksums compared, 0
mismatches, frame traces identical.

## Dolphin's frame-pacing work, and the recomp

| Dolphin | the recomp |
|---|---|
| Present hold (PresentStats::PresentHoldUntil, #314's stall rule) | ported here |
| Rollbacks run in the throttle's sleep (paced sleep lead) | has it (DetClock::PacedSleepUntil, the core's lead) |
| Hidden re-run layers (GitHub #26) | has its own (BRAWL_RESIM_HIDE light draws, BRAWL_RESIM_SKIP) |
| Re-run frames not presented | has it (VI_HLE_SetPresentSuppressed, a460aa8) |
| Timer resolution and no power throttling (Timer::IncreaseResolution) | has it (RaiseWindowsProcessPriority) |
| Precise sleeps (1 ms slices, the last few ms precise) | has it (DetClock::PacedSleepUntil, wait_until_precise) |
| Audio waits for re-runs | has it (card 783) |
| Input suggestions, fresh pad reads at the loop top | has it (e4259e6b7, 2f8cad8) |
| Shaders compiled before use / cached across updates | its own: the pipeline recipe cache in UserData (survives updates), boot prewarm on 22 workers (~0.6 s), unready pipelines skipped |
| Whole-machine sessions: a remote input in the last 5.5-8 ms before the boundary waits for the next frame's sleep (MachineSession BoundarySleep, hold_rx) | **not in either Brawl session** (the core's GameplaySession has no such rule, so Dolphin's Brawl doesn't either) |

The last row is the one that would shrink the hold: it is exactly the late 15% (a rollback that starts after the
pacing wait ended). It changes the shared core, so Dolphin's Brawl too, and shows a misprediction one frame longer in
exchange; not done here.

## Still open

- **Fill a late frame instead of holding for it.** The hold makes every frame wait for the late ones. A frame that
  misses its time could instead be covered by one more predicted picture of the frame before (Predict at t = 1), and
  the hold drop to the on-time frames' spread (~1 ms). That needs an extra staged sample per frame (GPU: a fifth render
  at 240, kept only when used) and the presenter driving the slots from the screen's clock.
- **Cheaper inserted frames.** Each is a full re-render of the scene at the internal resolution and MSAA; at 2.25x MSAA
  4x on the 4070 that held up here, but a slower GPU runs out first there.
- macOS: hold off until measured on the Mac; its presents run on the game thread.
