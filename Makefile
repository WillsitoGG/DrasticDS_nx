#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
APP_TITLE	:=	Drastic DS
APP_AUTHOR	:=	naga
APP_VERSION	:=	1.0.4
BUILD		:=	build
SOURCES		:=	source source/hooks source/switch
DATA		:=	data
INCLUDES	:=	source source/switch
DFX_GENERATED ?=
ifneq ($(strip $(DFX_GENERATED)),)
DATA		+=	$(DFX_GENERATED)/data
INCLUDES	+=	$(DFX_GENERATED)/include
endif
STORAGE_BUILD ?= $(TOPDIR)/launcher/dependencies/build
LIBSMB2_INCLUDE ?= $(STORAGE_BUILD)/_deps/libsmb2-src/include
LIBUSBHSFS_INCLUDE ?= $(STORAGE_BUILD)/_deps/libusbhsfs-src/include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
OPTIMIZATION := -O3 -flto=auto

# __SWITCH__ for libnx; DRASTIC_NX gates the port-specific host branches.
DEFINES	:=	-D__SWITCH__ -DDRASTIC_NX -DDRASTIC_NX_VERSION='"$(APP_VERSION)"'
ifneq ($(strip $(DFX_GENERATED)),)
DEFINES	+=	-DDRASTIC_DFX_GENERATED
endif

# --- renderer select: GL (default) or VK (Mesa NVK) ------------------------
# GL and Vulkan are mutually exclusive (switch-mesa's libEGL/GLES and the NVK
# archives both bundle mesa util/nir/compiler object code -> can't co-link).
#   make             -> OpenGL (switch-mesa GLES, unchanged)
#   make RENDERER=VK -> Vulkan (Mesa NVK, vendored flat under vulkan/)
RENDERER ?= GL
ifeq ($(RENDERER),VK)
DEFINES	+=	-DUSE_VULKAN -DVK_USE_PLATFORM_VI_NN
VULKAN_STAGE ?= $(TOPDIR)/vulkan
SOURCES	+=	source/lsfg \
			third_party/lsfg-vk/lsfg-vk-common/src/helpers \
			third_party/lsfg-vk/lsfg-vk-common/src/vulkan \
			third_party/lsfg-vk/lsfg-vk-backend/src \
			third_party/lsfg-vk/lsfg-vk-backend/src/extraction \
			third_party/lsfg-vk/lsfg-vk-backend/src/helpers \
			third_party/lsfg-vk/lsfg-vk-backend/src/shaderchains
INCLUDES	+=	source/lsfg \
			third_party/vulkan-headers/include \
			third_party/lsfg-vk/lsfg-vk-common/include \
			third_party/lsfg-vk/lsfg-vk-backend/include \
			third_party/lsfg-vk/lsfg-vk-backend/src
else
DEFINES	+=	-DUSE_OPENGL
endif

CFLAGS	:=	-Wall -Wextra $(OPTIMIZATION) -DNDEBUG -ffunction-sections -fdata-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. \
			-fmacro-prefix-map=$(CURDIR)=. $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -Wno-missing-field-initializers
ifeq ($(RENDERER),VK)
CXXFLAGS	+= -std=gnu++20
endif

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) $(OPTIMIZATION) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=sha1

STORAGE_LIBS := $(STORAGE_BUILD)/_deps/libsmb2-build/lib/libsmb2.a \
				$(STORAGE_BUILD)/_deps/libusbhsfs-build/liblibusbhsfs.a

# nx supplies audren, HID, applet, and filesystem services. Drastic's OpenSL ES
# ABI is implemented directly by the audren-backed source/opensles.c layer.
ifeq ($(RENDERER),VK)
# Mesa NVK: 23 vendored static archives (vulkan/lib) linked in one --start-group
# (circular NVK<->runtime<->nir<->compiler deps). -l:libX.a links by exact file
# name (avoids the -lvulkan GROUP-script + the double-prefixed liblibnil...a).
# -lz/-lzstd resolve crc32/ZSTD_*; the DRM/nouveau_ws path is dead-stripped so no
# -ldrm_nouveau. -lstdc++/libgcc unwinder are needed by NAK's bundled Rust.
LIBDIRS	:= $(VULKAN_STAGE) $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,--start-group \
		-l:libnvk.a -l:libvulkan_lite_runtime.a -l:libvulkan_runtime.a \
		-l:libvulkan_lite_instance.a -l:libvulkan_instance.a \
		-l:libvulkan_util.a -l:libvulkan_wsi.a \
		-l:libnak.a -l:libnak_rs.a -l:libvtn.a -l:libxmlconfig.a \
		-l:libnil.a -l:liblibnil_format_table.a -l:libnouveau_mme.a \
		-l:libnouveau_ws.a -l:libnvidia_headers_c.a \
		-l:libnir.a -l:libcompiler.a -l:libcompiler_c_helpers.a \
		-l:libmesa_util.a -l:libmesa_util_simd.a -l:libblake3.a -l:libmesa_util_c11.a \
		-Wl,--end-group $(STORAGE_LIBS) -lz -lzstd -lnx -lstdc++ -lm
else
# EGL/GLESv2/glapi/drm_nouveau: switch-mesa/nouveau GL.
LIBDIRS	:= $(PORTLIBS) $(LIBNX)
LIBS	:= $(STORAGE_LIBS) -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lz -lnx -lstdc++ -lm
endif

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

absolute_or_local = $(if $(filter /%,$(1)),$(1),$(CURDIR)/$(1))
export VPATH	:=	$(foreach dir,$(SOURCES),$(call absolute_or_local,$(dir))) \
			$(foreach dir,$(DATA),$(call absolute_or_local,$(dir)))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# link with g++ so mesa's C++ EGL/GLES pulls in libstdc++
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(call absolute_or_local,$(dir))) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(LIBSMB2_INCLUDE) -I$(LIBUSBHSFS_INCLUDE) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf \
		$(TARGET)_gl.nro $(TARGET)_gl.elf $(TARGET)_gl.map \
		$(TARGET)_vk.nro $(TARGET)_vk.elf $(TARGET)_vk.map \
		DrasticDS.nro vulkan
	@rm -f *.o

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
