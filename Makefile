# flmarkdown - plain old Makefile
#
#   make                   build ./flmarkdown
#   make run [FILE=x.md]   build and run
#   make install           install program, menu entry, icons (use sudo)
#   make uninstall         remove everything 'install' put there (use sudo)
#   make default           make flmarkdown your default .md viewer (no sudo)
#   make undefault         undo 'make default' (no sudo)
#   make test              self-test every menu action (needs a display)
#   make clean             remove objects and binary
#   make icons             regenerate PNG icons + embedded icon from the SVG
#   make help              list targets
#
# Override like:  make PREFIX=$HOME/.local install   (per-user, no sudo)
#                 make DESTDIR=/tmp/pkg install      (staged, for packaging)

PROG        = flmarkdown
VERSION     = 1.1.0
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
APPDIR     ?= $(DATADIR)/applications
ICONDIR    ?= $(DATADIR)/icons/hicolor
DOCDIR     ?= $(DATADIR)/doc/$(PROG)

CXX        ?= g++
FLTK_CONFIG ?= fltk-config
PKG_CONFIG ?= pkg-config
INSTALL    ?= install

CXXFLAGS   ?= -O2
CXXFLAGS   += -std=c++17 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS   += -DFLMD_VERSION=\"$(VERSION)\" \
              $(shell $(FLTK_CONFIG) --use-images --cxxflags) \
              $(shell $(PKG_CONFIG) --cflags md4c)
LDLIBS     += $(shell $(FLTK_CONFIG) --use-images --ldflags) \
              $(shell $(PKG_CONFIG) --libs md4c)

SRCS = src/main.cpp src/render.cpp
OBJS = $(SRCS:src/%.cpp=obj/%.o)
DEPS = $(OBJS:.o=.d)

ICON_SIZES = 16 24 32 48 64 128 256
MIME_TYPES = text/markdown text/x-markdown

.PHONY: all run install uninstall default undefault clean icons help test

all: $(PROG)

$(PROG): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

obj/%.o: src/%.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

obj:
	mkdir -p obj

run: $(PROG)
	./$(PROG) $(FILE)

# Refresh the menu / "Open With" database and the icon cache, but only for a
# real install (not a DESTDIR staging install).
define refresh_caches
	@if [ -z "$(DESTDIR)" ]; then \
		command -v update-desktop-database >/dev/null && update-desktop-database -q $(APPDIR) || true; \
		command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t -f $(ICONDIR) 2>/dev/null || true; \
		touch $(APPDIR) 2>/dev/null || true; \
	fi
endef

install: $(PROG)
	$(INSTALL) -Dm755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	$(INSTALL) -Dm644 data/$(PROG).desktop $(DESTDIR)$(APPDIR)/$(PROG).desktop
	$(INSTALL) -Dm644 data/icons/$(PROG).svg $(DESTDIR)$(ICONDIR)/scalable/apps/$(PROG).svg
	for s in $(ICON_SIZES); do \
		$(INSTALL) -Dm644 data/icons/$(PROG)-$$s.png $(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/$(PROG).png || exit 1; \
	done
	$(INSTALL) -Dm644 README.md $(DESTDIR)$(DOCDIR)/README.md
	$(refresh_caches)
	@echo
	@echo "Installed. flmarkdown is now in the menu (Accessories) and in"
	@echo "'Open With' for .md files in Thunar and SpaceFM."
	@echo "To open .md files with it on double-click, run (without sudo):  make default"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
	rm -f $(DESTDIR)$(APPDIR)/$(PROG).desktop
	rm -f $(DESTDIR)$(ICONDIR)/scalable/apps/$(PROG).svg
	for s in $(ICON_SIZES); do rm -f $(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/$(PROG).png; done
	rm -rf $(DESTDIR)$(DOCDIR)
	$(refresh_caches)
	@echo "Uninstalled $(PROG) from $(DESTDIR)$(PREFIX)"

# Per-user file association (writes ~/.config/mimeapps.list). Run without sudo.
default:
	@if [ "$$(id -u)" = 0 ]; then echo "Run 'make default' as your normal user, not with sudo."; exit 1; fi
	xdg-mime default $(PROG).desktop $(MIME_TYPES)
	@echo "Default for .md is now: $$(xdg-mime query default text/markdown)"

undefault:
	@if [ "$$(id -u)" = 0 ]; then echo "Run 'make undefault' as your normal user, not with sudo."; exit 1; fi
	f=$${XDG_CONFIG_HOME:-$$HOME/.config}/mimeapps.list; \
	[ -f "$$f" ] && sed -i -e '/^text\/\(x-\)\{0,1\}markdown=$(PROG)\.desktop;\{0,1\}$$/d' \
	                       -e 's/^\(text\/\(x-\)\{0,1\}markdown=.*\)$(PROG)\.desktop;/\1/' "$$f" || true
	@echo "Default for .md is now: $$(xdg-mime query default text/markdown || true)"

# Self-test: a separate build that runs every menu action inside the app
# and checks the result, plus a layout check of the bundled documents.
# xdg-open is replaced by tests/fakebin/xdg-open, which only writes to
# tests/xdg-open.log, so nothing gets launched.
$(PROG)-selftest: $(SRCS) $(wildcard src/*.h) tests/selftest.inc
	$(CXX) $(CPPFLAGS) -DFLMD_SELFTEST $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

test: $(PROG) $(PROG)-selftest
	@for f in README.md docs/sample.md; do \
		./$(PROG) --check $$f && echo "ok    layout fits the window: $$f" || exit 1; \
	done
	@: > tests/xdg-open.log
	PATH="$(CURDIR)/tests/fakebin:$$PATH" FLMD_XDG_LOG="$(CURDIR)/tests/xdg-open.log" \
		./$(PROG)-selftest README.md

clean:
	rm -rf obj $(PROG) $(PROG)-selftest tests/xdg-open.log

# Maintainer target: needs ImageMagick and python3. The results are committed,
# so normal builds and installs don't need these tools.
icons: data/icons/$(PROG).svg
	for s in $(ICON_SIZES); do \
		convert -background none -density $$((s * 2 * 96 / 128)) $< -resize $${s}x$${s} \
			PNG32:data/icons/$(PROG)-$$s.png || exit 1; \
	done
	python3 tools/embed.py data/icons/$(PROG)-64.png src/icon_data.h icon_png

help:
	@sed -n '3,15p' Makefile | sed 's/^# \{0,1\}//'

-include $(DEPS)
