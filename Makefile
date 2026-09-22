TARGET = PSPBox
OBJS = main.o

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspdebug -lpspge -lpspdisplay -lpspctrl -lpspsdk

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPBox DJ

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
