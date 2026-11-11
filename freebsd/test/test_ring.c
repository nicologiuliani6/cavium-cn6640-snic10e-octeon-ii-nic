/*-
 * test_ring.c — unit tests for ring/phase-bit logic.
 * Pure C, no framework. Compiles as a standalone host-side binary.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#define RING_SIZE 128

/* Phase bit: bit 7 of the producer/consumer counter.
 * Matches OCTNIC_TX_PHASE / OCTNIC_RX_PHASE in octnic_regs.h. */
static inline uint32_t octnic_phase(uint32_t val)
{
	return (val >> 7) & 1u;
}

/* Slot index: lower 7 bits of the counter.
 * Matches `tp & (OCTNIC_RING_SIZE - 1)` in if_octnic.c. */
static inline uint32_t ring_index(uint32_t val)
{
	return val & (RING_SIZE - 1);
}

/* Slot-free check: a descriptor slot is free when the producer phase
 * equals the descriptor's stored phase bit.
 * Matches the TX path in if_octnic.c line 358. */
static inline int ring_slot_free(uint32_t prod, uint32_t desc_phase)
{
	return octnic_phase(prod) == desc_phase;
}

/* Ring-full check: ring is full when the distance between producer
 * and consumer is >= RING_SIZE.
 * Matches `(tp - port->op_tc_cache) >= OCTNIC_RING_SIZE` in if_octnic.c. */
static inline int ring_full(uint32_t prod, uint32_t cons)
{
	return (prod - cons) >= RING_SIZE;
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("=== test_ring ===\n");

	/* --- octnic_phase --- */

	/* Phase is 0 for counters 0..127, 1 for 128..254, 0 again at 256. */
	assert(octnic_phase(0)   == 0);
	assert(octnic_phase(63)  == 0);
	assert(octnic_phase(127) == 0);
	assert(octnic_phase(128) == 1);
	assert(octnic_phase(254) == 1);
	assert(octnic_phase(255) == 1);
	assert(octnic_phase(256) == 0);
	assert(octnic_phase(383) == 0);
	assert(octnic_phase(384) == 1);

	/* Phase flips every RING_SIZE steps and repeats every 2*RING_SIZE. */
	for (uint32_t i = 0; i < 1024; i++) {
		uint32_t p = octnic_phase(i);
		assert(p != octnic_phase(i + RING_SIZE));
		assert(p == octnic_phase(i + 2 * RING_SIZE));
	}

	/* --- ring_index --- */

	/* Normal range: 0..127. */
	for (int i = 0; i < 128; i++)
		assert(ring_index(i) == (uint32_t)i);

	/* Wrapping: 128 -> 0, 129 -> 1, ... */
	assert(ring_index(128)  == 0);
	assert(ring_index(129)  == 1);
	assert(ring_index(255)  == 127);
	assert(ring_index(256)  == 0);
	assert(ring_index(383)  == 127);
	assert(ring_index(384)  == 0);

	/* Arbitrary large values. */
	assert(ring_index(1000) == (1000 & 127));   /* 1000 & 127 = 56 */
	assert(ring_index(0xFFFFFFFF) == 127);

	/* --- ring_slot_free --- */

	/* Same phase => slot is free. */
	assert(ring_slot_free(0,   0) == 1);
	assert(ring_slot_free(128, 1) == 1);
	assert(ring_slot_free(256, 0) == 1);

	/* Different phase => slot is occupied. */
	assert(ring_slot_free(0,   1) == 0);
	assert(ring_slot_free(128, 0) == 0);
	assert(ring_slot_free(256, 1) == 0);

	/* Wrapping: counter 127 has phase 0, counter 128 has phase 1. */
	assert(ring_slot_free(127, 0) == 1);
	assert(ring_slot_free(127, 1) == 0);
	assert(ring_slot_free(128, 1) == 1);
	assert(ring_slot_free(128, 0) == 0);

	/* --- ring_full --- */

	/* Empty ring: prod == cons => not full. */
	assert(ring_full(0, 0)   == 0);
	assert(ring_full(100, 50) == 0);  /* distance = 50 < 128 */

	/* Exactly full: distance == RING_SIZE => full. */
	assert(ring_full(128, 0) == 1);
	assert(ring_full(256, 128) == 1);
	assert(ring_full(1000, 872) == 1);  /* 1000 - 872 = 128 */

	/* Over-full (shouldn't happen in practice but logic holds). */
	assert(ring_full(256, 0) == 1);  /* distance = 256 >= 128 */

	/* Wrapping-safe: unsigned subtraction handles wrap of the counter itself. */
	assert(ring_full(0x7FFFFFFF, 0x7FFFFFFF - 128) == 1);

	printf("PASS\n");
	return 0;
}
