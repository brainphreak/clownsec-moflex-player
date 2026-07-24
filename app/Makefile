ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM (source /opt/devkitpro/devkitvars.sh or export it)")
endif
export PREFIX:=arm-none-eabi-
export CC:=$(PREFIX)gcc
export CXX:=$(PREFIX)g++
export AS:=$(PREFIX)as
export AR:=$(PREFIX)ar
export OBJCOPY:=$(PREFIX)objcopy
export LD:=$(PREFIX)gcc
include $(DEVKITARM)/3ds_rules

TARGET   := clownsec_player
BUILD    := build
# our decoder + the unmodified FFmpeg support files + the app itself
SOURCES  := source ../decoder ../ffmpeg_support ../playback ../net ../ui \
	../thirdparty/cJSON ../thirdparty/zip ../thirdparty/helixaac
FF_INC   := ../ffmpeg_support/include

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
BUILD_TAG ?= $(shell ./buildnum.sh)
export BUILD_TAG
CFLAGS  := -w -O3 -mword-relocations -ffunction-sections -D__3DS__ -D_GNU_SOURCE \
	-DBUILD_TAG=\"$(BUILD_TAG)\" \
	-Wno-error=implicit-function-declaration $(ARCH) $(INCLUDE)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export SFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export OFILES  := $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD) \
	-I$(CURDIR)/$(FF_INC) -I$(CURDIR)/$(FF_INC)/libavcodec -I$(CURDIR)/../decoder \
	-I$(CURDIR)/../playback -I$(CURDIR)/../net -I$(CURDIR)/../ui \
	-I$(CURDIR)/../thirdparty/cJSON -I$(CURDIR)/../thirdparty/zip \
	-I$(CURDIR)/../thirdparty/stb -I$(CURDIR)/../thirdparty/helixaac
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_TITLE       := CLOWNSEC VIDEO
export APP_DESCRIPTION := MOFLEX VIDEO PLAYER
export APP_AUTHOR      := brAinphreAk
export APP_ICON        := $(TOPDIR)/icon.png
export _3DSXFLAGS      += --smdh=$(OUTPUT).smdh

.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).map $(TARGET).smdh
else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
