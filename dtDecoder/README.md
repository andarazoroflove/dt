# dtDecoder

**Vintage HF Digital, CW/Morse, RTTY, SAME/EAS, HF Weather-Fax & SSTV Multimode Decoder for CDE (`Radio Apps`)**

`dtDecoder` brings a benchtop 1980s/1990s communications terminal unit & facsimile decoder faceplate (inspired by the HAL Communications ST-8000, Kantronics KAM TNC, and Alden Marine Weatherfax) to the Common Desktop Environment (`Radio Apps` subpanel). Automatically taps live desktop audio (`pactl` PulseAudio/PipeWire `.monitor` so whatever plays in `dtRadio` feeds `dtDecoder` directly!), `MIC / LINE-IN`, `.WAV` airchecks from `~/Music/dtRadio`, or its built-in **Live Test Signal Transmitter**.

### Capabilities
- **Top Instrument Deck**: Backlit ballistic **Analog Signal / SNR Meter**, scrolling `200–3000 Hz` **Phosphor Waterfall** with draggable/clickable passband & Mark/Space cursors, authentic **HAL ST-6 Crossed-Ellipse X-Y Tuning Scope**, and glowing **VFD Telemetry & Sync Lock LEDs**.
- **5 Real-Time DSP Demodulators + Built-in `[▶ SEND TEST SIGNAL]` Loopback**:
  1. **`CW / MORSE`**: Narrowband quadrature envelope detector (`400–1000 Hz`, `[SNAP PEAK]`), adaptive WPM & dit/dah tracker (`5–45 WPM`), and perforated `#FFF7E9` teletype paper roll.
  2. **`RTTY (BAUDOT)`**: `45.45` / `50` / `75` baud (`170` / `425` / `850 Hz` shift), matched-filter ATC discriminator, ITA2 `LTRS`/`FIGS` + `USOS` decoder, and live 5-bit punched paper-tape indicator.
  3. **`SAME / EAS (NOAA/FEMA)`**: `520.83 baud` AFSK (`2083.3` / `1562.5 Hz`) `ZCZC-ORG-EEE-PSSCCC+TTTT-JJJHHMM-LLLLLLLL-` emergency header decoder with plain-English FIPS/event translation and alert lamp.
  4. **`HF WEFAX (IOC 576 / 120 LPM)`**: Analytic Hilbert FM discriminator (`1500 Hz` black .. `2300 Hz` white + phasing auto-align) rendering live onto an Alden-style **Thermal Drum Facsimile Canvas** (`[INVERT]`, `SLANT / FINE`, `[SAVE PNG]`).
  5. **`SSTV (ROBOT 36 / MARTIN M1)`**: `1200 Hz` sync + VIS code auto-detection rendering live `320x240` / `320x256` color Slow-Scan TV frames (`[SAVE PNG]`).

## Install

```bash
cd dtDecoder && ./install.sh
```
