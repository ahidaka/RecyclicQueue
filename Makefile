#

TARGET = queue

CC = gcc
IFLAGS = 
CFLAGS = -g -Wall -Wno-parentheses -Wno-unused-function -Wno-format-overflow $(IFLAGS)

all: $(TARGET)
	touch make.date

queue: queue.c
	$(CC) $(CFLAGS) queue.c -o queue -lpthread

clean:
	/bin/rm -f core *.o *~ $(TARGET) make.date
