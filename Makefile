# NaomiDiag -- diagnostic BIOS ROM for SEGA Naomi / Naomi 2
# Toolchain: Debian gcc-sh-elf / binutils-sh-elf
#
# Language:  LANG=EN (default), FR, DE, ES, IT, PT, DA, JA, ZH -> NaomiDIAG_<LANG>.bin
# Quick:     QUICK=1 tests only 1MB per RAM pass (fast emulator bring-up)

CROSS   ?= sh-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

LANG    ?= EN
QUICK   ?= 0

# AUDIO=1 (default) embeds the spoken-report clips. AUDIO=0 builds a silent
# ROM from a stub clip table, freeing that space.
AUDIO   ?= 1

# lowercase language tag for the generated audio header / clip source
lang_lc := $(shell echo $(LANG) | tr A-Z a-z)
# DIMMFW=1 -- LOCAL TEST BUILD ONLY: embeds SEGA NetDIMM firmware images,
# packed by tools/dimmfw_pack.py into local/dimmfw.bin (git-ignored): seven
# slot images in one LZMA stream, ~545 KB, so the speech stays in. Sources:
#  - SEGADIMM: MAME's segadimm.zip (a recent set: 0.218's lacks
#    fpr23905c.ic36), used as it is, spaces in its path allowed;
#  - FW403: the "4.03 CD-R" image, FW_Netdimm_403.bin from the SEGA DIMM
#    update CD-R, downloaded on first use from the GitHub repository that
#    keeps it (pinned commit, SHA-256 checked) into local/.
# The image gets its own name, NaomiDIAG_<LANG>_DIMMFW_LOCAL.bin, and must
# NEVER be published.
DIMMFW   ?= 0
SEGADIMM ?= ../SEGA_Naomi_ROMs/segadimm.zip
FW403    ?= local/FW_Netdimm_403.bin
FW403_URL    := https://raw.githubusercontent.com/mathieulh/SEGA_DIMM_CD-R/df16ea0b47fa35fd8910e32360cb9e5aaf455507/Updater/FW_Netdimm_403.bin
FW403_SHA256 := af0f261ee659bbafc49230dc9ab95ffce64090f6677cd9370d1a084c9e6a335e
ifeq ($(DIMMFW),1)
BIN     := NaomiDIAG_$(LANG)_DIMMFW_LOCAL.bin
else
BIN     := NaomiDIAG_$(LANG).bin
endif
ELF     := naomi_diag_$(lang_lc).elf

# Relocate the memory-test loops into CPU RAM and run them cached. RELOC=0
# keeps everything executing from the boot EPROM, which is much slower but
# depends on nothing beyond the ROM itself.
RELOC ?= 1

# Serial console speed: 57600 (default, +0.47 %) or 115200 (-3.1 %).
BAUD ?= 57600
# The G1 bus is unlocked by crt0.S itself (ROM 0x000-0x017, key 0x15):
# tools/check_g1min.py checks that block in every image built.

# Anything in src/config.h can be overridden from the command line without
# editing the file, e.g. make CFLAGS_EXTRA=-DCFG_LANE_BEACON=1
CFLAGS_EXTRA ?=

CFLAGS  := -ml -m4-nofpu -O2 -ffreestanding -fno-builtin -fomit-frame-pointer \
           -Wall -Wextra -std=c11 -DQUICK_TEST=$(QUICK) -DLANG_$(LANG) \
           -DRELOC=$(RELOC) -DCFG_SCIF_BAUD=$(BAUD) $(CFLAGS_EXTRA) \
           -DLANG_STRINGS=\"lang/$(lang_lc).h\" -DLANG_GLYPHS=\"lang/glyphs_$(lang_lc).h\"
# P2 (0xA0000000) is the default: it is the window the reset vector lands in
# and the only one this ROM has been observed to run from on real hardware.
# ROM_BASE=0x80000000 links for P1, the cached alias, which is much faster but
# has not yet been confirmed to boot on a real board -- keep it opt-in.
ROM_BASE ?= 0xA0000000
# --emit-relocs: tools/textreloc.py reads them to let .text run from CPU RAM
LDFLAGS := -nostdlib -Wl,-T,linker.gen.ld -Wl,--build-id=none -Wl,--emit-relocs -Wl,-Map,$(ELF).map

OBJS := src/crt0.o src/main.o src/progress.o src/scif.o src/sdram.o src/ramtest.o src/ramtest_fast.o src/reloc.o \
        src/timer.o src/aica.o src/pvr.o src/pvr2.o src/vram_diag.o src/vram_scan.o src/periph.o src/dimm.o src/maple.o \
        src/input.o src/eta.o src/board.o src/sha1.o src/cart.o \
        src/dimm_host.o src/lzma_dec.o src/dimm_fw.o src/dimmfw_blob.o

HDRS := src/config.h src/version.inc src/hw.h src/scif.h src/sdram.h src/ramtest.h src/vram_mapping.h src/timer.h src/aica.h \
        src/pvr.h src/periph.h src/dimm.h src/maple.h src/board.h src/sha1.h \
        src/cart.h src/cartdb.h src/eta.h src/strings.h src/mie_prog.h src/arm_prog.h src/input.h src/dimm_host.h \
        src/dimm_fw.h src/lzma_dec.h \
        $(wildcard src/lang/*.h)

all: $(BIN)

# config stamp: objects carry no LANG/QUICK in their name, so force a
# rebuild whenever the selected language or QUICK setting changes.
# CFLAGS_EXTRA may hold several flags: their spaces become underscores, or
# the stamp would be several targets and the extra words would be touched
# as stray empty files in the tree (DSIM_LANE0x... and the like).
empty :=
space := $(empty) $(empty)
STAMP := .build_$(LANG)_$(QUICK)_$(RELOC)_A$(AUDIO)_B$(BAUD)_F$(DIMMFW)$(subst $(space),_,$(subst -,,$(subst =,,$(strip $(CFLAGS_EXTRA)))))
$(STAMP):
	rm -f .build_* && touch $@

# the active audio clips header is a copy of the per-language one (AUDIO=1) or
# of the silent stub (AUDIO=0), so the C code can always #include "audio_clips.h"
ifeq ($(AUDIO),0)
audio_src := src/audio_stub.h
CFLAGS    += -DNO_AUDIO=1
else
audio_src := src/audio_clips_$(lang_lc).h
endif

src/audio_clips.h: $(audio_src) $(STAMP)
	cp -f $< $@

src/%.o: src/%.c $(HDRS) src/audio_clips.h $(STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(DIMMFW),1)
CFLAGS += -DDIMMFW=1
# segadimm.zip is followed through a signature file (path, size, date):
# a path with spaces cannot be a make prerequisite.
dimmfw_sig := $(shell stat -c '%n %s %Y' "$(SEGADIMM)" 2>/dev/null || echo "$(SEGADIMM) missing")
$(shell mkdir -p local; [ "$$(cat local/segadimm.sig 2>/dev/null)" = "$(dimmfw_sig)" ] || echo "$(dimmfw_sig)" > local/segadimm.sig)
local/dimmfw.bin: tools/dimmfw_pack.py local/segadimm.sig $(FW403)
	python3 -I tools/dimmfw_pack.py "$(SEGADIMM)" "$(FW403)" $@
local/FW_Netdimm_403.bin:
	mkdir -p local
	curl -fsSL -o $@.part "$(FW403_URL)"
	echo "$(FW403_SHA256)  $@.part" | sha256sum -c --quiet - || \
	    { echo "FW_Netdimm_403.bin: unexpected SHA-256, not used"; rm -f $@.part; exit 1; }
	mv $@.part $@
src/dimmfw_blob.o: local/dimmfw.bin
endif

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
	CROSS=$(CROSS) python3 tools/textreloc.py $< $@
	python3 tools/patch_crc.py $@
	python3 tools/check_g1min.py $@

# 27C322 (4 MB) in the IC27 socket: the 2 MB image twice, so the chip's
# extra address line reads the same whatever level the board leaves it at.
27c322: $(BIN)
	cat $(BIN) $(BIN) > $(BIN:.bin=_27C322.bin)
	@echo "$(BIN:.bin=_27C322.bin): 4194304 bytes (2 MB image twice)"

# Languages with a strings file (src/lang/<lang>.h), clip texts and a voice.
LANGS := EN FR $(sort $(filter-out EN FR,$(shell ls src/lang | sed -n 's/^\([a-z][a-z]\)\.h$$/\1/p' | tr a-z A-Z)))

# regenerate both spoken-clip headers (needs Piper venv + sox)
audio:
	python3 tools/gen_audio.py fr src/audio_clips_fr.h
	python3 tools/gen_audio.py en src/audio_clips_en.h

# one language's generated files: its glyphs (needs tools/fonts/unifont.hex.gz)
# and its spoken clips (Piper; the voice is downloaded if missing), checked
lang-assets:
	python3 -I tools/check_lang.py $(lang_lc)
	python3 -I tools/gen_glyphs.py $(lang_lc)
	python3 tools/gen_audio.py $(lang_lc) src/audio_clips_$(lang_lc).h

# every language's ROM
all-langs:
	@for l in $(LANGS); do $(MAKE) -s LANG=$$l || exit 1; done

# regenerate the AICA ARM7 program header (needs arm-none-eabi binutils)
armprog:
	python3 tools/gen_armprog.py arm/aica_arm_test.S src/arm_prog.h

# regenerate the MIE Z80 program header (needs z80asm)
mieprog:
	python3 tools/gen_mieprog.py src/mie_prog.z80 src/mie_prog.h

# regenerate the cartridge SHA1 database (needs a mame -listxml dump)
cartdb:
	mame -listxml | python3 tools/gen_cartdb.py - src/cartdb.h

dis: $(ELF)
	$(OBJDUMP) -d $< > $(ELF:.elf=.dis)

# ---------------------------------------------------------------------------
# MAME test set: a naomi.zip made from the local SEGA dumps (the board's
# BIOSes and the real MIE/JVS device ROMs, ROMS = a MAME rompath laid out in
# folders, one per set) with our ROM replacing the default BIOS
# (epr-21576h). The wrong-checksum warning on the BIOS is expected.
# ---------------------------------------------------------------------------
ROMS   ?= ../SEGA_Naomi_ROMs
ROMDIR := ../roms_diag

mame-rom: $(BIN)
	mkdir -p $(ROMDIR)/stage
	rm -f $(ROMDIR)/naomi.zip
	zip -j -q $(ROMDIR)/naomi.zip $(ROMS)/naomi/* $(ROMS)/mie/* $(ROMS)/jvs13551/*
	cp -f $(BIN) $(ROMDIR)/stage/epr-21576h.ic27
	cd $(ROMDIR)/stage && zip -j ../naomi.zip epr-21576h.ic27

run-mame: mame-rom
	mame naomi -rompath $(abspath $(ROMDIR)) -video none -sound none \
	    -nothrottle -seconds_to_run 60 \
	    -autoboot_script scif_tap.lua -autoboot_delay 0

clean:
	rm -f src/*.o naomi_diag_*.elf naomi_diag_*.map naomi_diag_*.dis \
	      NaomiDIAG_*.bin src/audio_clips.h .build_* linker.gen.ld

.PHONY: all audio lang-assets all-langs cartdb mieprog dis mame-rom run-mame clean
