PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall
LDFLAGS  = -lncurses

all: fm

fm: fm.cpp Methods.hpp
	$(CXX) $(CXXFLAGS) fm.cpp $(LDFLAGS) -o fm

install: fm
	install -Dm755 fm           $(DESTDIR)$(BINDIR)/fm
	install -Dm644 fm-shell.sh  $(DESTDIR)$(BINDIR)/fm-shell.sh
	@echo ""
	@echo "✓ fm instalado em $(BINDIR)/fm"
	@echo ""
	@echo "Último passo — adicione ao seu shell (só uma vez):"
	@echo ""
	@echo "  bash:  echo 'source $(BINDIR)/fm-shell.sh' >> ~/.bashrc && source ~/.bashrc"
	@echo "  zsh:   echo 'source $(BINDIR)/fm-shell.sh' >> ~/.zshrc  && source ~/.zshrc"
	@echo ""

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/fm
	rm -f $(DESTDIR)$(BINDIR)/fm-shell.sh
	@echo "✓ fm desinstalado"

clean:
	rm -f fm