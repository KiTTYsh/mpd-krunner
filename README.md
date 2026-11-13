# krunner-mpd

KRunner plugin (Plasma 6) that searches the MPD library. Selecting a track
inserts it right after the current song and skips to it.

- Type any part of the artist, album and/or title (3+ characters). Matching is
  case- and accent-insensitive; every word must match somewhere.
- Prefix with `mpd ` to rank MPD results above other runners.
- Prefix with `q ` to only queue the track after the current song, without
  skipping to it (also ranks MPD results first).
- Connects using `MPD_HOST` / `MPD_PORT` (incl. `password@host`), defaulting to
  localhost:6600. The environment that counts is plasmashell's / krunner's.

Results show the album's cover (MPD's `albumart`), cached in memory per album.
Queue-only results get a small "append" emblem on the cover.

By default the library index (artist/album/title) is cached in memory and
reloaded when MPD's database update time changes. This can be turned off in
System Settings → Search → KRunner → MPD (configure); queries then go straight
to MPD as `search` commands. That's fresher, but slower per keystroke, only
case-insensitive (not accent-insensitive) and limited to ranking the first 500
songs MPD returns.

## Build & install

Requires KF6 (Runner, I18n, CoreAddons, Config, KCMUtils), ECM and libmpdclient >= 2.20.

    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
    sudo cmake --install build
    kquitapp6 krunner   # restarts on next use

## Nix

A flake is included; the package builds both plugins into
`$out/lib/qt-6/plugins/kf6/krunner{,/kcms}`, which is on `QT_PLUGIN_PATH` once
it is installed into a profile.

    nix build                  # result/
    nix develop                # cmake, KF6 deps, clang-tools, mpc
    nix profile install .      # then: kquitapp6 krunner

On NixOS, add it system-wide via the overlay:

    # flake.nix
    inputs.krunner-mpd.url = "github:KiTTYsh/mpd-krunner";
    # configuration.nix
    nixpkgs.overlays = [ inputs.krunner-mpd.overlays.default ];
    environment.systemPackages = [ pkgs.krunner-mpd ];

`MPD_HOST` / `MPD_PORT` must be set in plasmashell's environment, e.g. via
`environment.sessionVariables`.
