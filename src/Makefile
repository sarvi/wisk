

INSTALLDIR = ../binaries

.PHONY: all
all: libfilesystem_tracker.so

.PHONY: install
install:
	mkdir -p $(INSTALLDIR)
	cp -p $(OUTPUTFILE) $(INSTALLDIR)

libfilesystem_tracker.so: filesystem_tracker.o
	$(CXX) -shared -fPIC $(LDFLAGS) -ldl -o $@ $^

filesystem_tracker.o: filesystem_tracker.c
	$(CXX) -fPIC -c -o $@ $^

.PHONY: clean 
clean:
	rm *.so *.o
