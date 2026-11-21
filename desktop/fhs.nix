# Buildroot invokes /usr/bin/file and the Bootlin toolchain's ELF interpreter.
{ pkgs ? import <nixpkgs> {} }:
pkgs.buildFHSEnv {
  name = "l2-desktop-make";
  targetPkgs = p: with p; [
    bash coreutils gcc gnumake which file bc cpio unzip rsync perl gawk
    wget git gnutar xz bzip2 gzip gnupatch diffutils findutils python3
    util-linux binutils cacert cmake ninja
  ];
  runScript = "make";
}
