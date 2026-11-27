# dtMaps

**Tactical Amateur Radio GridTracker, Multi-Mode Slippy Atlas & Live PSKReporter Propagation Map for CDE**

`dtMaps` brings an authentic OSF/Motif-beveled cartographic workstation and live HF/VHF propagation display to the Common Desktop Environment (`Utilities` / `Applications` subpanel). Pre-configured out of the box for **`W1AW` (`FN31pr` — Newington, CT)** with editable station settings stored in `~/.config/dtmaps/config.json`.

## Highlights

- **Smooth Zoomable Slippy-Tile Map Engine (`z=2..15`)**: Click-drag panning, cursor-anchored scroll-wheel zoom, quick jump buttons (`[HOME FN31pr]`, `[USA]`, `[WORLD]`), persistent SQLite tile cache (`~/.cache/dtmaps/tiles.sqlite`), and an embedded offline vector world coastline & US/country boundary dataset so maps stay sharp even off-grid in the field.
- **5 Switchable Tactical Cartography Modes**: `GRIDTRACKER DARK`, `TOPOGRAPHIC` (`OpenTopoMap`), `DAYLIGHT ATLAS` (`OpenStreetMap`), `AMBER PHOSPHOR` (vintage CRT monochrome), and `NIGHT-VISION RED` (dark-shack red-light preservation).
- **Dual Live PSKReporter Engine (`W1AW` + Worldwide FT8 / All-Mode)**:
  - **24-Hour `W1AW` Historical Query**: Polls `retrieve.pskreporter.info` (respecting the 5-minute cache policy in `~/.cache/dtmaps/psk_{CALL}.xml`) and highlights all `W1AW` TX/RX paths with pulsing gold/cyan great-circle geodesic arcs, directional beacons, and callsign + dB SNR pills.
  - **Real-Time Live MQTT Feed (`mqtt.pskreporter.info:1883`)**: Pure-stdlib MQTT v3.1.1 streaming client subscribed to live `W1AW`, home grid `FN31`, and worldwide `FT8` / `FT4` / `JS8` / `CW` / `WSPR` spots with shaded 4-char Maidenhead grid squares and band-colored (`160m`–`2m`) great-circle arcs.
- **ADIF Log Import & WSJT-X UDP (`127.0.0.1:2237`)**: Import `.adi` logs (auto-detects `~/.local/share/WSJT-X/wsjtx_log.adi`) to highlight Worked/Confirmed Maidenhead grids, plus listen for live WSJT-X UDP decode packets.
- **Tactical Overlays & VFD Telemetry**: Adaptive `2/4/6-char` Maidenhead grid overlay, real-time astronomical **Solar Greyline Day/Night Terminator** + subsolar marker, interactive **Range & Bearing Ruler**, live cursor `LAT / LON / GRID / DISTANCE (km & mi) / SHORT & LONG PATH BEARING` VFD readout, clickable band-filter bar, and collapsible greenbar **Live Spot Roster**.

## Install

```bash
cd dtMaps && ./install.sh
```
