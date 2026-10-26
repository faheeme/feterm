# Image for packaging/windows/build.sh: MinGW-w64 cross compiler and libraries,
# Wine (to run the tests and Inno Setup) and Inno Setup itself.
FROM fedora:44

RUN dnf -y -q install make findutils zip mingw64-gcc mingw64-pkg-config mingw64-SDL3 mingw64-pcre2 \
        mingw64-freetype mingw64-fontconfig mingw64-angleproject wine xorg-x11-server-Xvfb && \
    dnf clean all

# Inno Setup 6.7.3, checked against the SHA-256 GitHub publishes for the release.
ARG INNO_URL=https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe
ARG INNO_SHA256=9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732
ENV WINEPREFIX=/opt/wine WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
RUN curl -fsSL -o /tmp/is.exe "$INNO_URL" && \
    echo "$INNO_SHA256  /tmp/is.exe" | sha256sum -c - && \
    xvfb-run -a sh -c 'wineboot -i && wine /tmp/is.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER \
        "/DIR=C:\\InnoSetup" && wineserver -w' && \
    test -f "$WINEPREFIX/drive_c/InnoSetup/ISCC.exe" && \
    rm -f /tmp/is.exe
