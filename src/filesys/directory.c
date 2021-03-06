#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    struct lock *lock;                  /* Shared across dirs of inode. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for no entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector)
{
  return inode_create (sector, 0, true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && inode_isdir (inode) && dir != NULL)
    {
      dir->inode = inode;
      dir->lock = inode_dir_lock (dir->inode);
      dir->pos = 2 * sizeof (struct dir_entry); /* Skip . and .. */
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens the directory containing the file at NULL-terminated FILEPATH
   and returns its pointer, or NULL on error. FILEPATH can be a absolute
   path or relative to the current working directory and be made up of
   arbitrarily many nested directory names. */ 
struct dir *
dir_open_dirs (const char *filepath)
{
  struct dir *parent_dir;
  struct inode *curr_inode;
  const char *next_slash;
  char curr_name[NAME_MAX + 1];
  size_t curr_name_len;

  ASSERT (filepath != NULL);

  /* Start with the root dir if FILEPATH is absolute or CWD otherwise. */
  if (filepath[0] == '/')
    {
      parent_dir = dir_open_root();
      filepath++;  /* Skip the root slash. */
    }
  else
    {
      parent_dir = dir_reopen (thread_current ()->cwd);
    }
  if (parent_dir == NULL)
    goto fail;
  
  /* Keep traversing FILEPATH until there are no more trailing slashes. */
  while ((next_slash = strchr (filepath, '/')))
    {
      /* Reject paths with a trailing slash. */
      if (*(next_slash + 1) == '\0')
        goto fail;
      /* Skip consecutive slashes. */
      if (next_slash == filepath)
        {
          filepath++;
          continue;
        }
      curr_name_len = next_slash - filepath;
      /* Fail is the current name is larger than supported. */
      if (curr_name_len > NAME_MAX)
        goto fail;
      /* Copy the NULL-terminated current name. */ 
      strlcpy (curr_name, filepath, curr_name_len + 1);
      /* Lookup the current_name in the parent and fail if not found. */
      lock_acquire (parent_dir->lock);
      if (!dir_lookup (parent_dir, curr_name, &curr_inode))
        goto fail;
      /* Update the state to reflect the current step of path traversal. */
      lock_release (parent_dir->lock);
      dir_close (parent_dir);
      parent_dir = dir_open (curr_inode);
      if (parent_dir == NULL)
        goto fail;
      filepath = next_slash + 1;
    }
  return parent_dir;
fail:
  if (parent_dir != NULL && lock_held_by_current_thread (parent_dir->lock))
    lock_release (parent_dir->lock);
  dir_close (parent_dir);
  return NULL;
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Returns a pointer to the first character in the 
   filename component in FILEPATH (i.e. what comes
   after the last slash in FILEPATH if it exists). */
const char *
dir_parse_filename (const char *filepath)
{
  const char *filename;

  ASSERT (filepath != NULL);
  
  filename = strrchr (filepath, '/');
  if (filename == NULL)
    return filepath;  /* FILEPATH is the file name. */
  return filename + 1;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  lock_acquire (dir->lock);
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  if (lock_held_by_current_thread (dir->lock))
    lock_release (dir->lock);
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  struct dir *dir_removed = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  lock_acquire (dir->lock);
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode and fail on error. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* In the case where inode is a dir, check that it's empty and not open. */
  if (inode_isdir (inode))
    {
      /* Attempt to open the dir and fail otherwise. */
      dir_removed = dir_open (inode);
      if (dir_removed == NULL)
        goto done;
      /* Check that the dir is not open anywhere else and that it's empty. */
      if ((inode_open_count (inode) > 1)
          || !dir_empty (dir_removed))
        goto done;
    }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  if (lock_held_by_current_thread (dir->lock))
    lock_release (dir->lock);
  if (dir_removed != NULL)
    dir_close (dir_removed);
  else if (inode != NULL)
    inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  lock_acquire (dir->lock);
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          lock_release (dir->lock);
          return true;
        } 
    }
  lock_release (dir->lock);
  return false;
}

/* Returns true if DIR is empty (except for subdirs . and ..) and 
   false otherwise. */
bool 
dir_empty (struct dir *dir)
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);

  lock_acquire (dir->lock);
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    /* Check if the entry is in_use and neither . nor .. */
    if (e.in_use && strcmp (".", e.name) && strcmp ("..", e.name))
      {
        lock_release (dir->lock);
        return false;
      }
  lock_release (dir->lock);
  return true;
}
