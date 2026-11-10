# krunner-mpd

KRunner plugin (Plasma 6) that searches the MPD library. Selecting a track
inserts it right after the current song and skips to it.

- Type any part of the artist, album and/or title (3+ characters). Matching is
  case- and accent-insensitive; every word must match somewhere.
- Prefix with `mpd ` to rank MPD results above other runners.
- Connects using `MPD_HOST` / `MPD_PORT` (incl. `password@host`), defaulting to
  localhost:6600. The environment that counts is plasmashell's / krunner's.

The library index (artist/album/title) is cached in memory and reloaded when
MPD's database update time changes.

## Build & install

Requires KF6 (Runner, I18n, CoreAddons), ECM and libmpdclient >= 2.20.

    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
    sudo cmake --install build
    kquitapp6 krunner   # restarts on next use
