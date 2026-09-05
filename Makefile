#---------------------------------------------------------------------------------
# Genshin Impact Android compatibility loader for Nintendo Switch/libnx.
#
# Graphics is intentionally Vulkan-only.  The Mesa/NVK SDK must not be linked
# with devkitPro's older switch-mesa EGL/GLES stack.
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
APP_VERSION := 7.0.1
APP_ICON    := $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON

SOURCES  := source
INCLUDES := source

# The original Mesa 26.1.5 SDK remains the default.  NVK_SWITCH_ROOT supports a
# public, self-contained SDK containing include/vulkan and lib/libvulkan.a.
# The public backend is experimental because Mesa does not consider NVK
# conformant on the Switch's Maxwell GPU.
NVK_SWITCH_SDK  ?= $(TOPDIR)/vendor/mesa-26.1.5-switch-vulkan-sdk
NVK_SWITCH_ROOT ?=
VULKAN_HEADERS_ROOT ?=
ifneq ($(strip $(NVK_SWITCH_ROOT)),)
ifeq ($(wildcard $(NVK_SWITCH_ROOT)/include/vulkan/vulkan.h),)
$(error "Missing Vulkan headers below NVK_SWITCH_ROOT=$(NVK_SWITCH_ROOT)")
endif
ifeq ($(wildcard $(NVK_SWITCH_ROOT)/include/vulkan/vulkan_vi.h),)
$(error "Missing Switch VI Vulkan header below NVK_SWITCH_ROOT=$(NVK_SWITCH_ROOT)")
endif
ifeq ($(wildcard $(NVK_SWITCH_ROOT)/lib/libvulkan.a),)
$(error "Missing lib/libvulkan.a below NVK_SWITCH_ROOT=$(NVK_SWITCH_ROOT)")
endif
ifeq ($(wildcard $(NVK_SWITCH_ROOT)/include/vk_video/vulkan_video_codecs_common.h),)
ifeq ($(strip $(VULKAN_HEADERS_ROOT)),)
$(error "The external SDK omits include/vk_video. Set VULKAN_HEADERS_ROOT to matching Khronos Vulkan-Headers")
endif
ifeq ($(wildcard $(VULKAN_HEADERS_ROOT)/include/vk_video/vulkan_video_codecs_common.h),)
$(error "Missing include/vk_video below VULKAN_HEADERS_ROOT=$(VULKAN_HEADERS_ROOT)")
endif
NVK_SWITCH_CPPFLAGS := -I$(VULKAN_HEADERS_ROOT)/include
endif
NVK_SWITCH_CPPFLAGS += -I$(NVK_SWITCH_ROOT)/include -DVK_USE_PLATFORM_VI_NN
NVK_SWITCH_CPPFLAGS += -DGENSHIN_EXTERNAL_LOADERLESS_NVK
NVK_SWITCH_LDFLAGS  :=
NVK_SWITCH_LIBS     := -L$(NVK_SWITCH_ROOT)/lib -lvulkan -lz -lzstd -lnx -lstdc++ -lm
LOADERLESS_VULKAN_ARCHIVE := $(NVK_SWITCH_ROOT)/lib/libvulkan.a
BUILD               := build_nx_external
else
ifeq ($(wildcard $(NVK_SWITCH_SDK)/share/nvk-switch/nvk-switch.mk),)
$(error "Missing Mesa/NVK SDK. Set NVK_SWITCH_ROOT to a public SDK prefix or provide $(NVK_SWITCH_SDK)")
endif
include $(NVK_SWITCH_SDK)/share/nvk-switch/nvk-switch.mk
LOADERLESS_VULKAN_ARCHIVE := $(NVK_SWITCH_SDK)/lib/libvulkan.a
BUILD := build_nx
endif

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
            -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=open_memstream \
            -Wl,--wrap=_sbrk_r \
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

.PHONY: all clean preflight check-loaderless-abi
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

check-loaderless-abi: all
	@sh tools/check_loaderless_abi.sh \
		"$(LOADERLESS_VULKAN_ARCHIVE)" \
		"$(BUILD)/vulkan_bridge.o" \
		"$(TARGET).elf"

preflight:
	@printf '%s\n' "DEVKITPRO=$(DEVKITPRO)" "NVK_SWITCH_ROOT=$(NVK_SWITCH_ROOT)" "VULKAN_HEADERS_ROOT=$(VULKAN_HEADERS_ROOT)" "NVK_SWITCH_SDK=$(NVK_SWITCH_SDK)"
	@test -f "$(DEVKITPRO)/libnx/switch_rules"
	@test -f "$(PORTLIBS)/include/SDL2/SDL.h"
	@test -f "$(PORTLIBS)/include/curl/curl.h"
	@printf '%s\n' "Native build dependencies are present."

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr build_nx build_nx_external $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)
endif
