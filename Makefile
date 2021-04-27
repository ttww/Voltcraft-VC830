
CC = gcc
CFLAGS = -O3 -Wall
ARCH	:= $(shell uname -m)

TARGET = vc830

all: $(TARGET).$(ARCH)

$(TARGET).$(ARCH): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET).$(ARCH) $(TARGET).c

clean	:
	$(RM) $(TARGET).$(ARCH)

