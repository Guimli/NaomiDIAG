# NaomiDiag -- diagnostic BIOS ROM for SEGA Naomi / Naomi 2
# Toolchain: Debian gcc-sh-elf / binutils-sh-elf
#
# Language:  LANG=EN (default) or LANG=FR  -> NaomiDIAG_EN.bin / NaomiDIAG_FR.bin
# Quick:     QUICK=1 tests only 1MB per RAM pass (fast emulator bring-up)

CROSS   ?= sh-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

LANG    ?= EN
QUICK   ?= 0

# lowercase language tag for the generated audio header / clip source
lang_lc := $(shell echo $(LANG) | tr A-Z a-z)
BIN     := NaomiDIAG_$(LANG).bin
ELF     := naomi_diag_$(lang_lc).elf

CFLAGS  := -ml -m4-nofpu -O2 -ffreestanding -fno-builtin -fomit-frame-pointer \
           -Wall -Wextra -std=c11 -DQUICK_TEST=$(QUICK) -DLANG_$(LANG)
ROM_BASE ?= 0x80000000
LDFLAGS := -nostdlib -Wl,-T,linker.gen.ld -Wl,--build-id=none -Wl,-Map,$(ELF).map

OBJS := src/crt0.o src/main.o src/scif.o src/sdram.o src/ramtest.o \
        src/timer.o src/aica.o src/pvr.o src/periph.o src/dimm.o src/maple.o \
        src/board.o src/sha1.o src/cart.o

HDRS := src/hw.h src/scif.h src/sdram.h src/ramtest.h src/timer.h src/aica.h \
        src/pvr.h src/periph.h src/dimm.h src/maple.h src/board.h src/sha1.h \
        src/cart.h src/cartdb.h src/version.inc src/strings.h

all: $(BIN)

# config stamp: objects carry no LANG/QUICK in their name, so force a
# rebuild whenever the selected language or QUICK setting changes.
STAMP := .build_$(LANG)_$(QUICK)
$(STAMP):
	rm -f .build_* && touch $@

# the active audio clips header is a copy of the per-language one, so the
# C code can always #include "audio_clips.h"
src/audio_clips.h: src/audio_clips_$(lang_lc).h $(STAMP)
	cp -f $< $@

src/%.o: src/%.c $(HDRS) src/audio_clips.h $(STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.S src/version.inc src/strings.h $(STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

linker.gen.ld: linker.ld
	sed 's/@ROM_BASE@/$(ROM_BASE)/' $< > $@

$(ELF): $(OBJS) linker.gen.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt 2097152 ]; then \
	    echo "ERROR: image $$sz bytes > 2MB EPROM (would be truncated)"; \
	    rm -f $@; exit 1; fi; \
	    echo "$(BIN): $$sz / 2097152 bytes ($$((sz*100/2097152))%)"
	truncate -s 2M $@
	python3 tools/patch_crc.py $@

# regenerate both spoken-clip headers (needs Piper venv + sox)
audio:
	python3 tools/gen_audio.py fr src/audio_clips_fr.h
	python3 tools/gen_audio.py en src/audio_clips_en.h

# regenerate the cartridge SHA1 database (needs a mame -listxml dump)
cartdb:
	python3 tools/gen_cartdb.py mamelist.xml src/cartdb.h

dis: $(ELF)
	$(OBJDUMP) -d $< > $(ELF:.elf=.dis)

# ---------------------------------------------------------------------------
# MAME test set: copy of the stock naomi.zip (which includes the real
# MIE/JVS device ROMs) with our ROM replacing the default BIOS
# (epr-21576h). The wrong-checksum warning on the BIOS is expected.
# ---------------------------------------------------------------------------
ROMSRC := ../roms/naomi.zip
ROMDIR := ../roms_diag

mame-rom: $(BIN)
	mkdir -p $(ROMDIR)/stage
	cp -f $(ROMSRC) $(ROMDIR)/naomi.zip
	cp -f $(BIN) $(ROMDIR)/stage/epr-21576h.ic27
	cd $(ROMDIR)/stage && zip -j ../naomi.zip epr-21576h.ic27

run-mame: mame-rom
	mame naomi -rompath $(abspath $(ROMDIR)) -video none -sound none \
	    -nothrottle -seconds_to_run 60 \
	    -autoboot_script scif_tap.lua -autoboot_delay 0

clean:
	rm -f src/*.o naomi_diag_*.elf naomi_diag_*.map naomi_diag_*.dis \
	      NaomiDIAG_*.bin src/audio_clips.h .build_* linker.gen.ld

.PHONY: all audio cartdb dis mame-rom run-mame clean
