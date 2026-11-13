{
  description = "KRunner plugin (Plasma 6) that searches the MPD library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      overlays.default = final: prev: {
        krunner-mpd = final.callPackage ./package.nix { };
      };

      packages = forAllSystems (pkgs: rec {
        krunner-mpd = pkgs.callPackage ./package.nix { };
        default = krunner-mpd;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.krunner-mpd ];
          packages = with pkgs; [ clang-tools mpc ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
