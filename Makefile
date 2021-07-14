queue: queue.c
	gcc -g -Wall -O0 $^ -o $@ -lpthread -lurcu
