{
  lib,
  stdenv,
  cmake,
  pkg-config,
  kdePackages,
  libmpdclient,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "krunner-mpd";
  version = "0.1";

  src = lib.cleanSource ./.;

  nativeBuildInputs = [
    cmake
    pkg-config
    kdePackages.extra-cmake-modules
    kdePackages.wrapQtAppsHook
  ];

  buildInputs = [
    kdePackages.qtbase
    kdePackages.kcmutils
    kdePackages.kconfig
    kdePackages.kcoreaddons
    kdePackages.ki18n
    kdePackages.krunner
    libmpdclient
  ];

  # Only plugins are installed, there is nothing to wrap.
  dontWrapQtApps = true;

  meta = {
    description = "KRunner plugin (Plasma 6) that searches the MPD library";
    homepage = "https://github.com/KiTTYsh/mpd-krunner";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
})
