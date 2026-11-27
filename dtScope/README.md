# dtScope

**Hybrid Vintage Analog CRT & Digital Storage Audio Oscilloscope / Function Generator for CDE**

`dtScope` brings a benchtop **Tektronix 465B Analog CRT + HP 54600 Digital Storage Oscilloscope (DSO)** faceplate to the Common Desktop Environment (`Radio Apps` subpanel). Designed as the companion instrument to `dtRadio`, it taps system monitor audio (`PulseAudio` / `PipeWire` `.monitor`), microphone/line-in, or its built-in **Bench Function, CTCSS PL & DTMF Radio Tone Generator**.

## Highlights

- **4 CRT Phosphor & DSO Themes**: `P31 GREEN`, `P7 AMBER`, `P4 WHITE`, and `DSO DIGITAL` with adjustable phosphor persistence trail buffer, beam halo bloom, and draggable `ΔT` / `ΔV` measurement cursors.
- **5 Display Modes**: `SCOPE (Y-T)`, `DUAL (CH1 + GEN/R)`, `FFT SPECTRUM` (`4096`-pt Hanning `20 Hz – 20 kHz` with Log/Lin scale, Peak-Hold & harmonic peak callouts), `WATERFALL` phosphor spectrogram, and `X-Y LISSAJOUS` phase vectorscope.
- **Live VFD Frequency Counter & Telemetry Bar**: Sub-bin interpolated `FREQ (Hz)`, `PERIOD (ms)`, `Vpp`, `Vrms`, `PEAK (dBFS)`, `DOMINANT FFT (Hz)`, and `TRIG'D` lock indicator.
- **Spun-Aluminum Rotary Controls**: `TIME/DIV` (`0.2 ms – 50 ms`), `VOLTS/DIV` (`0.2x – 25x`), `TRIG LEVEL`, `Y-POS`, `INTENSITY`, and `PERSIST`.
- **Built-in Bench Function & Radio Tone Deck**: `SINE`, `SQUARE`, `TRIANGLE`, `SAWTOOTH`, `WHITE NOISE`, and `2-TONE SSB (700+1900 Hz)`, plus `1 kHz PROBE COMP` calibrator, standard EIA `CTCSS PL` tones (`67.0 – 254.1 Hz`), interactive `4x4 DTMF` keypad, and live speaker output (`[GEN OUT ON]`).

## Install

```bash
cd dtScope && ./install.sh
```
