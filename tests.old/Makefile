

SRCS = $(wildcard *.c)
PROGS = $(patsubst %.c,%.testbin,$(SRCS))

CXX=/router/bin/gcc.c4.9.3-p0
INSTALLDIR = ../testdir

.PHONY: all
all: $(PROGS)
	SOCKET_WRAPPER_DIR=/tmp/filesystem_testdir LD_PRELOAD=../filesystem_tracker.so ./hello_world.testbin

.PHONY: testall
testall:
	mkdir -p $(INSTALLDIR)
	cp -p $(OUTPUTFILE) $(INSTALLDIR)

%.testbin: %.c
	$(CXX) -fPIC $(LDFLAGS) -o $@ $^

.PHONY: clean 
clean:
	rm *.testbin *.o
