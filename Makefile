# Path to the Rack SDK (must not contain spaces). Override with: make RACK_DIR=...
RACK_DIR ?= C:/Rack-SDK

# Where `make install` puts the plugin. LOCALAPPDATA is not passed into MSYS2 login shells.
LOCALAPPDATA ?= $(shell cygpath -m -F 28)

# Workaround: an old zlib1.dll in C:\Windows\System32 breaks the as.exe/ld.exe that gcc
# invokes from mingw64/x86_64-w64-mingw32/bin. -B makes gcc use the copies in mingw64/bin instead.
MINGW_BIN ?= C:/msys64/mingw64/bin/
FLAGS += -B $(MINGW_BIN)
LDFLAGS += -B $(MINGW_BIN)

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
