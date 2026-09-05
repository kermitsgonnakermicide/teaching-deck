# PSP Teaching Deck - Robot Controller
# Requires PSPSDK installed

TARGET = teaching-deck
OBJS = main.o network.o input.o gfx.o video.o

INCDIR = 
CFLAGS = -G0 -O2 -Wall -g
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 0
PSP_FW_VERSION = 371

LIBDIR = 
LDFLAGS = 
LIBS = -lpspnet -lpspnet_apctl -lpspnet_inet -lpspnet_resolver \
       -lpspwlan -lpsputility -lpspctrl -lpspsdk -lpsprtc -lpsppower -lpspdebug \
       -lpspgu -lpspge

# PSPDEV/PSPSDK auto-detection. Override on the command line if installed elsewhere:
#   make PSPDEV=/path/to/pspdev
PSPDEV ?= $(firstword $(wildcard $(HOME)/pspdev /home/daksh/pspdev /opt/pspdev))
PSPSDK ?= $(PSPDEV)/psp/sdk
export PSPSDK PSPDEV
PATH := $(PSPDEV)/bin:$(PATH)
export PATH

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Teaching Deck

include $(PSPSDK)/lib/build.mak
