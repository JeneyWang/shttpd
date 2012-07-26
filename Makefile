CC=cc
CFLAGS=-O2 -Wall -Wextra
LIBS=`[ \`uname\` = "SunOS" ] && echo -lsocket -lnsl`
TARGETS = bsd linux solaris
.PHONY: all $(TARGETS)

all: shttpd

darkhttpd: shttpd.c
	$(CC) $(CFLAGS) $(LIBS) shttpd.c -o $@

clean:
	rm -f shttpd
