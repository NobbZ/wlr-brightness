{stdenv, pkg-config, wayland, glib}:

stdenv.mkDerivation {
  pname = "wlr-gamma-service";
  version = "0-unstable-2024-11-28";

  src = ./.;

  makeFlags = ["INSTALL_PATH=$(out)"];

  nativeBuildInputs = [pkg-config glib];

  buildInputs = [wayland];
}
