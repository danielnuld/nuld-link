# make       -> link.elf (NULD Link: the PS2 as a streaming client of the PC)
# make test  -> host selftests of the shared pieces (gfx, ini, net)
#
# nuld-av (github.com/danielnuld/nuld-av), built, beside this folder: the player, its netring.irx and, for
# server/link.py, its tools
NULDAV ?= ../nuld-av
EE_BIN = link.elf
IRX_FILES = iomanX fileXio sio2man mcman mcserv freepad libsd audsrv bdm bdmfs_fatfs usbd_mini usbmass_bd_mini ps2dev9 netman \
	smap ps2ip_nm ps2ips netring
EE_OBJS = link.o splash.o logo_data.o iop.o gfx.o font_data.o ui_data.o ini.o net.o $(IRX_FILES:=_irx.o)
EE_LIBS = -L$(NULDAV) -lnuldav -L$(PS2SDK)/ports/lib -lnetman -lps2ips -lmad -laudsrv \
	-lcdvd -lmc -lfont -lpacket -ldma -lgraph -ldraw -lpad -lfileXio -lpatches -lc
EE_INCS += -I$(NULDAV)/include -I$(PS2SDK)/ports/include # libmad (link.c): ps2sdk ports

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

link.elf: $(NULDAV)/libnuldav.a

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx
ps2ip_nm_irx.c: # the file has a dash, a C name can't
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/ps2ip-nm.irx $@ ps2ip_nm_irx
smap_irx.c: # the modular build: netring.irx hooks its receive path
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/smap-modular-netman.irx $@ smap_irx
netring_irx.c: $(NULDAV)/iop/netring/netring.irx
	$(PS2SDK)/bin/bin2c $< $@ netring_irx

test:
	cc -std=c99 -Wall -DSELFTEST gfx.c font_data.c -o /tmp/gfx_selftest && /tmp/gfx_selftest
	cc -std=gnu99 -Wall -DSELFTEST ini.c -o /tmp/ini_selftest && /tmp/ini_selftest
	cc -std=gnu99 -Wall -DSELFTEST net.c -o /tmp/net_selftest && /tmp/net_selftest

clean:
	rm -f *.elf *.o *_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
