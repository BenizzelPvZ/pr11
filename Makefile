# Makefile für Dateikopier-Client/Server

CC = gcc
CFLAGS = -Wall -Wextra -g

all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o server

client: client.c
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client
	rm -f /tmp/mysocket

.PHONY: all clean
