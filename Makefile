# naomi-diag — diagnostic BIOS ROM for SEGA Naomi
# Toolchain: Debian gcc-sh-elf / binutils-sh-elf

CROSS   ?= sh-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

# QUICK=1 : only 1MB per RAM pass (fast emulator bring-up)
QUICK   ?= 0

CFLAGS  := -ml -m4-nofpu -O2 -ffreestanding -fno-builtin -fomit-frame-pointer \
           -Wall -Wextra -std=c11 -DQUICK_TEST=$(QUICK)
LDFLAGS := -nostdlib -Wl,-T,linker.ld -Wl,--build-id=none -Wl,-Map,naomi_diag.map

OBJS := src/crt0.o src/main.o src/scif.o src/sdram.o src/ramtest.o \
        src/timer.o src/aica.o src/pvr.o src/periph.o src/dimm.o src/maple.o src/board.o src/sha1.o src/cart.o

all: naomi_diag.bin

src/%.o: src/%.c src/hw.h src/scif.h src/sdram.h src/ramtest.h \
         src/timer.h src/aica.h src/pvr.h src/periph.h src/dimm.h src/maple.h src/board.h src/sha1.h src/cart.h src/cartdb.h src/audio_clips.h
	$(CC) $(CFLAGS) -c $< -o $@

# regenerate the spoken clips (needs espeak-ng + sox)
audio:
	python3 tools/gen_audio.py src/audio_clips.h

src/%.o: src/%.S
	$(CC) $(CFLAGS) -c $< -o $@

naomi_diag.elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

naomi_diag.bin: naomi_diag.elf
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt 2097152 ]; then \
	    echo "ERROR: image $$sz bytes > 2MB EPROM (would be truncated)"; \
	    rm -f $@; exit 1; fi; \
	    echo "image $$sz / 2097152 bytes ($$((sz*100/2097152))%)"
	truncate -s 2M $@
	python3 tools/patch_crc.py $@

dis: naomi_diag.elf
	$(OBJDUMP) -d $< > naomi_diag.dis

# ---------------------------------------------------------------------------
# MAME test set: copy of the stock naomi.zip (which includes the real
# MIE/JVS device ROMs) with our ROM replacing the default BIOS
# (epr-21576h). The wrong-checksum warning on the BIOS is expected.
# ---------------------------------------------------------------------------
ROMSRC := ../roms/naomi.zip
ROMDIR := ../roms_diag

mame-rom: naomi_diag.bin
	mkdir -p $(ROMDIR)/stage
	cp -f $(ROMSRC) $(ROMDIR)/naomi.zip
	cp -f naomi_diag.bin $(ROMDIR)/stage/epr-21576h.ic27
	cd $(ROMDIR)/stage && zip -j ../naomi.zip epr-21576h.ic27

run-mame: mame-rom
	mame naomi -rompath $(abspath $(ROMDIR)) -video none -sound none \
	    -nothrottle -seconds_to_run 60 \
	    -autoboot_script scif_tap.lua -autoboot_delay 0

clean:
	rm -f src/*.o naomi_diag.elf naomi_diag.bin naomi_diag.map naomi_diag.dis

.PHONY: all dis mame-rom run-mame clean
