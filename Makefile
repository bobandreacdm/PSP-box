TARGET = PSPBox
OBJS = main.o

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspdebug -lpspge -lpspdisplay -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPBox DJ v1.9.2

PSPSDK=$(shell psp-config --psp-dev-kit-dir)
include $(PSPSDK)/lib/build.mak
