CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 \
          $(shell pkg-config --cflags libpipewire-0.3 x11)
LIBS    = $(shell pkg-config --libs   libpipewire-0.3 x11) \
          -lXext -lm -lpthread
TARGET  = audiov
PREFIX  ?= $(HOME)/.local

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): audiov.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
	@echo "  ✓  built ./$(TARGET)"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET)  $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 README.md  $(DESTDIR)$(PREFIX)/share/doc/$(TARGET)/README.md
	install -Dm644 LICENSE    $(DESTDIR)$(PREFIX)/share/licenses/$(TARGET)/LICENSE
	@echo "  ✓  installed to $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f  $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/$(TARGET)
	@echo "  ✓  uninstalled"
