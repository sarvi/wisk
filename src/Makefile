

CXX=/router/bin/gcc.c4.9.3-p0
INSTALLDIR = ../binaries

.PHONY: all
all: libwisktrack.so runtests

.PHONY: install
install: libfilesystem_tracker.so
	mkdir -p $(INSTALLDIR)
	cp -p $^ $(INSTALLDIR)

.PHONY: runtests
runtests: install
	make -C ../tests

libwisktrack.so: wisktrack.o
	$(CXX) -shared -fPIC -pthread -luuid $(LDFLAGS) -ldl -o $@ $^

wisktrack.o: wisktrack.c
	$(CXX) -fPIC -pthread -c -o $@ $^

.PHONY: clean 
clean:
	rm *.so *.o
