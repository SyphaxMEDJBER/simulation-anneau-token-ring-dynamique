CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude
BUILD_DIR = build
BIN_DIR = bin
SRC_DIR = src

DRIVER_OBJ = $(BUILD_DIR)/ring_driver.o
COMM_OBJ = $(BUILD_DIR)/ring_comm.o
COMMON_OBJ = $(BUILD_DIR)/ring_common.o

all: dirs $(BIN_DIR)/ring_driver $(BIN_DIR)/ring_comm

dirs:
	mkdir -p $(BUILD_DIR) $(BIN_DIR)

$(BIN_DIR)/ring_driver: $(DRIVER_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $(DRIVER_OBJ) $(COMMON_OBJ)

$(BIN_DIR)/ring_comm: $(COMM_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $(COMM_OBJ) $(COMMON_OBJ)

$(BUILD_DIR)/ring_driver.o: $(SRC_DIR)/ring_driver.c include/ring_common.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/ring_driver.c -o $@

$(BUILD_DIR)/ring_comm.o: $(SRC_DIR)/ring_comm.c include/ring_common.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/ring_comm.c -o $@

$(BUILD_DIR)/ring_common.o: $(SRC_DIR)/ring_common.c include/ring_common.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/ring_common.c -o $@

clean:
	rm -f $(BUILD_DIR)/*.o $(BIN_DIR)/ring_driver $(BIN_DIR)/ring_comm

.PHONY: all clean dirs
