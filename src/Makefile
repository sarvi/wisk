

CXX=/router/bin/gcc.c4.9.3-p0
INSTALLDIR = ../binaries

.PHONY: all
all: libfilesystem_tracker.so runtests

.PHONY: install
install: libfilesystem_tracker.so
	mkdir -p $(INSTALLDIR)
	cp -p $^ $(INSTALLDIR)

.PHONY: runtests
runtests: install
	make -C ../tests

libfilesystem_tracker.so: filesystem_tracker.o
	$(CXX) -shared -fPIC -pthread $(LDFLAGS) -ldl -o $@ $^

filesystem_tracker.o: filesystem_tracker.c
	$(CXX) -fPIC -pthread -c -o $@ $^

.PHONY: clean 
clean:
	rm *.so *.o
