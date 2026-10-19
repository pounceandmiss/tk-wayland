# Makefile --
#
#	Cross-build Tcl, Tk for Android and its dependencies (pixman, FreeType,
#	expat, fontconfig, cairo) with the NDK, and package wish as an APK. Run
#	it in the image of the Dockerfile here, through build.sh, which mounts
#	the sources and the SDK at the same paths as on the host.
#
#	The dependencies and Tcl and Tk are static libraries, compiled with
#	-fPIC, which are linked with tkAndroidMain.c into libmain.so, the
#	library a NativeActivity loads. The script libraries of Tcl and Tk and
#	the application are in a zip in the library, mounted with zipfs.
#
#	make deps		the dependencies
#	make tcl tk		Tcl and Tk
#	make apk		build/tk.apk, with APP_DIR (default: the demos)
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

TK_SRC		:= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
TCL_SRC		?= $(abspath $(TK_SRC)/../tcl)
ANDROID_ABI	?= x86_64
ANDROID_API	?= 30
BUILD		?= $(abspath $(TK_SRC)/../build/android-$(ANDROID_ABI))
SRCDIR		?= $(abspath $(TK_SRC)/../build/android-src)
ANDROID_HOME	?= $(abspath $(TK_SRC)/../android-sdk)
BUILD_TOOLS	?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/build-tools/*)))
PLATFORM_JAR	?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/platforms/android-*/android.jar)))
NPROC		:= $(shell nproc)

ifeq ($(ANDROID_ABI),x86_64)
  TRIPLE	:= x86_64-linux-android
  CPU_FAMILY	:= x86_64
else ifeq ($(ANDROID_ABI),arm64-v8a)
  TRIPLE	:= aarch64-linux-android
  CPU_FAMILY	:= aarch64
else
  $(error ANDROID_ABI must be x86_64 or arm64-v8a)
endif

CC		:= $(TRIPLE)$(ANDROID_API)-clang
CXX		:= $(TRIPLE)$(ANDROID_API)-clang++
AR		:= llvm-ar
RANLIB		:= llvm-ranlib
STRIP		:= llvm-strip
CFLAGS		:= -O2 -g -fPIC -ffunction-sections -fdata-sections
PREFIX		:= $(BUILD)/prefix
HOST_PREFIX	:= $(abspath $(BUILD)/../android-host)
HOST_TCLSH	:= $(HOST_PREFIX)/bin/tclsh9.1
TOOLCHAIN	:= CC=$(CC) CXX=$(CXX) AR=$(AR) RANLIB=$(RANLIB) STRIP=$(STRIP)

export PKG_CONFIG_LIBDIR := $(PREFIX)/lib/pkgconfig
export PKG_CONFIG_PATH :=

# ==== Sources ====

PIXMAN_VER	:= 0.46.4
FREETYPE_VER	:= 2.13.3
EXPAT_VER	:= 2.7.1
FONTCONFIG_VER	:= 2.16.0
CAIRO_VER	:= 1.18.4

PIXMAN_URL	:= https://cairographics.org/releases/pixman-$(PIXMAN_VER).tar.gz
FREETYPE_URL	:= https://downloads.sourceforge.net/project/freetype/freetype2/$(FREETYPE_VER)/freetype-$(FREETYPE_VER).tar.xz
EXPAT_URL	:= https://github.com/libexpat/libexpat/releases/download/R_$(subst .,_,$(EXPAT_VER))/expat-$(EXPAT_VER).tar.xz
FONTCONFIG_URL	:= https://www.freedesktop.org/software/fontconfig/release/fontconfig-$(FONTCONFIG_VER).tar.xz
CAIRO_URL	:= https://cairographics.org/releases/cairo-$(CAIRO_VER).tar.xz

PIXMAN_SHA256	:= d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c
FREETYPE_SHA256	:= 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
EXPAT_SHA256	:= 354552544b8f99012e5062f7d570ec77f14b412a3ff5c7d8d0dae62c0d217c30
FONTCONFIG_SHA256 := 6a33dc555cc9ba8b10caf7695878ef134eeb36d0af366041f639b1da9b6ed220
CAIRO_SHA256	:= 445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb

# Download (unless it is there) and check a tarball, and extract it in
# $(BUILD)/src.
define fetch
	mkdir -p $(SRCDIR) $(BUILD)/src
	[ -f $(SRCDIR)/$(notdir $(1)) ] || curl -fsSL -o $(SRCDIR)/$(notdir $(1)) $(1)
	echo "$(2)  $(SRCDIR)/$(notdir $(1))" | sha256sum -c --quiet
	rm -rf $(BUILD)/src/$(3)
	tar xf $(SRCDIR)/$(notdir $(1)) -C $(BUILD)/src
endef

.PHONY: all deps tcl tk apk host-tcl clean
all: apk

# ==== meson ====

CROSS_FILE := $(BUILD)/cross.txt

$(CROSS_FILE):
	mkdir -p $(BUILD)
	printf '%s\n' \
	    "[binaries]" \
	    "c = '$(CC)'" \
	    "cpp = '$(CXX)'" \
	    "ar = '$(AR)'" \
	    "strip = '$(STRIP)'" \
	    "pkg-config = 'pkg-config'" \
	    "[host_machine]" \
	    "system = 'android'" \
	    "cpu_family = '$(CPU_FAMILY)'" \
	    "cpu = '$(CPU_FAMILY)'" \
	    "endian = 'little'" \
	    "[built-in options]" \
	    "c_args = [$(foreach f,$(CFLAGS),'$(f)',)]" \
	    "[properties]" \
	    "pkg_config_libdir = '$(PREFIX)/lib/pkgconfig'" > $@

MESON_SETUP = meson setup --cross-file $(CROSS_FILE) --prefix $(PREFIX) \
	--libdir lib --default-library static --buildtype release \
	--wrap-mode nofallback

# ==== Dependencies ====

deps: $(PREFIX)/.cairo

$(PREFIX)/.pixman: $(CROSS_FILE)
	$(call fetch,$(PIXMAN_URL),$(PIXMAN_SHA256),pixman-$(PIXMAN_VER))
	cd $(BUILD)/src/pixman-$(PIXMAN_VER) && \
	    $(MESON_SETUP) -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
		-Dlibpng=disabled -Dopenmp=disabled _build && \
	    ninja -C _build install
	touch $@

$(PREFIX)/.freetype: $(CROSS_FILE)
	$(call fetch,$(FREETYPE_URL),$(FREETYPE_SHA256),freetype-$(FREETYPE_VER))
	cd $(BUILD)/src/freetype-$(FREETYPE_VER) && \
	    $(MESON_SETUP) -Dpng=disabled -Dbzip2=disabled -Dbrotli=disabled \
		-Dharfbuzz=disabled -Dzlib=system -Dtests=disabled _build && \
	    ninja -C _build install
	touch $@

$(PREFIX)/.expat: $(CROSS_FILE)
	$(call fetch,$(EXPAT_URL),$(EXPAT_SHA256),expat-$(EXPAT_VER))
	cd $(BUILD)/src/expat-$(EXPAT_VER) && \
	    $(TOOLCHAIN) CFLAGS="$(CFLAGS)" ./configure --host=$(TRIPLE) \
		--prefix=$(PREFIX) --disable-shared --without-docbook \
		--without-examples --without-tests --without-xmlwf && \
	    $(MAKE) -j$(NPROC) && $(MAKE) install
	touch $@

$(PREFIX)/.fontconfig: $(PREFIX)/.freetype $(PREFIX)/.expat
	$(call fetch,$(FONTCONFIG_URL),$(FONTCONFIG_SHA256),fontconfig-$(FONTCONFIG_VER))
	cd $(BUILD)/src/fontconfig-$(FONTCONFIG_VER) && \
	    $(MESON_SETUP) -Ddoc=disabled -Dtests=disabled -Dtools=disabled \
		-Dcache-build=disabled -Dnls=disabled -Dxml-backend=expat _build && \
	    ninja -C _build install
	touch $@

$(PREFIX)/.cairo: $(PREFIX)/.pixman $(PREFIX)/.fontconfig
	$(call fetch,$(CAIRO_URL),$(CAIRO_SHA256),cairo-$(CAIRO_VER))
	cd $(BUILD)/src/cairo-$(CAIRO_VER) && \
	    $(MESON_SETUP) -Dxlib=disabled -Dxcb=disabled -Dquartz=disabled \
		-Dpng=disabled -Dtee=disabled -Dfontconfig=enabled \
		-Dfreetype=enabled -Dglib=disabled -Dzlib=disabled \
		-Dspectre=disabled -Dsymbol-lookup=disabled -Dtests=disabled \
		-Dlzo=disabled -Dgtk_doc=false _build && \
	    ninja -C _build install
	touch $@

# ==== Tcl ====

# A native tclsh of the same version, for the install steps of the cross
# build and to make the zip of the scripts.
host-tcl: $(HOST_TCLSH)

$(HOST_TCLSH):
	mkdir -p $(HOST_PREFIX)/build
	cd $(HOST_PREFIX)/build && \
	    CC=gcc $(TCL_SRC)/unix/configure --prefix=$(HOST_PREFIX) \
		--disable-shared && \
	    $(MAKE) -j$(NPROC) && $(MAKE) install

tcl: $(PREFIX)/lib/tclConfig.sh

$(PREFIX)/lib/tclConfig.sh: $(HOST_TCLSH)
	mkdir -p $(BUILD)/tcl
	cd $(BUILD)/tcl && \
	    $(TOOLCHAIN) CFLAGS="$(CFLAGS)" $(TCL_SRC)/unix/configure \
		--host=$(TRIPLE) --prefix=$(PREFIX) --disable-shared \
		--enable-zipfs --with-system-libtommath=no && \
	    $(MAKE) -j$(NPROC) libtcl9.1.a libtclstub.a TCLSH_PROG=$(HOST_TCLSH) && \
	    $(MAKE) install-binaries install-headers install-private-headers \
		NATIVE_TCLSH=$(HOST_TCLSH) TCLSH_PROG=$(HOST_TCLSH)

# ==== Tk ====

tk: $(BUILD)/tk/libtcl9tk9.1.a

$(BUILD)/tk/Makefile: $(PREFIX)/lib/tclConfig.sh $(PREFIX)/.cairo \
		$(TK_SRC)/unix/configure
	mkdir -p $(BUILD)/tk
	cd $(BUILD)/tk && \
	    $(TOOLCHAIN) CFLAGS="$(CFLAGS)" $(TK_SRC)/unix/configure \
		--host=$(TRIPLE) --prefix=$(PREFIX) --with-tcl=$(PREFIX)/lib \
		--disable-shared --enable-android

$(BUILD)/tk/libtcl9tk9.1.a: $(BUILD)/tk/Makefile FORCE
	$(MAKE) -C $(BUILD)/tk -j$(NPROC) libtcl9tk9.1.a libtkstub.a \
	    TCLSH_PROG=$(HOST_TCLSH)

FORCE:

# ==== The APK ====

APP_DIR		?= $(TK_SRC)/library/demos
APK_DIR		:= $(BUILD)/apk
LIBMAIN		:= $(APK_DIR)/lib/$(ANDROID_ABI)/libmain.so
SCRIPTS_ZIP	:= $(BUILD)/scripts.zip
KEYSTORE	:= $(abspath $(BUILD)/../android-debug.keystore)

# The scripts: the libraries of Tcl and Tk, and the application, whose
# main.tcl is run.
$(SCRIPTS_ZIP): FORCE $(HOST_TCLSH)
	rm -rf $(BUILD)/scripts $@
	mkdir -p $(BUILD)/scripts
	cp -r $(TCL_SRC)/library $(BUILD)/scripts/tcl_library
	rm -rf $(BUILD)/scripts/tcl_library/tzdata
	cp -r $(TK_SRC)/library $(BUILD)/scripts/tk_library
	rm -rf $(BUILD)/scripts/tk_library/demos
	cp -r $(APP_DIR) $(BUILD)/scripts/app
	cd $(BUILD)/scripts && echo 'zipfs mkzip $@ . .' | $(HOST_TCLSH)

$(BUILD)/scripts.o: $(SCRIPTS_ZIP)
	printf '%s\n' '.section .rodata' '.p2align 4' \
	    '.globl tkAndroidScripts' 'tkAndroidScripts:' \
	    '.incbin "$(SCRIPTS_ZIP)"' '.globl tkAndroidScriptsEnd' \
	    'tkAndroidScriptsEnd:' > $(BUILD)/scripts.s
	$(CC) -c $(BUILD)/scripts.s -o $@

$(BUILD)/tkAndroidMain.o: $(TK_SRC)/android/tkAndroidMain.c FORCE
	$(CC) $(CFLAGS) -I$(PREFIX)/include -I$(TCL_SRC)/generic \
	    -I$(TK_SRC)/generic -I$(TK_SRC)/xlib -I$(TK_SRC)/android -c $< -o $@

$(LIBMAIN): $(BUILD)/tkAndroidMain.o $(BUILD)/scripts.o \
		$(BUILD)/tk/libtcl9tk9.1.a
	mkdir -p $(dir $@)
	$(CC) -shared -o $@ -Wl,--gc-sections -Wl,-z,max-page-size=16384 \
	    -Wl,--exclude-libs,ALL -u ANativeActivity_onCreate \
	    $(BUILD)/tkAndroidMain.o $(BUILD)/scripts.o \
	    $(BUILD)/tk/libtcl9tk9.1.a $(BUILD)/tcl/libtcl9.1.a \
	    $$(pkg-config --static --libs cairo fontconfig freetype2 pixman-1) \
	    -landroid -llog -lz -lm

$(KEYSTORE):
	keytool -genkeypair -keystore $@ -storepass android -keypass android \
	    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
	    -dname "CN=Android Debug,O=Android,C=US"

# The Java side of the activity.
JAVA_SRC	:= $(wildcard $(TK_SRC)/android/java/tk/android/*.java)

$(BUILD)/classes.dex: $(JAVA_SRC)
	rm -rf $(BUILD)/classes $@
	mkdir -p $(BUILD)/classes
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
	    -bootclasspath $(PLATFORM_JAR) -d $(BUILD)/classes $(JAVA_SRC)
	$(BUILD_TOOLS)/d8 --min-api $(ANDROID_API) --lib $(PLATFORM_JAR) \
	    --output $(BUILD) $$(find $(BUILD)/classes -name '*.class')

apk: $(BUILD)/tk.apk

$(BUILD)/tk.apk: $(LIBMAIN) $(TK_SRC)/android/AndroidManifest.xml \
		$(wildcard $(TK_SRC)/android/res/*/*) $(BUILD)/classes.dex \
		$(KEYSTORE)
	rm -f $(BUILD)/base.apk $(BUILD)/aligned.apk $(BUILD)/res.zip $@
	$(BUILD_TOOLS)/aapt2 compile --dir $(TK_SRC)/android/res \
	    -o $(BUILD)/res.zip
	$(BUILD_TOOLS)/aapt2 link -o $(BUILD)/base.apk \
	    --manifest $(TK_SRC)/android/AndroidManifest.xml $(BUILD)/res.zip \
	    -I $(PLATFORM_JAR) --min-sdk-version $(ANDROID_API) \
	    --target-sdk-version 35
	cd $(APK_DIR) && zip -0 -r $(BUILD)/base.apk lib
	cd $(BUILD) && zip $(BUILD)/base.apk classes.dex
	$(BUILD_TOOLS)/zipalign -P 16 -f 4 $(BUILD)/base.apk $(BUILD)/aligned.apk
	$(BUILD_TOOLS)/apksigner sign --ks $(KEYSTORE) --ks-pass pass:android \
	    --out $@ $(BUILD)/aligned.apk

clean:
	rm -rf $(BUILD)
