# The toolchain image of the Android port: zippy's NDK image (NDK r29, a
# native tclsh 9.0) plus what the meson builds of pixman, cairo and
# fontconfig need. The SDK (aapt2, d8, apksigner, android.jar) is not in the
# image: the Makefile takes it from ANDROID_HOME, which is mounted.
#
#	docker build -t tk-android-build android/build

FROM zippy-toolchain:ndk

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip ninja-build gperf bison openjdk-17-jdk-headless \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --break-system-packages meson==1.6.1
