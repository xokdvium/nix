{
  pkgs,
  lib,
}:
let

  evalNixpkgs =
    { nixpkgs, expectedHash }:
    let
      inherit (pkgs) runCommand nix;
    in
    runCommand "eval-nixos" { buildInputs = [ nix ]; } ''
      type -p nix-env
      # Note: we're filtering out nixos-install-tools because https://github.com/NixOS/nixpkgs/pull/153594#issuecomment-1020530593.
      (
        set -x
        time nix-env --option max-call-depth 30000 --store dummy:// -f ${nixpkgs} -qaP --drv-path | sort | grep -v nixos-install-tools > packages
        [[ $(sha1sum < packages | cut -c1-40) = ${expectedHash} ]]
      )
      mkdir $out
    '';

  nixpkgsRevisions = {
    "release-16.03" = {
      rev = "dda40aa8d18c836cecf64742ace96e14662afdc6";
      hash = "sha256-j0cdOBBKgeAgvSxpoc8jSysIciGXRs0X3SJoTjSTdGw=";
      expected = "30c04bbc5b672e145c142d7d555dca83cb1c132f";
    };

    "nixpkgs-regression" = {
      rev = "215d4d0fd80ca5163643b03a33fde804a29cc1e2";
      hash = "sha256-uGJ0VXIhWKGXxkeNnq4TvV3CIOkUJ3PAoLZ3HMzNVMw=";
      expected = "e01b031fc9785a572a38be6bc473957e3b6faad7";
    };
  };
in

lib.mapAttrs (
  name:
  {
    hash,
    rev,
    expected,
  }:
  evalNixpkgs {
    nixpkgs = pkgs.fetchFromGitHub {
      owner = "NixOS";
      repo = "nixpkgs";
      inherit hash;
      inherit rev;
    };

    expectedHash = expected;
  }
) nixpkgsRevisions
