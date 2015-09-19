include  buildconfig.mk

# Output directors to store intermediate compiled files
# relative to the project directory
BUILD_BASE	= build
FW_BASE		= firmware

# name for the target project
TARGET		= zmote-firmware

# which modules (subdirectories) of the project to include in compiling
MODULES		= user
EXTRA_INCDIR	= include libesphttpd/include libmqtt/mqtt/include rboot/rboot

#Add in esphttpd and mqtt libs
EXTRA_LIBS = esphttpd mqtt


# libraries used in this project, mainly provided by the SDK
LIBS		= c gcc hal phy pp net80211 wpa main lwip $(EXTRA_LIBS)
# compiler flags using during compilation of source files
CFLAGS		+= -Os -ggdb -Werror -Wpointer-arith -Wundef -Wall -Wl,-EL -fno-inline-functions \
		-nostdlib -mlongcalls -mtext-section-literals  -D__ets__ -DICACHE_FLASH -D_STDINT_H \
		-Wno-address

# linker flags used to generate the main object file
LDFLAGS		= -nostdlib -Wl,--no-check-sections -u call_user_start -Wl,-static $(patsubst %, -Llib%, $(EXTRA_LIBS))

# various paths from the SDK used in this project
SDK_LIBDIR	= lib
SDK_LDDIR	= ld
SDK_INCDIR	= include include/json

# select which tools to use as compiler, librarian and linker
CC		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-gcc
AR		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-ar
LD		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-gcc


####
#### no user configurable options below here
####
SRC_DIR		:= $(MODULES)
BUILD_DIR	:= $(addprefix $(BUILD_BASE)/,$(MODULES))

SDK_LIBDIR	:= $(addprefix $(SDK_BASE)/,$(SDK_LIBDIR))
SDK_LDDIR	:= $(addprefix $(SDK_BASE)/,$(SDK_LDDIR))
SDK_INCDIR	:= $(addprefix -I$(SDK_BASE)/,$(SDK_INCDIR))

SRC		:= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
OBJ		:= $(patsubst %.c,$(BUILD_BASE)/%.o,$(SRC))
LIBS	:= $(addprefix -l,$(LIBS))
APP_AR	:= $(addprefix $(BUILD_BASE)/,$(TARGET)_app.a)
ROM0	:= $(addprefix $(FW_BASE)/,rom0.bin)
ROM1	:= $(addprefix $(FW_BASE)/,rom1.bin)
FS_TAB	:= $(addprefix $(FW_BASE)/,$(TARGET)_fs.json)

INCDIR	:= $(addprefix -I,$(abspath $(SRC_DIR)))
EXTRA_INCDIR	:= $(addprefix -I,$(abspath $(EXTRA_INCDIR)))
MODULE_INCDIR	:= $(addsuffix /include,$(INCDIR))

V ?= $(VERBOSE)
ifeq ("$(V)","1")
Q :=
vecho := @true
else
Q := @
vecho := @echo
endif

ifeq ("$(ENABLE_WPS)","yes")
CFLAGS		+= -DENABLE_WPS
LIBS +=  wps crypto
endif

ifeq ("$(SHOW_HEAP_USE)","yes")
CFLAGS		+= -DSHOW_HEAP_USE
endif

vpath %.c $(SRC_DIR)

define compile-objects
$1/%.o: %.c
	$(vecho) "CC $$<"
	$(Q) $(CC) $(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS)  -c $$< -o $$@
endef

.PHONY: all checkdirs clean libesphttpd libmqtt rboot roms

all: checkdirs $(ESPTOOL2) rboot $(ROM0) $(ROM1) $(FS_TAB) name_binaries
roms: checkdirs $(ESPTOOL2) $(ROM0) $(ROM1)

libesphttpd/Makefile libmqtt/Makefile rboot/Makefile:
	$(Q) echo "Fetching submodules..."
	$(Q) git submodule init
	$(Q) git submodule update

libesphttpd: libesphttpd/Makefile
	$(Q) make -C libesphttpd lib

libmqtt: libmqtt/Makefile
	$(Q) make -C libmqtt

rboot: rboot/Makefile $(ESPTOOL2)
	$(Q) PATH="$(PATH):$(XTENSA_TOOLS_ROOT)" RBOOT_FW_BASE=$(abspath $(FW_BASE)) \
		ESPTOOL2=$(ESPTOOL2) make -C rboot/rboot \
		CFLAGS="$(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS)" \
		XTENSA_BINDIR=$(XTENSA_TOOLS_ROOT)

$(ESPTOOL2): rboot/Makefile
	$(Q) make -C rboot esptool2

$(BUILD_DIR)/%.elf: $(APP_AR)
	$(vecho) "LD $@"
	$(Q) $(LD) -L$(SDK_LIBDIR) -L$(SDK_LDDIR) -Trboot/rboot-sampleproject/$(notdir $(basename $@)).ld $(LDFLAGS) -Wl,--start-group $(LIBS) $^ -Wl,--end-group -o $@

$(FW_BASE)/%.bin: $(BUILD_DIR)/%.elf
	@echo "FW $(notdir $@)"
	@$(ESPTOOL2) $(FW_USER_ARGS) $^ $@ $(FW_SECTS)

$(APP_AR):  $(patsubst %, lib%, $(EXTRA_LIBS)) $(OBJ)
	$(vecho) "AR $@"
	$(Q) $(AR) cru $@ $(OBJ)

checkdirs: $(BUILD_DIR) $(FW_BASE)

$(BUILD_DIR):
	$(Q) mkdir -p $@

$(FW_BASE):
	$(Q) mkdir -p $@

fs: $(FS_TAB)

zfs/node_modules: zfs/package.json
	cd zfs && npm install
frontend/node_modules: frontend/package.json
	cd frontend && npm install

$(FS_TAB): $(FW_BASE) zfs/node_modules  frontend/node_modules force
	$(Q) mkdir -p $(WEB_DIR) && cd frontend && gulp widget-files
	$(Q) node zfs/zfs.js --outdir=$(FW_BASE) --fstab=$@ $(WEB_DIR)

name_binaries: force
	$(Q) cp $(FW_BASE)/rboot.bin $(FW_BASE)/0x00000.bin && \
		cp $(ROM0) $(FW_BASE)/0x02000.bin && \
		cp $(ROM1) $(FW_BASE)/0x82000.bin && \
		cp $(SDK_BASE)/bin/esp_init_data_default.bin $(FW_BASE)/0xFC000.bin && \
		cp $(SDK_BASE)/bin/blank.bin $(FW_BASE)/0xFE000.bin

clean:
	$(Q) make -C libesphttpd clean
	$(Q) make -C libmqtt clean
	$(Q) make -C rboot clean
	$(Q) find $(BUILD_BASE) -type f | xargs rm -f
	$(Q) rm -rf $(FW_BASE)
	$(Q) rm -rf $(WEB_DIR)

force:
	@true

$(foreach bdir,$(BUILD_DIR),$(eval $(call compile-objects,$(bdir))))
