# --------------- esphttpd config options ---------------

ZMOTE_FIRMWARE_VERSION = "\"0.3.1\""

ESP_DEV_HOME = $(abspath $(PWD)/../zmote-toolchain)

ESPTOOL2      ?= $(PWD)/rboot/esptool2/esptool2.exe
FW_SECTS      = .text .data .rodata
FW_USER_ARGS  = -quiet -bin -boot2

SDK_BASE      ?= $(ESP_DEV_HOME)/esp_iot_sdk_v1.3.0

XTENSA_TOOLS_ROOT ?= $(ESP_DEV_HOME)/xtensa-lx106-elf/bin

ESPTOOL ?= $(ESP_DEV_HOME)/bin/esptool.py
ESPPORT ?= COM5
ESPBAUD ?= 921600

PYTHON ?= /c/Python27/python.exe

# If GZIP_COMPRESSION is set to "yes" then the static css, js, and html files will be compressed with gzip before added to the espfs image
# and will be served with gzip Content-Encoding header.
# This could speed up the downloading of these files, but might break compatibility with older web browsers not supporting gzip encoding
# because Accept-Encoding is simply ignored. Enable this option if you have large static files to serve (for e.g. JQuery, Twitter bootstrap)
# By default only js, css and html files are compressed.
# If you have text based static files with different extensions what you want to serve compressed then you will need to add the extension to the following places:
# - Add the extension to this Makefile at the webpages.espfs target to the find command
# - Add the extension to the gzippedFileTypes array in the user/httpd.c file
#
# Adding JPG or PNG files (and any other compressed formats) is not recommended, because GZIP compression does not works effectively on compressed files.

#Static gzipping is disabled by default.
GZIP_COMPRESSION ?= yes

# If COMPRESS_W_YUI is set to "yes" then the static css and js files will be compressed with yui-compressor
# This option works only when GZIP_COMPRESSION is set to "yes"
# http://yui.github.io/yuicompressor/
#Disabled by default.
COMPRESS_W_YUI ?= no
YUI-COMPRESSOR ?= /usr/bin/yui-compressor

#If USE_HEATSHRINK is set to "yes" then the espfs files will be compressed with Heatshrink and decompressed
#on the fly while reading the file. Because the decompression is done in the esp8266, it does not require
#any support in the browser.
USE_HEATSHRINK ?= no

SHOW_HEAP_USE ?= yes

WEB_DIR ?= ./html

CFLAGS = -DZMOTE_CFG_SECTOR=0x80 -DUSE_US_TIMER -DZMOTE_FIRMWARE_VERSION=$(ZMOTE_FIRMWARE_VERSION) -DZMOTE_FIRMWARE_COMMIT="\"$(shell git log | head -1 | awk '{print $$2}')\"" 

ifeq ($(BUILD), nodemcu)
	CFLAGS += -DINVERT_IR_TX -DENABLE_UART_DEBUG -DZMOTE_FIRMWARE_BUILD=\"nodemcu\"
else
	CFLAGS += -DZMOTE_FIRMWARE_BUILD=\"v1hw\"  -DUART_TX_AS_STLED
endif

ZLIB_CFLAGS = -I$(PWD)/../../zmote-toolchain/zlib/include
ZLIB_LFLAGS = -L$(PWD)/../../zmote-toolchain/zlib/lib -lz

