

SRCS = $(wildcard *.c)
# SRCS = test_execve1.c
PROGS = $(patsubst %.c,%.testbin,$(SRCS))

CXX=/router/bin/gcc.c4.9.3-p0
SCRIPTDIR = ../scripts
INSTALLDIR = ../testdir
TRACKER_PIPE = /tmp/tracker.pipe

.PHONY: all
all: $(PROGS)
#	${SCRIPTDIR}/wisktrack -vv ./test_open1.testbin
#	FS_TRACKER_PIPE=$TRACKER_PIPE LD_PRELOAD=../binaries/libfilesystem_tracker.so ./hello_world.testbin

.PHONY: testall
testall:
	mkdir -p $(INSTALLDIR)
	cp -p $(OUTPUTFILE) $(INSTALLDIR)

%.testbin: %.c
	@echo "Compiling Test Case"
	$(CXX) -fPIC $(LDFLAGS) -o $@ $^
	@echo "Testing Started"
	${SCRIPTDIR}/wisktrack -vvvv ./$@
	@echo "Testing Complete"
	@rm $@

.PHONY: clean 
clean:
	rm *.testbin *.o
