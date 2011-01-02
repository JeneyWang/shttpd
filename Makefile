CC?=cc
CFLAGS?=-O
LIBS=`[ \`uname\` = "SunOS" ] && echo -lsocket -lnsl`

TARGETS = bsd linux solaris
.PHONY: all $(TARGETS)

all: darkhttpd

darkhttpd: darkhttpd.c
	$(CC) $(CFLAGS) $(LIBS) darkhttpd.c -o $@

clean:
	rm -f darkhttpd core darkhttpd.core
