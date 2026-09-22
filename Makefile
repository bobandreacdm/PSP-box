TARGET = PSPBox
OBJS = main.o

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspdebug -lpspge -lpspdisplay -lpspctrl -lpspsdk

# Abilita la struttura PRX obbligatoria per CFW
BUILD_PRX = 1
PSP_FW_VERSION = 500

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPBox DJ v1.9.3

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
