
CCAN_DIR=~/src/ccan

CFLAGS:=-Og -std=gnu11 -Wall -g -march=native \
	-D_GNU_SOURCE -I $(CCAN_DIR) -I $(abspath .) \
	#-DDEBUG_ME_HARDER #-DCCAN_LIST_DEBUG=1

TEST_BIN:=$(patsubst t/%.c,t/%,$(wildcard t/*.c))


all: tags bench $(TEST_BIN)


clean:
	@rm -f *.o t/*.o $(TEST_BIN)


distclean: clean
	@rm -f bench tags core
	@rm -rf .deps


check: $(TEST_BIN)
	prove -v -m $(sort $(TEST_BIN))


tags: $(shell find . -iname "*.[ch]" -or -iname "*.p[lm]")
	@ctags -R *


bench: bench.o pht.o \
		ccan-list.o ccan-hash.o ccan-htable.o \
		ccan-tally.o ccan-str.o ccan-read_write_all.o
	@echo "  LD $@"
	@$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)


t/%: t/%.o pht.o \
		ccan-list.o ccan-htable.o ccan-hash.o ccan-tap.o
	@echo "  LD $@"
	@$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)


ccan-%.o ::
	@echo "  CC $@ <ccan>"
	@$(CC) -c -o $@ $(CCAN_DIR)/ccan/$*/$*.c $(CFLAGS)


%.o: %.c
	@echo "  CC $@"
	@$(CC) -c -o $@ $< $(CFLAGS) -MMD
	@test -d .deps || mkdir -p .deps
	@mv $(<:.c=.d) .deps/


include $(wildcard .deps/*.d)
