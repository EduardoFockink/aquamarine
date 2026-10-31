# Maintainer: Eduardo Fockink <54868856+EduardoFockink@users.noreply.github.com>
# Based on the official Arch package (Maintainer: Caleb Maclennan <caleb@alerque.com>); the only differences
# are the AQ_MGPU_CPU_COPY patches (exported from branch mgpu-cpu-copy-0.15.1 by export-patches.sh) and the
# marker files below.
#
# pkgrel N.M = official pkgrel N + revision M of the patch set. It is higher than the official pkgrel, so a
# routine upgrade keeps this package until a new upstream version lands; aquamarine-mgpu-cpu-copy.hook warns
# when that happens.

pkgname=aquamarine
pkgver=0.15.1
pkgrel=1.3
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
sha256sums=('2f9de98c0bd1b7b1b09c576e390a2fef436449762fb334163c414f0c300296f2'
            '4a8d1909f2c126a507f7e794341a309cc20d1dd959bfb021cfb3663b8310470c'
            'b334db3259051448350227cd58dbccc6d7b41e7ff004af8a37077323f9e54f97'
            'ebab48654656a28131ffd9eab5f3ccda40297e32efe3c369f5a3060aab577ef5')

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
