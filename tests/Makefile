

# SRCS = $(wildcard *.c)
SRCS = test_exec.c
PROGS = $(patsubst %.c,%.testbin,$(SRCS))

ifeq (,$(wildcard /router/bin/gcc.c4.9.3-p0))
CXX=/router/bin/gcc.c4.7.0-p3
else
CXX=/router/bin/gcc.c4.9.3-p0
endif
$(info "CXX=${CXX}")
SCRIPTDIR = ../scripts
INSTALLDIR = ../testdir
TRACKER_PIPE = /tmp/tracker.pipe

.PHONY: all
all: $(PROGS)
#	${SCRIPTDIR}/wisktrack -vvvv code.py
#	${SCRIPTDIR}/wisktrack -vv ./test_open1.testbin
#	FS_TRACKER_PIPE=$TRACKER_PIPE LD_PRELOAD=../binaries/libwisktrack.so ./hello_world.testbin

.PHONY: testall
testall:
	mkdir -p $(INSTALLDIR)
	cp -p $(OUTPUTFILE) $(INSTALLDIR)

%.testbin: %.c
	@echo "Compiling Test Case"
	$(CXX) -fPIC $(LDFLAGS) -o $@ $^
	@echo "Testing Started"
	if [[ "$@" == "test_exec.testbin" ]]; then \
	    ./$@ execv ; ./$@ execve ; ./$@ execvpe ; ./$@ execvp ; ./$@ execl ; ./$@ execle ; ./$@ execlp ; ./$@ execlpe ; \
	else \
	    ./$@ ; \
	fi
#	${SCRIPTDIR}/wisktrack -vvvv ./$@
	@echo "Testing Complete"
	@rm $@

.PHONY: clean 
clean:
	rm -f *.testbin *.o
