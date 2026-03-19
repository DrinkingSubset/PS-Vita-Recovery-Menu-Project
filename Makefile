PREFIX   = arm-vita-eabi
CC       = $(PREFIX)-gcc

APP_NAME = VitaRecovery
TITLE_ID = RECM00001

SOURCES  = source/main.c \
           source/display.c \
           source/menu.c \
           source/draw.c \
           source/compat.c \
           source/plugins.c \
           source/cpu.c \
           source/sysinfo.c \
           source/registry.c \
           source/restore.c \
           source/restore_screen.c \
           source/official_recovery.c \
           source/sd2vita.c \
           source/cheat_manager.c

LIBS     = -lSceCtrl_stub \
           -lSceLibKernel_stub \
           -lSceProcessmgr_stub \
           -lSceDisplay_stub \
           -lScePower_stub \
           -lSceIofilemgr_stub \
           -lSceSysmem_stub \
           -lSceAppMgr_stub \
           -lSceRegistryMgr_stub \
           -lSceSysmodule_stub \
           -lScePromoterUtil_stub \
           -lm \
           -lc

CFLAGS   = -Wl,-q -Wall -O2 -Isource
LDFLAGS  = $(LIBS)

all: $(APP_NAME).vpk

$(APP_NAME).elf: $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

$(APP_NAME).velf: $(APP_NAME).elf
	vita-make-fself $< $@

$(APP_NAME).sfo:
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) -s APP_VER=01.00 -s CATEGORY=gd -s ATTRIBUTE2=12 "$(APP_NAME)" $@

$(APP_NAME).vpk: $(APP_NAME).velf $(APP_NAME).sfo
	vita-pack-vpk -s $(APP_NAME).sfo -b $(APP_NAME).velf \
		-a sce_sys/icon0.png=sce_sys/icon0.png \
		-a sce_sys/param.sfo=sce_sys/param.sfo $@

clean:
	rm -f *.elf *.velf *.vpk *.sfo
