# dtDeck

**Vintage 1980s 4-Track Simul-Sync Tape & Aircheck Studio for CDE (`Utilities` Subpanel)**

`dtDeck` brings a tactile 1980s TASCAM Portastudio / TEAC / Nakamichi-inspired 4-track multitrack recorder and aircheck mastering deck to the Common Desktop Environment.

## Highlights

- **Backlit Mechanical Tape Well (`30 fps` Cairo)**: Smoked acrylic viewing window with warm incandescent interior cabinet glow, switchable **7" 3-Slot Spun-Aluminum NAB Reels** and **Studio CrO₂ Cassette Shell**, dynamic magnetic tape pack transfer (`0%` → `100%`), Permalloy `4-TRACK SIMUL-SYNC` head shield, engaging pinch roller, and spring-loaded tension arms that physically flex on transport start/stop.
- **Dual Warm-Backlit Ballistic Analog VU Meters (`CH L` & `CH R`)**: Honey-amber incandescent meter faces (`-20` to `+3 dB` red overmodulation arc), realistic attack/release needle ballistics driven by live GStreamer `level` RMS/peak telemetry, and crimson jewel `PEAK` clip LEDs.
- **Mechanical Drum Counter & Cyan VFD Auto-Locator**: 4-digit 3D rolling mechanical drum counter (`0000`–`9999`) + glowing cyan VFD `MM:SS.d` readout with `[RESET 0000]`, `[MEMORY]`, and `[RTZ]` (Return-to-Zero) auto-stop wind.
- **4-Track Simul-Sync Studio Mixer & Live Overdubbing**: 4 hardware channel strips (`TRK 1`–`TRK 4`) with illuminated vertical studio faders, rotary `PAN (L-R)` knobs, 12-segment vertical LED bar meters, `[REC ARM]`, `[MUTE]`, `[SOLO]`, live GStreamer mic/desktop monitor recording into `~/Music/dtDeck/`, built-in **4-Track Demo Tape Synthesizer**, `~/Music/dtRadio` aircheck quick-browser, and `numpy` stereo 16-bit `[BOUNCE MASTER .WAV]`.
- **10-Band Graphic Equalizer + Varispeed (`±20%`) & Tape Bias / NR**: Live GStreamer `equalizer-10bands` (`31 Hz`–`16 kHz` with `FLAT`, `VOICE / SSB`, `WARM TAPE`, and `LOUDNESS` presets), real speed+pitch `VARISPEED (±20%)` rotary control, and `TAPE BIAS / NR` (`FLAT`, `WARM`, `DOLBY-B`) head/oxide contouring.

## Install

```bash
cd dtDeck && ./install.sh
```
