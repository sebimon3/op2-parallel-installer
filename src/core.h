#ifndef OP22_CORE_H
#define OP22_CORE_H

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*op22_progress)(int done_bytes, int total_bytes, void *user);

/* Decode the selected payload chunks and install them.
 * comp_mask is a bitmask indexed by g_manifest.comps[].  task_mask likewise.
 * Returns 0 on success (all decoded chunks passed their SHA-1 check).
 */
int op22_install(const char *source_dir, const char *output_dir,
                 unsigned comp_mask, unsigned task_mask,
                 op22_progress progress, void *user,
                 char *err, size_t errlen);

/* Limit the decoder worker pool to `n` threads.  0 (default) means use all
 * logical processors, matching the Windows equivalent of nproc. */
void op22_set_worker_limit(int n);

/* Decode and verify every chunk without writing any output. */
int op22_verify_only(const char *source_dir, op22_progress progress, void *user,
                     char *err, size_t errlen);

/* Decode one of the three glove preview BMPs (variant 0..2) into memory.
 * Caller frees *buf with free().
 */
int op22_decode_preview(const char *source_dir, int variant,
                        uint8_t **buf, size_t *len, char *err, size_t errlen);

/* Live per-worker status for the debug progress page.  The caller supplies
 * an array of at most `max` entries and gets back the number filled. */
typedef struct {
    int busy;                  /* 1 while decoding, 0 between chunks */
    int data_index;            /* index into g_manifest.data */
    int file_index;            /* index into g_manifest.files, -1 in verify mode */
    const char *dest;          /* destination path, may be NULL */
    uint64_t bytes_done;       /* bytes of this chunk decoded so far */
    uint64_t bytes_total;      /* uncompressed size of this chunk */
    unsigned thread_id;        /* OS thread id */
} op22_thread_status;

int op22_get_thread_status(op22_thread_status *out, int max);

/* Create Start Menu / desktop shortcuts after a successful install.
 * task_mask bit OP22_TASK_COMMON => all-users desktop, OP22_TASK_CURRENT => user desktop.
 * Returns 0 on success (individual shortcut failures are non-fatal).
 */
int op22_create_shortcuts(const char *output_dir, unsigned task_mask,
                          char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
