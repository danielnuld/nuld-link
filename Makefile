# make       -> link.elf (ORBIT Link: the PS2 as a streaming client of the PC)
# make test  -> host selftests of the shared pieces (gfx, ini, net, mpegps)
EE_BIN = link.elf
IRX_FILES = iomanX fileXio sio2man mcman mcserv freepad libsd audsrv bdm bdmfs_fatfs usbd_mini usbmass_bd_mini ps2dev9 netman smap
EE_OBJS = link.o iop.o gfx.o font_data.o ui_data.o ini.o net.o http.o mpegps.o $(IRX_FILES:=_irx.o)
EE_LIBS = -L$(PS2SDK)/ports/lib -lwolfssl -lnetman -lps2ip -Xlinker --wrap=open -Xlinker --wrap=read -lmpeg -lmad -laudsrv \
	-lcdvd -lmc -lfont -lpacket -ldma -lgraph -ldraw -lpad -lfileXio -lpatches -lc
EE_INCS += -I$(PS2SDK)/ports/include # wolfssl (net.c), libmad (link.c): ps2sdk ports

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

test:
	cc -std=c99 -Wall -DSELFTEST gfx.c font_data.c -o /tmp/gfx_selftest && /tmp/gfx_selftest
	cc -std=gnu99 -Wall -DSELFTEST ini.c -o /tmp/ini_selftest && /tmp/ini_selftest
	cc -std=gnu99 -Wall -DSELFTEST net.c -o /tmp/net_selftest && /tmp/net_selftest
	cc -std=gnu99 -Wall -DSELFTEST mpegps.c -o /tmp/ps_selftest && /tmp/ps_selftest $(PS_CHECK)

clean:
	rm -f *.elf *.o *_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
