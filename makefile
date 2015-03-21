DESTDIR ?=
PREFIX ?= /usr
VERSION ?= git

CFLAGS += -std=gnu99 -Isrc/common -Isrc/external -Wall -DVERSION=\"$(VERSION)\"

# Prefer the static library
LIBS += -l:libz.a -lm

ifeq ($(USE_LIBCURL),1)
    CFLAGS += -DUSE_LIBCURL
    # Only sfmf-unpack actually needs the libcurl functions
    LIBS_sfmf-unpack += $(shell pkg-config --libs libcurl)
endif

# Remove unused functions in the executable
CFLAGS += -fdata-sections -ffunction-sections
LIBS += -Wl,--gc-sections

SCRIPTS := $(wildcard scripts/sfmf-*)

TOOLS_SRC := $(wildcard src/tools/*.c)
TOOLS_OBJ := $(patsubst %.c,%.o,$(TOOLS_SRC))

TOOLS := $(patsubst src/tools/%.c,sfmf-%,$(TOOLS_SRC))

STATIC_LIB := libsfmf.a

COMMON_SRC := $(wildcard src/common/*.c src/external/*.c)
COMMON_OBJ := $(patsubst %.c,%.o,$(COMMON_SRC))

all: libsfmf.a $(TOOLS)

$(STATIC_LIB): $(COMMON_OBJ)
	ar rcs $@ $^

sfmf-%: src/tools/%.o $(STATIC_LIB)
	$(CC) -o $@ $^ $(LIBS) $(LIBS_$@)

install: $(TOOLS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 $(TOOLS) $(SCRIPTS) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f $(TOOLS) $(STATIC_LIB) $(COMMON_OBJ) $(TOOLS_OBJ)

distclean: clean

.PHONY:
	all install clean distclean
