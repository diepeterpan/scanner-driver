CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC $(shell pkg-config --cflags libusb-1.0 sane-backends 2>/dev/null || echo "-I/usr/include")
LDFLAGS ?= -shared -Wl,-soname,libsane-ricoh_sp110.so.1
LIBS ?= $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-lusb-1.0")

TARGET = libsane-ricoh_sp110.so.1.0.0
SYMLINK1 = libsane-ricoh_sp110.so.1
SYMLINK2 = libsane-ricoh_sp110.so

all: $(TARGET)

$(TARGET): ricoh_sp110.c ricoh_sp110.h
	$(CC) $(CFLAGS) $(LDFLAGS) ricoh_sp110.c -o $(TARGET) $(LIBS)
	ln -sf $(TARGET) $(SYMLINK1)
	ln -sf $(SYMLINK1) $(SYMLINK2)

clean:
	rm -f $(TARGET) $(SYMLINK1) $(SYMLINK2) *.o

install: $(TARGET)
	install -d /usr/lib/x86_64-linux-gnu/sane /etc/sane.d /etc/udev/rules.d
	install -m 755 $(TARGET) /usr/lib/x86_64-linux-gnu/sane/
	ln -sf $(TARGET) /usr/lib/x86_64-linux-gnu/sane/$(SYMLINK1)
	install -m 644 ../sane.d/ricoh_sp110.conf /etc/sane.d/
	install -m 644 ../sane.d/99-ricoh-sp110.rules /etc/udev/rules.d/
	@grep -q "^ricoh_sp110" /etc/sane.d/dll.conf || echo "ricoh_sp110" >> /etc/sane.d/dll.conf
	udevadm control --reload-rules && udevadm trigger || true

.PHONY: all clean install
