#include "vm/frame.h"
#include <debug.h>
#include <stdio.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Frame table keeping track of all frames in the system. */
static struct frame_table ft;
/* Lock guarding operations on ft. */
static struct lock frame_table_lock;

static struct frame *frame_pick_and_evict (void);

/* Frees up FRAME for future use. Does not evict the data in 
   FRAME. */
void 
frame_free (struct frame *frame)
{
  lock_acquire (&frame_table_lock);
  /* Move the frame to the list of free frames. */
  frame->page = NULL;
  frame->pinned = false;
  list_remove (&frame->elem);
  list_push_back (&ft.free_frames, &frame->elem);
  lock_release (&frame_table_lock);
}

/* Initializes the frame table FT by calling palloc_get_page on all
   user pages and storing them as free frames for future use. */
void
frame_init (void)
{
  void *upage;
  struct frame *frame;

  lock_init (&frame_table_lock);
  lock_acquire (&frame_table_lock);
  list_init (&ft.free_frames);
  list_init (&ft.allocated_frames);
  /* Query palloc_get_page until user pool is exhausted. */
  while ((upage = palloc_get_page (PAL_USER)))
    {
      frame = malloc (sizeof (struct frame));
      ASSERT (frame != NULL); /* Otherwise fails to build frame table. */
      frame->kaddr = upage;
      frame->page = NULL;
      frame->pinned = false;
      list_push_back (&ft.free_frames, &frame->elem);
    }
  lock_release (&frame_table_lock);
}

/* Helper for frame_alloc. Chooses which frame to evict and evicts it.
   Assumes that frame_table_lock is acquired by the current thread.

   TODO - implement a better eviction strategy. */
static struct frame *
frame_pick_and_evict (void)
{
  struct list_elem *e;
  struct frame *frame = NULL;

  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  ASSERT (!list_empty (&ft.allocated_frames));

  /* Simple algorithm just scans for an unpinned frame. */
  for (e = list_begin (&ft.allocated_frames); 
       e != list_end (&ft.allocated_frames);
       e = list_next (e))
    {
      frame = list_entry (e, struct frame, elem);
      if (frame_evict (frame))
        break;
      else
        frame = NULL;
    }
  /* Panic if unable to evict any frames, i.e. OOM. */
  if (frame == NULL)
    PANIC ("Attempting to evict a frame but all frames are pinned!");
  return frame;
}

/* Allocates a free frame for THREAD's PAGE and updates THREAD's
   pagedir to reflect the new physical address. Useful when resolving
   a pagefault. The returned frame is pinned by default. */
struct frame *
frame_alloc (void)
{
  struct frame *frame;

  lock_acquire (&frame_table_lock);
  if (!list_empty (&ft.free_frames))
    frame = list_entry (list_pop_back (&ft.free_frames), struct frame, elem);
  else
    frame = frame_pick_and_evict ();
  frame->pinned = true;
  list_push_back (&ft.allocated_frames, &frame->elem);
  lock_release (&frame_table_lock);
  return frame;
}

/* Makes sure FRAME is never evicted after this call until
   frame_unpin (FRAME) is called. */
void
frame_pin (struct frame *frame)
{
  lock_acquire (&frame_table_lock);
  frame->pinned = true;
  lock_release (&frame_table_lock);
}

/* Cancels the effect of frame_pin and makes FRAME a candidate
   for eviction. */
void
frame_unpin (struct frame *frame)
{
  lock_acquire (&frame_table_lock);
  frame->pinned = false;
  lock_release (&frame_table_lock);
}

/* Evicts FRAME by calling page_evict on its page if it exists. 
   It also removes the frame from the list of allocated frames.
   Returns false if FRAME is pinned or page_evict fails and
   true otherwise. Assumes frame_table_lock is acquired. */
bool
frame_evict (struct frame *frame)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  
  /* Will not evict a pinned frame. */
  if (frame->pinned)
    return false;
  /* Evict page if it exists. */
  if (frame->page != NULL && !page_evict (frame->page))
    return false;
  frame->page = NULL;
  /* Remove the frame from list of allocated frames. */
  list_remove (&frame->elem);
  return true;
}

