CC = gcc
CFLAGS = -Wall -Wextra -std=c11

COMMON_OBJ = ring_common.o

all: ring_driver ring_comm

ring_driver: ring_driver.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ ring_driver.o $(COMMON_OBJ)

ring_comm: ring_comm.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ ring_comm.o $(COMMON_OBJ)

ring_driver.o: ring_driver.c ring_common.h
	$(CC) $(CFLAGS) -c ring_driver.c

ring_comm.o: ring_comm.c ring_common.h
	$(CC) $(CFLAGS) -c ring_comm.c

ring_common.o: ring_common.c ring_common.h
	$(CC) $(CFLAGS) -c ring_common.c

clean:
	rm -f *.o ring_driver ring_comm

.PHONY: all clean
