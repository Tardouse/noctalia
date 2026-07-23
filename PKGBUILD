# Maintainer: Jesse Wang <z872845991@gmail.com>

pkgname=noctalia-git
_pkgname=noctalia
pkgver=5.0.0.r4718.g55da1264e
pkgrel=1
pkgdesc='A sleek, customizable desktop shell crafted for Wayland'
arch=('x86_64' 'aarch64')
url='https://github.com/Tardouse/noctalia'
license=('MIT')
options=('!debug')
depends=(
  'cairo'
  'curl'
  'fontconfig'
  'freetype2'
  'gcc-libs'
  'git'
  'glib2'
  'glibc'
  'jemalloc'
  'libglvnd'
  'libpipewire'
  'libqalculate'
  'librsvg'
  'libsecret'
  'libsodium'
  'libwebp'
  'libwireplumber'
  'libxkbcommon'
  'libxml2'
  'md4c'
  'pam'
  'pango'
  'polkit'
  'sdbus-cpp'
  'tomlplusplus'
  'wayland'
)
makedepends=(
  'meson'
  'ninja'
  'nlohmann-json'
  'pkgconf'
  'stb'
  'wayland-protocols'
)
optdepends=(
  'ddcutil: external monitor brightness control'
  'gnome-keyring: Secret Service provider for persistent credentials'
  'upower: battery and power device integration'
)
provides=('noctalia')
conflicts=('noctalia' 'noctalia-bin')
source=("${_pkgname}::git+${url}.git#branch=main")
sha256sums=('SKIP')

pkgver() {
  cd "${_pkgname}"

  local version
  version="$(sed -n "s/^  version: '\([^']*\)',/\1/p" meson.build)"
  printf '%s.r%s.g%s' \
    "${version}" \
    "$(git rev-list --count HEAD)" \
    "$(git rev-parse --short=9 HEAD)"
}

build() {
  CXXFLAGS+=" -Wno-unused-result"
  arch-meson "${_pkgname}" build \
    -Db_ndebug=true \
    -Dnative_optimizations=false \
    -Dtests=disabled
  meson compile -C build
}

package() {
  meson install -C build --destdir "${pkgdir}"
  install -Dm644 "${_pkgname}/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
  install -Dm644 "${_pkgname}/README.md" "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}
