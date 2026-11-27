# dtNet

**Vintage SunNet Manager / SGI NetVisualizer Analog Network Telemetry & Visual World Traceroute Console for CDE**

`dtNet` brings an authentic late-1980s / early-1990s UNIX workstation network operations rack to the Common Desktop Environment (`Utilities` subpanel), combining ballistic edge-lit analog needle gauges and dual-trace phosphor strip-charts with a live great-circle world traceroute radar, pure-`/proc/net` socket inspector, and LAN / protocol bench tools.

## Highlights

- **Top Analog Telemetry Rack (`18 fps` damped spring needle ballistics)**:
  - **3 Switchable `[WARM AMBER]` / `[DARK PHOSPHOR]` Edge-Lit Analog Dials**:
    1. `RX THROUGHPUT` — Auto-ranging `Kbps` / `Mbps` scale, peak-hold tick, packet activity jewel lamp, and glowing VFD digital readout driven directly from `/proc/net/dev`.
    2. `TX THROUGHPUT` — Auto-ranging `Kbps` / `Mbps` ballistic needle, peak-hold tick, error/drop indicator lamp, and VFD readout.
    3. `RTT LATENCY / RF LINK` — Switchable live ICMP RTT (`0–250 ms` + jitter VFD) or Wi-Fi RF signal (`-100..-30 dBm` + link quality `%` from `/proc/net/wireless`).
  - **Phosphor Dual-Trace RX/TX Rolling Strip-Chart & NIC Inspector** — Per-interface or `ALL` selector from `/sys/class/net/` displaying live `UP`/`DOWN` jewel LED, IPv4/IPv6 addresses, MAC, link speed, MTU, cumulative RX/TX `GiB`, and error/drop counters.
- **3-Tab Sunken Workstation Deck**:
  1. **World Traceroute Radar** — Targets any host/IP with 1-click presets (`1.1.1.1`, `8.8.8.8`, `github.com`, `pskreporter.info`, `kiwisdr.com`, `arrl.org`, `noaa.gov`), `[▶ TRACE ROUTE]` (`traceroute -n` / `tracepath -n` with automatic rootless `ping -t <ttl>` TTL-stepping fallback), `[▶ CONTINUOUS PING]`, cached IP geolocation (`~/.cache/dtnet/ipgeo.json`), and glowing neon great-circle hop arcs from Home QTH (`Newington, CT` / configurable) over an embedded vector world map alongside a tractor-feed greenbar hop table.
  2. **Active Sockets & Ports (`/proc/net`)** — Pure-Python `/proc/net/tcp`, `tcp6`, `udp`, `udp6` hex decoder + `/proc/[pid]/fd` socket-inode-to-process mapper with `[LISTEN ONLY]`, `[ESTABLISHED]`, protocol filters, and live search.
  3. **LAN Sweep & Bench Tools** — Threaded `/24` subnet & `/proc/net/arp` neighbor sweeper (`22, 53, 80, 443, 445, 631, 5900, 8073, 8080`) paired with an integrated **DNS / TCP Port Banner / TLS Certificate / HTTP Header** diagnostic bench.

## Install

```bash
cd dtNet && ./install.sh
```

## Self-Test

```bash
./dtnet --self-test
```
