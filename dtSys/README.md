# dtSys

**Vintage Control-Room Analog Dial & Strip-Chart System Telemetry Console for CDE (`Utilities` Subpanel)**

`dtSys` brings a 1970s/1980s control-room analog meter panel fused with classic SGI IRIX `gr_osview` and SunOS `perfmeter` strip-chart recorders to the Common Desktop Environment:

- **4 Edge-Lit Ballistic Analog Needle Gauges** (`CPU Utilization %`, `Memory & Swap`, `Core Thermal °C / °F` prioritizing `x86_pkg_temp`, and `Battery & AC Mains Power` (`BAT0`/`BAT1` + `AC`) with live voltage, wattage, `Wh` battery health, and jewel pilot lamps), switchable between **Warm Incandescent Cream/Amber Dials** and **Dark Radium / Phosphor Night Dials**.
- **4 Rolling 120-Sample Drum / Strip-Chart Recorders** (`CPU & Load Avg`, `Network RX/TX`, `Disk I/O + / & /home Fuel Bars`, and an SGI `gr_osview` **Per-Core & Memory Pressure Matrix**), switchable between **Cream Paper-Roll + Fountain-Pen Ink** (with tractor-feed sprocket holes) and **CRT Green/Amber Phosphor** traces.
- **Glowing VFD Host/Uptime Strip & Collapsible Greenbar Process Inspector** reading directly from `/proc` & `/sys` with zero `psutil` dependency, plus a 1-click **`[COMPACT RACK]`** (`340×600`) vertical desktop telemetry rack mode.

## Install

```bash
cd dtSys && ./install.sh
```
