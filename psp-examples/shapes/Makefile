TARGET = shapes
OBJS = main.o

PSPDEV ?= $(firstword $(wildcard $(HOME)/pspdev /home/daksh/pspdev /opt/pspdev))
PSPSDK ?= $(PSPDEV)/psp/sdk
export PSPSDK PSPDEV
PATH := $(PSPDEV)/bin:$(PATH)
export PATH

CFLAGS = -G0 -O2 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti

BUILD_PRX = 0
PSP_FW_VERSION = 371

LIBS = -lpspgu -lpspge -lpspdisplay

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Drawing Shapes

include $(PSPSDK)/lib/build.mak