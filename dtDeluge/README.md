# dtDeluge

**Transmission-Inspired BitTorrent Client for CDE in Authentic OSF/Motif Livery**

`dtDeluge` brings the classic Transmission GTK workflow and layout to the Common Desktop Environment (`Utilities` / `Applications` subpanel), styled automatically from CDE's active `xrdb` Motif palette (`#AEB2C3` 3D bevel chrome, `#FFF7E9` warm cream wells, and `#EEF4DB` greenbar alternating rows).

## Features

- **Self-Contained Pure-Python 3 BitTorrent Engine**: Bencode codec, single/multi-file `.torrent` parser & creator, `magnet:` URI parser (`btih` hex & Base32), HTTP/HTTPS & BEP-15 UDP tracker announces, TCP BitTorrent Peer Wire protocol (`51413`), BEP-10 Extension Protocol + BEP-9 `ut_metadata` magnet resolution, pipelined 16 KiB block transfers, SHA-1 piece verification, seeding, and global / Turtle (`Alt Speed`) token-bucket rate limiting.
- **Signature Transmission UX**:
  - Chiseled 3D Motif toolbar (`Open`, `Magnet`, `Create`, `Start`, `Pause`, `Start All`, `Pause All`, `Remove`, `Inspector`, `Prefs`) + `.torrent` drag-and-drop.
  - Status & tracker filter bar (`All`, `Active`, `Downloading`, `Seeding`, `Paused`, `Finished`) + live search.
  - Custom 3-line Transmission transfer cards (or `Compact View`) with 3D Motif bevel progress bars colored by state.
  - Dockable **5-Tab Transmission Inspector**: `Activity / Info`, `Peers` (client ID decoder), `Trackers` (multi-tier + add tracker), `Files` (per-file Wanted & Priority), and `Pieces` (live 2D Cairo piece-map grid).
  - Bottom status bar with **Turtle `Alt Speed`** toggle, live Up/Down throughput sparkline, free disk space (`os.statvfs`), and listening port LED.
- **CDE Desktop Integration**: Front Panel icon (`PUSH_ACTION` & `DROP_ACTION` for `.torrent` drops) + `TORRENT_FILE` (`*.torrent`) `dtfile` file association.

## Install

```bash
cd dtDeluge && ./install.sh
```

## Self-Test

```bash
./dtdeluge --self-test
```
