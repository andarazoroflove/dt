# dtSat

**Vintage NASA/NORAD & Amateur Radio Satellite Pass & Doppler Tracker for CDE**

`dtSat` brings a 1980s/1990s Cheyenne Mountain / NASA Mission Control orbital tracking console and amateur satellite radio Doppler computer to the Common Desktop Environment (`Radio Apps` subpanel).

## Features

- **Pure-Python SGP4/J2 Secular Perturbation Propagator**: Standard 3-line NORAD TLE parser, iterative Kepler solver, J2 nodal regression ($\dot{\Omega}$) & apsidal precession ($\dot{\omega}$), atmospheric drag (`B*` / $\dot{n}$), GMST Earth rotation, ECI $\to$ ECEF $\to$ WGS84 geodetic & topocentric `Azimuth`, `Elevation`, `Slant Range`, and `Range-Rate` from `W1AW` (`FN31pr`, Newington, CT — customizable in `~/.config/dtsat/config.json`).
- **Offline + Live CelesTrak TLE Engine**: Ships with 15 built-in iconic spacecraft (`ISS (ZARYA)`, `SO-50`, `AO-91`, `RS-44`, `AO-7`, `IO-117`, `PO-101`, `LILACSAT-2`, `TEVEL-3`, `NOAA 15`, `NOAA 18`, `NOAA 19`, `METEOR-M2 3`, `CSS (TIANHE)`, `HUBBLE (HST)`) plus one-click `[⟳ UPDATE TLEs]` live fetching from CelesTrak (`amateur`, `weather`, `stations`) cached to `~/.cache/dtsat/tles.txt`.
- **Vintage NORAD World Map (`Gtk.DrawingArea`)**: Crisp offline vector continents, borders, lat/lon graticule, Maidenhead grid overlay, astronomical Solar Day/Night Terminator, 3-orbit sinusoidal ground-track, live spherical radio-visibility footprint ($R = R_E \arccos(R_E / (R_E + h))$), and Line-of-Sight beam (`NORAD CYAN/BLUE`, `P31 PHOSPHOR GREEN`, `AMBER TACTICAL` themes).
- **Circular Az/El Polar Sky-Dome Radar**: Plots all visible satellites (`El > 0°`) and full `AOS -> MAX EL -> LOS` pass trajectory arcs with 1-minute tick marks.
- **Glowing VFD Telemetry & Live Radio Doppler Computer**: Real-time `AZ`, `EL`, `RANGE`, `ALT`, `ORBIT #`, `SUNLIT/ECLIPSED`, `AOS/LOS T- HH:MM:SS` countdown, and full-duplex amateur/APT/LRPT Doppler-corrected `RX DOWNLINK MHz` & `TX UPLINK MHz` ($\Delta f$ Hz).
- **Greenbar 24-Hour Pass Predictor**: Interactive fanfold table of upcoming passes — click any row to preview its ground track and polar sky-dome arc.

## Install

```bash
cd dtSat && ./install.sh
```
