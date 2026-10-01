CC = cc
CFLAGS = -std=c99 -Wall -Wextra -O2

rage: rage.c
	$(CC) $(CLFAGS) rage.c -o rage

clean:
	rm -f rage
