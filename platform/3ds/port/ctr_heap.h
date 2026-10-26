#pragma once

/*
 * (AI-assisted)
 * The 3DS malloc heap (ctr_heap.cpp): what is free, and in trace builds who holds it.
 */

#include <cstddef>
#include <cstdint>

struct CtrHeapStats {
  uint32_t heap;        // the heap's size (__ctru_heap_size)
  uint32_t in_use;      // mallinfo uordblks
  uint32_t arena_free;  // free inside the arena (mallinfo fordblks)
  uint32_t never_used;  // beyond the arena (never handed to malloc yet)
  uint32_t free;        // arena_free + never_used
  uint32_t largest;     // the largest block malloc can hand out now
};

// (walks the free lists and probes malloc: not every frame)
void ctr_heap_stats(CtrHeapStats* st);
// allocations that failed so far
uint32_t ctr_heap_failures();
// trace builds (OG3DS_HEAP_TRACE): the sites holding the most heap, into the log; else nothing
void ctr_heap_trace_dump(const char* tag);

// (AI-assisted, lavamem) The music bank's block: made once (the first music load, early in the
// session) and kept, so a music bank always finds its memory however fragmented the heap gets
// later (lava tube, Oct 25: 2.6 MB free, no block of 564 KB: no music). One music bank is loaded at
// a time (srpc.cpp unloads the old one first). take: the block for n bytes (nullptr: too big, or
// taken); free() of it gives it back to the slot.
void* ctr_heap_music_slot_take(size_t n);
