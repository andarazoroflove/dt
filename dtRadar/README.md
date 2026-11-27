# dtRadar

**Vintage NWS NEXRAD Doppler Radar, GOES-18/16 Satellite, METAR Aviation Instrument Cluster & HF Solar Space-Weather Console for CDE**

`dtRadar` brings a 1980s/1990s tabletop meteorological & amateur radio propagation console to the Common Desktop Environment (`Radio Apps` subpanel). Styled with authentic OSF/Motif 3D bevels, CDE `xrdb` palette integration, glowing cyan-green Vacuum Fluorescent Displays (VFD), warm incandescent analog gauges, and a phosphor PPI radar scope.

## Features

- **Configurable Home QTH (`W1AW` / `FN31pr` default)**: Centered on Newington, CT (`41.7292° N, -72.7083° W`), nearest NEXRAD `KBOX`, nearest METAR `KBDL`, and GOES-16 `ne` (Northeast) sector — all editable in the top bar and persisted in `~/.config/dtradar/config.json`.
- **4 Switchable Tactical CRT Views + Animated `[▶ LOOP]` / Frame Scrubber**:
  1. **`NEXRAD DOPPLER MAP`**: Zoomable/pannable dark tactical slippy map (OpenStreetMap dark-LUT tiles + embedded vector US state & world coastline fallback) overlaid with live 6-frame (`30-min`) Iowa Environmental Mesonet NEXRAD Base Reflectivity (`N0Q`) tiles, standard NWS `dBZ` color scale bar, `50/100/150/200 km` range rings, optional rotating green phosphor **PPI Sweep Arm**, and active **NWS CAP Warning Polygons**.
  2. **`NWS STATION RIDGE RADAR`**: Direct NOAA NWS RIDGE station still & multi-frame `_loop.gif` animated viewer (`KBOX`, `KCBX`, `KMTX`, `KMSX`, `KOTX`, `KRIW`, `CONUS`, or any 4-letter NEXRAD ID).
  3. **`GOES-18 / 16 SATELLITE`**: Zoomable/pannable live NOAA NESDIS STAR imagery (`Northern Rockies GeoColor`, `Band-13 Clean IR`, `Band-09 Water Vapor`, `CONUS GeoColor`, `Full Disk Earth`) plus SWPC `Aurora Oval North`.
  4. **`HF SOLAR & PROPAGATION DESK`**: Live NOAA SWPC 6-hour GOES X-Ray Flare Flux logarithmic chart (`A/B/C/M/X`), Planetary `Kp` 3-hour bar graph, `10.7cm SFI` / Sunspot / Solar Wind `Bz` telemetry, and a real-time **HF Amateur Band Condition Matrix (`80m–6m` Day & Night `GOOD / FAIR / POOR` + VHF Aurora/E-Skip)**.
- **3 Backlit Analog Weather Instruments + Sunken Teletype**:
  - **Barometer (`28.5–31.0 inHg` / `hPa`)** with `STORM / CHANGE / FAIR` brass dial lettering
  - **360° Wind Compass & Gust VFD (`KT / MPH`)**
  - **Dual-Needle Thermometer / Dewpoint / Hygrometer (`-20°F..110°F` + `RH%`)**
  - **Sunken `#FFF7E9` / Amber Teletype Strip** displaying live raw & decoded `KBDL` **METAR / TAF** and active **NWS Severe Weather Alerts**.

## Install

```bash
cd dtRadar && ./install.sh
```
