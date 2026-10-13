# Jetpac SMS - devkitSMS / SDCC build
DEVKIT   ?= $(HOME)/.devkitsms
PROG     ?= jetpac
CC       := sdcc
IHX2SMS  := $(DEVKIT)/bin/ihx2sms
FOLDER2C := $(DEVKIT)/bin/folder2c
SMSTEST  := $(DEVKIT)/bin/smstest

BANKFLAGS := -Wl-b_BANK2=0x28000 -Wl-b_BANK3=0x38000
CFLAGS   := -mz80 -I$(DEVKIT)/include --peep-file $(DEVKIT)/include/peep-rules.txt -Isrc -Ibuild --opt-code-speed
LDFLAGS  := -mz80 --no-std-crt0 --data-loc 0xC000 $(BANKFLAGS)

CODEOBJS := build/main.rel build/game.rel build/render.rel build/sound.rel build/gen_tables.rel build/gen_sfx.rel build/isr.rel
BANKRELS := build/bank2.rel build/bank3.rel

B2FILES := neb_tiles neb_map font plat_tiles pal_bg spr_core
B3FILES := spr_alien spr_rocket

all: $(PROG).sms

# folder2c emits the arrays in readdir() order, which differs between file
# systems; converting one file at a time and concatenating in B2FILES/B3FILES
# order keeps the ROM reproducible.
define BANK_RULE
build/bank$(1).c: $$($(2):%=data/%.bin)
	rm -rf build/b$(1) && mkdir -p build/b$(1)
	for f in $$($(2)); do mkdir -p build/b$(1)/$$$$f && cp data/$$$$f.bin build/b$(1)/$$$$f/ && (cd build/b$(1) && $$(FOLDER2C) $$$$f $$$$f >/dev/null); done
	cat $$($(2):%=build/b$(1)/%.c) > build/bank$(1).c
	cat $$($(2):%=build/b$(1)/%.h) > build/bank$(1).h
endef
$(eval $(call BANK_RULE,2,B2FILES))
$(eval $(call BANK_RULE,3,B3FILES))

build/bank%.rel: build/bank%.c
	$(CC) -mz80 --constseg BANK$* -c $< -o $@

build/%.rel: src/%.c src/game.h src/gen_assets.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/isr.rel: src/isr.s
	@mkdir -p build
	sdasz80 -o $@ $<

build/$(PROG).ihx: $(CODEOBJS) $(BANKRELS)
	@rm -f $@
	-$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel $(CODEOBJS) \
	      $(DEVKIT)/lib/SMSlib.lib $(DEVKIT)/lib/PSGlib.lib $(BANKRELS) 2>&1 | grep -v "Conflicting sdcc options\|sdcccall(1)\|in module" || true
	@test -f $@

$(PROG).sms: build/$(PROG).ihx
	$(IHX2SMS) $< $@
	cp build/$(PROG).noi $(PROG).noi 2>/dev/null || true

playtest: $(PROG).sms
	mkdir -p shots
	$(SMSTEST) $(PROG).sms tests/playtest.txt

# run every scenario test
test: $(PROG).sms
	mkdir -p shots
	@for t in smoke play1 play2 play3 play4 play5 play6 play7 busy playtest; do \
	  printf '%-10s ' $$t; $(SMSTEST) $(PROG).sms tests/$$t.txt | tail -1; done

clean:
	rm -rf build $(PROG).sms $(PROG).noi

.PHONY: all clean playtest test
