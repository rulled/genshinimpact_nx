#---------------------------------------------------------------------------------
# Genshin Impact Android compatibility loader for Nintendo Switch/libnx.
#
# Graphics is intentionally Vulkan-only.  The vendored Mesa 26.1.5/NVK SDK is a
# loaderless static driver and must not be linked with devkitPro's older
# switch-mesa EGL/GLES stack.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := genshinimpact_nx
APP_TITLE   := Genshin Impact NX
APP_AUTHOR  := naga
APP_VERSION := 6.7.0
APP_ICON    := $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON

BUILD    := build_nx
SOURCES  := source
INCLUDES := source

# Keep this SDK self-contained.  libvulkan.a is a GNU ld GROUP script over its
# private archives, so the complete lib directory has to remain together.
NVK_SWITCH_SDK := $(TOPDIR)/vendor/mesa-26.1.5-switch-vulkan-sdk
ifeq ($(wildcard $(NVK_SWITCH_SDK)/share/nvk-switch/nvk-switch.mk),)
$(error "Missing vendored Mesa/NVK SDK at $(NVK_SWITCH_SDK)")
endif
include $(NVK_SWITCH_SDK)/share/nvk-switch/nvk-switch.mk

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# VK_USE_PLATFORM_ANDROID_KHR exposes the Android WSI types consumed by the
# guest bridge; VK_USE_PLATFORM_VI_NN comes from NVK_SWITCH_CPPFLAGS.
CFLAGS   := -Wall -O2 -DNDEBUG -ffunction-sections -fdata-sections $(ARCH) \
            $(DEFINES) $(INCLUDE) $(NVK_SWITCH_CPPFLAGS) \
            -D__SWITCH__ -DVULKAN_ONLY -DVK_USE_PLATFORM_ANDROID_KHR
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) \
            -Wl,--gc-sections \
            -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc \
            -Wl,--wrap=free -Wl,--wrap=memalign -Wl,--wrap=aligned_alloc \
            -Wl,--wrap=posix_memalign -Wl,--wrap=malloc_usable_size \
            $(NVK_SWITCH_LDFLAGS)

# Do not add -lGLESv2, -lEGL, -lglapi, or -ldrm_nouveau here.  NVK contains the
# Vulkan WSI implementation and compatibility-only EGL symbols required by the
# guest import table.  SDL2 supplies controller/audio integration; zlib/zstd and
# the remaining runtime libraries are declared by NVK_SWITCH_LIBS.
LIBS := -lSDL2 -lcurl $(NVK_SWITCH_LIBS)

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)
endif
