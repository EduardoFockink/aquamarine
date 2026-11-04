# Maintainer: Eduardo Fockink <54868856+EduardoFockink@users.noreply.github.com>
# Based on the official Arch package (Maintainer: Caleb Maclennan <caleb@alerque.com>); the only differences
# are the AQ_MGPU_CPU_COPY patches (exported from branch mgpu-cpu-copy-0.16.0 by export-patches.sh) and the
# marker files below.
#
# pkgrel N.M = official pkgrel N + revision M of the patch set. It is higher than the official pkgrel, so a
# routine upgrade keeps this package until a new upstream version lands; aquamarine-mgpu-cpu-copy.hook warns
# when that happens.

pkgname=aquamarine
pkgver=0.16.0
pkgrel=1.1
pkgdesc='a very light linux rendering backend library (+ AQ_MGPU_CPU_COPY for NVIDIA-render / Intel-scanout)'
arch=(x86_64 aarch64)
url="https://github.com/hyprwm/$pkgname"
license=(BSD-3-Clause)
depends=(libgcc
         libstdc++
         glibc # libc.so libm.so
         hyprutils libhyprutils.so
         libdisplay-info libdisplay-info.so
         libdrm # libdrm.so
         libglvnd libEGL.so
         libinput # libinput.so
         mesa # libgbm.so
         opengl-driver
         pixman
         seatd libseat.so
         systemd-libs libudev.so
         wayland libwayland-client.so
         wayland-protocols)
makedepends=(cmake
             hyprwayland-scanner)
provides=("lib$pkgname.so")
_archive="$pkgname-$pkgver"
source=("$url/archive/v$pkgver/$_archive.tar.gz"
        0001-drm-add-AQ_MGPU_CPU_COPY-a-CPU-copy-path-for-seconda.patch
        0002-drm-AQ_MGPU_CPU_COPY-support-10-bit-X-A-2101010-outp.patch
        0003-drm-AQ_MGPU_CPU_COPY-wait-for-the-GPU-asleep-before-.patch)
sha256sums=('42339c3096c9de6582c0d71bc86b7e63334a3faf6a2b917d385ad093caf66c3c'
            '5b1bd80944f7333a471d0a3bcbdf513948722cb2168952f612451e72fae09102'
            '277b9d3b621ec4b2a4c99802688e8d748bcca6a37adc862a6fc5baae74da9af0'
            '1e2632b32a2c381aa21c955ef460bc6c0389a3e95f478328f6018485782d350d')

prepare() {
	cd "$_archive"
	local p
	for p in "${source[@]:1}"; do
		patch -Np1 -i "../$p"
	done
}

build() {
	cd "$_archive"
	cmake -B build \
		-D CMAKE_INSTALL_PREFIX=/usr \
		-D CMAKE_BUILD_TYPE=Release
	cmake --build build
}

package() {
	cd "$_archive"
	DESTDIR="$pkgdir" cmake --install build
	install -Dm0644 -t "$pkgdir/usr/share/licenses/$pkgname" LICENSE

	# Markers a compositor config can test before enabling the mode: with the official package the
	# NVIDIA->Intel copy aborts in Mesa, so a missing marker should mean "fall back, don't loop the login".
	install -Dm0644 /dev/stdin "$pkgdir/usr/share/aquamarine/mgpu-cpu-copy" <<EOF
aquamarine $pkgver-$pkgrel with AQ_MGPU_CPU_COPY (copies 8-bit frames).
https://github.com/EduardoFockink/aquamarine/tree/packaging
EOF
	install -Dm0644 /dev/stdin "$pkgdir/usr/share/aquamarine/mgpu-cpu-copy-10bit" <<EOF
aquamarine $pkgver-$pkgrel: AQ_MGPU_CPU_COPY also copies 10-bit frames (X/A*2101010, HDR).
EOF
}
