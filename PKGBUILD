# Maintainer: you <you@example.com>
pkgname=audiov
pkgver=1.0.0
pkgrel=1
pkgdesc="Lightweight X11 + PipeWire desktop audio visualizer"
arch=('x86_64' 'aarch64')
url="https://github.com/yourusername/audiov"
license=('MIT')
depends=('pipewire' 'libx11' 'libxext')
optdepends=(
    'gamemode: suspend visualizer while gaming'
    'picom: compositor required for transparency'
)
makedepends=('gcc' 'make' 'pkgconf')
source=("$pkgname-$pkgver.tar.gz::$url/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$pkgname-$pkgver"
    make
}

package() {
    cd "$pkgname-$pkgver"
    install -Dm755 audiov        "$pkgdir/usr/bin/audiov"
    install -Dm644 LICENSE       "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md     "$pkgdir/usr/share/doc/$pkgname/README.md"
}
