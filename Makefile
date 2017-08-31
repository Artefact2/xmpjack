JACK_FLAGS=$(shell pkg-config --cflags --libs jack)
XMP_FLAGS=$(shell pkg-config --cflags --libs libxmp)

default: build/xmpjack

build/xmpjack: src/xmpjack.c build Makefile
	clang -g -O2 -Wall -Wno-unused-parameter -lm $(JACK_FLAGS) $(XMP_FLAGS) -o $@ $<

build:
	mkdir $@

clean:
	rm -Rf build

.PHONY: clean
