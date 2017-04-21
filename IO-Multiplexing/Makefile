
CC = gcc
CFLAGS = -std=gnu11 -Wall -g 
CFLAGS = -Wall -g 
LDFLAGS = -pthread

OBJS = server.o

all: server

server: $(OBJS)

server.o: server.c
	$(CC) $(CFLAGS) -c server.c
clean:
	rm -f *~ *.o server 

