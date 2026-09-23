# Fullscreen display choice — September 23, 2026

Issue [#168](https://github.com/omacom/try-omarchy-windows/issues/168)
reported that Immersive mode opens on the laptop's primary screen even when
an external display is preferable. The source candidate adds a **Fullscreen
display** choice to Settings > General and `-fullscreen-display` for scripted
launches. The selected Windows display device name is persisted separately
from the guest display count. It sets the first guest output's resolution and
window placement; later guest outputs follow connected host displays. A missing
saved target falls back to the primary screen without deleting the choice.

## Checks

- Go app tests passed locally; Windows test binary and launcher cross-built.
- The copied Windows test binary and launcher matched local SHA-256 values
  `2a117af0b9935394dfe24ec5e44e2ba7c5a16b516e1709c915226ab6332ac332`
  and `bdc27a82a6311598cb08b1bc2e0f4bf533f5a4cf0f4e166b7df37e18374cccea`.
- Native interactive Windows tests on the AMD laptop passed: primary and missing
  target selection, monitor name/bounds enumeration, fullscreen target sizing,
  and the Settings control saving/reloading the selected host display.
- The laptop had one active 1366 × 768 screen. A second active physical monitor
  was unavailable, so window placement onto a second screen was not observed.
  The next user report from a multi-monitor setup can confirm that last step;
  no broad hardware matrix is required for this source change.

This source candidate is not in public `v0.1.0`.
