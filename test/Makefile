# SPDX-License-Identifier: GPL-2.0
# Makefile for card_sim — Linux userspace card simulator for testing octnic driver

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
TARGET = card_sim

all: $(TARGET)

$(TARGET): card_sim.c
	$(CC) $(CFLAGS) -o $@ $< -lrt

clean:
	rm -f $(TARGET)

.PHONY: all clean
