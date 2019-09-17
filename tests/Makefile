

SRCS = $(wildcard *.c)
PROGS = $(patsubst %.c,%.testbin,$(SRCS))

CXX=/router/bin/gcc.c4.9.3-p0
INSTALLDIR = ../testdir

.PHONY: all
all: $(PROGS)
	@echo "Testing Started"
	mkdir -p /tmp/filesystem_testdir
	SOCKET_WRAPPER_DIR=/tmp/tracker.pipe LD_PRELOAD=../binaries/libfilesystem_tracker.so ./hello_world.testbin
	@echo "Testing Complete"

.PHONY: testall
testall:
	mkdir -p $(INSTALLDIR)
	cp -p $(OUTPUTFILE) $(INSTALLDIR)

%.testbin: %.c
	$(CXX) -fPIC $(LDFLAGS) -o $@ $^

.PHONY: clean 
clean:
	rm *.testbin *.o
