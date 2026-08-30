{
  description = "Ammonia — runtime tweak loader for macOS Apple Silicon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        ammonia = pkgs.stdenv.mkDerivation {
          pname = "ammonia";
          version = "1.0-pre";

          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.darwin.sigtool
          ];

          buildInputs = [
            pkgs.apple-sdk_26
          ];

          cmakeFlags = [
            "-DBUILD_CONFIGURATOR=OFF"
          ];

          preInstall = ''
            cp "$src/fridagum.dylib" "$PWD/fridagum.dylib" 2>/dev/null || true
          '';

          meta = with pkgs.lib; {
            description = "Runtime tweak loader for macOS Apple Silicon";
            homepage = "https://github.com/CoreBedtime/ammonia";
            license = licenses.mit;
            maintainers = [ ];
            platforms = [ "aarch64-darwin" ];
          };
        };
      in
      {
        packages.default = ammonia;
        packages.ammonia = ammonia;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ ammonia ];
          buildInputs = with pkgs; [
            clang-tools
            git
          ];
        };
      }
    ) // {
      darwinModules.default = { config, lib, pkgs, ... }:
        with lib;
        let
          cfg = config.services.ammonia;
        in {
          options.services.ammonia = {
            enable = mkEnableOption "Ammonia runtime tweak loader";
            package = mkOption {
              type = types.package;
              default = self.packages.${pkgs.system}.default;
              description = "The ammonia package to use.";
            };
          };

          config = mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];

            system.activationScripts.preUserActivation.text = ''
              sudo mkdir -p /private/var/ammonia/core/tweaks
              sudo mkdir -p /var/log/ammonia
              sudo chmod 755 /private/var/ammonia/core/tweaks
              sudo chmod 755 /var/log/ammonia
              if [ ! -f /private/var/ammonia/core/current.options ]; then
                sudo touch /private/var/ammonia/core/current.options
                sudo chmod 644 /private/var/ammonia/core/current.options
              fi
            '';

            launchd.daemons."com.ammonia.inject" = {
              serviceConfig = {
                Label = "com.ammonia.inject";
                ProgramArguments = [ "/private/var/ammonia/core/ammonia" ];
                RunAtLoad = true;
                KeepAlive = { SuccessfulExit = false; };
                StandardOutPath = "/var/log/ammonia/ammonia.log";
                StandardErrorPath = "/var/log/ammonia/ammonia.err";
              };
            };
          };
        };
    };
}
