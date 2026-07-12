ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM")
endif
export PREFIX:=arm-none-eabi-
export CC:=$(PREFIX)gcc
export CXX:=$(PREFIX)g++
export AS:=$(PREFIX)as
export AR:=$(PREFIX)ar
export OBJCOPY:=$(PREFIX)objcopy
export LD:=$(PREFIX)gcc
include $(DEVKITARM)/3ds_rules

TARGET   := moflex_gputest
BUILD    := build
SOURCES  := source ../decoder ../ffmpeg_support
FF_INC   := ../ffmpeg_support/include

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -w -O3 -mword-relocations -munaligned-access -ffunction-sections -D__3DS__ -D_GNU_SOURCE \
	-Wno-error=implicit-function-declaration $(ARCH) $(INCLUDE)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export CFILES  := $(filter-out mp4_%.c cia_moflex.c,$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
export SFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export OFILES  := $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD) \
	-I$(CURDIR)/$(FF_INC) -I$(CURDIR)/$(FF_INC)/libavcodec -I$(CURDIR)/../decoder
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_TITLE       := MoFlex GPU test
export APP_DESCRIPTION := citro2d GPU render test
export APP_AUTHOR      := brAinphreAk

.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).map
else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).3dsx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
