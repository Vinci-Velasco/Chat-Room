CC = gcc
CFLAGS = -g -Wall

all: list.o lets-talk.o
	$(CC) $(CFLAGS) -pthread -o lets-talk list.o lets-talk.o

lets-talk: lets-talk.c
	$(CC) $(CFLAGS) -c lets-talk.c

list: list.h list.c
	$(CC) $(CFLAGS) -c list.c

clean:
	rm *.o
	rm lets-talk

valgrind:
	valgrind ./lets-talk 3001 localhost 3000