

CXX=/router/bin/gcc.c4.9.3-p0
INSTALLDIR = ../binaries

.PHONY: all
all: lib64/libwisktrack.so lib32/libwisktrack.so runtests

.PHONY: install
install: install32 install64

.PHONY: install64
install64: lib64/libwisktrack.so
	mkdir -p $(INSTALLDIR)/lib64
	cp -p $^ $(INSTALLDIR)/lib64

.PHONY: install32
install32: lib32/libwisktrack.so
	mkdir -p $(INSTALLDIR)/lib32
	cp -p $^ $(INSTALLDIR)/lib32

.PHONY: runtests
runtests: install
	make -C ../tests

lib64/libwisktrack.so: lib64/wisktrack.o
	$(CXX) -shared -fPIC -pthread $(LDFLAGS) -ldl -o $@ $^

lib32/libwisktrack.so: lib32/wisktrack.o
	$(CXX) -m32 -shared -fPIC -pthread $(LDFLAGS) -ldl -o $@ $^

lib64/wisktrack.o: wisktrack.c
	mkdir -p lib64
	$(CXX) -fPIC -pthread -c -o $@ $^

lib32/wisktrack.o: wisktrack.c
	mkdir -p lib32
	$(CXX) -m32 -fPIC -pthread -c -o $@ $^

.PHONY: clean 
clean:
	rm -rf *.so *.o lib32 lib64
	make -C ../tests clean
