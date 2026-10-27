# LogosViewChannel listens wherever Qt puts local sockets — including under a
# macOS-length $TMPDIR, where an over-long socket name used to make listen fail
# and the backend crash. See view-channel/main.cpp.
{ pkgs }:

pkgs.stdenv.mkDerivation {
  pname = "logos-view-channel-test";
  version = "0.0.1";
  src = ./view-channel;
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.qt6.wrapQtAppsNoGuiHook ];
  buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects ];
  cmakeFlags = [ "-DLOGOS_VIEW_INCLUDE=${../cpp}" ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    QT_QPA_PLATFORM=offscreen ./view_channel_test
    runHook postCheck
  '';
  installPhase = "mkdir -p $out && touch $out/passed";
}
