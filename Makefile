CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
           -Wstrict-prototypes -Wpointer-arith -D_FORTIFY_SOURCE=2 \
           -fstack-protector-strong
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGET := freq-tuner
PROBE  := knobprobe

SRC := src/main.c src/plugin.c src/focus.c src/plugins/gqrx.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean install uninstall

all: $(TARGET) $(PROBE)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(PROBE): src/knobprobe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET) $(PROBE)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 755 $(PROBE) $(DESTDIR)$(BINDIR)/$(PROBE)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(BINDIR)/$(PROBE)

clean:
	rm -f $(TARGET) $(PROBE) $(OBJ)