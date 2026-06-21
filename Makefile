CC = gcc
CFLAGS = -O3 -Wall -Wextra

TARGET = cviewer

all:
	$(CC) $(CFLAGS) src/main.c -o $(TARGET) -lm

install:
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/bin/$(TARGET)

clean:
	rm -f $(TARGET)