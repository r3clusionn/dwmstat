CFLAGS ?= -O2 -march=native -pipe
CFLAGS += -std=c99 -Wall -Wextra
PREFIX ?= $(HOME)/.local

dwmstat: dwmstat.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lX11

install: dwmstat
	install -Dm755 dwmstat $(PREFIX)/bin/dwmstat

clean:
	rm -f dwmstat
