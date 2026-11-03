{
  description = "Mirror the Steam Frame headset display over SSH";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system} self.packages.${system});
    in
    {
      packages = forAllSystems (
        pkgs: _: rec {
          tools = pkgs.stdenv.mkDerivation {
            pname = "framecopy-tools";
            version = "0.0.1";
            src = ./src;

            nativeBuildInputs = [ pkgs.pkg-config ];
            buildInputs = [
              pkgs.libdrm
              pkgs.vulkan-headers
              pkgs.vulkan-loader
            ];

            buildPhase = ''
              gcc -O2 -Wall -Wextra -Werror -o framecopy-kms kms.c proto.c $(pkg-config --cflags --libs libdrm)
              gcc -O2 -Wall -Wextra -Werror -o framecopy-capture capture.c gpu.c proto.c -lvulkan
            '';
            installPhase = ''
              install -Dm755 -t $out/bin framecopy-kms framecopy-capture
            '';
          };

          framecopy = pkgs.stdenvNoCC.mkDerivation {
            pname = "framecopy";
            version = "0.0.1";
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./framecopy
                ./src
              ];
            };

            nativeBuildInputs = [ pkgs.makeWrapper ];
            dontBuild = true;
            installPhase = ''
              install -Dm755 framecopy $out/libexec/framecopy/framecopy
              cp -r src $out/libexec/framecopy/src
              makeWrapper $out/libexec/framecopy/framecopy $out/bin/framecopy \
                --suffix PATH : ${
                  pkgs.lib.makeBinPath [
                    pkgs.coreutils
                    pkgs.gnugrep
                    pkgs.mpv
                    pkgs.openssh
                  ]
                }
            '';

            meta = {
              description = "Mirror the Steam Frame headset display over SSH";
              mainProgram = "framecopy";
              platforms = pkgs.lib.platforms.linux;
            };
          };

          default = framecopy;
        }
      );

      checks = forAllSystems (
        pkgs: packages: {
          inherit (packages) tools;
          format = pkgs.runCommand "framecopy-format" { nativeBuildInputs = [ pkgs.clang-tools ]; } ''
            cd ${
              pkgs.lib.fileset.toSource {
                root = ./.;
                fileset = pkgs.lib.fileset.unions [
                  ./.clang-format
                  ./src
                ];
              }
            }
            clang-format --dry-run -Werror src/*.c src/*.h
            touch $out
          '';
        }
      );

      devShells = forAllSystems (
        pkgs: packages: {
          default = pkgs.mkShell {
            inputsFrom = [ packages.tools ];
            packages = [ pkgs.clang-tools ];
          };
        }
      );

      formatter = forAllSystems (pkgs: _: pkgs.nixfmt);
    };
}
