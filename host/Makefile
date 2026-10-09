# Host build of the engine (macOS / Linux, SDL2): make -j8 && ./q2host +map base1
SRC := ../components/quake2/source
TARGET ?= q2host
ENGINE := $(wildcard $(SRC)/client/*.c $(SRC)/server/sv_*.c $(SRC)/qcommon/*.c $(SRC)/ref_soft/*.c $(SRC)/game/*.c) \
	$(SRC)/other/glob.c $(SRC)/other/q_arena.c $(SRC)/other/q_assets.c $(SRC)/other/q_hunk.c $(SRC)/other/q_system.c \
	$(SRC)/other/vid_lib.c $(SRC)/other/vid_menu.c $(SRC)/net/net_udp.c $(SRC)/null/cd_null.c $(SRC)/port_esp32.c
ENGINE := $(filter-out $(SRC)/server/sv_null.c,$(ENGINE))
OBJDIR ?= obj
OBJ := $(patsubst $(SRC)/%.c,$(OBJDIR)/%.o,$(ENGINE)) $(OBJDIR)/host_main.o
CFLAGS += -MMD -MP -O2 -g -fsigned-char -w -fno-strict-aliasing -Icompat -I$(SRC) $(shell sdl2-config --cflags) $(EXTRA_CFLAGS)
LDLIBS += $(shell sdl2-config --libs) -lm

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

$(OBJDIR)/host_main.o: host_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf obj q2host

-include $(OBJ:.o=.d)
