/* Shared interface between the extraction core and the Win32 wizard.
 * All strings are UTF-8.  The manifest is generated at build time by
 * gen_manifest.py from op22_meta.json (which itself comes from the
 * CRC-validated Inno Setup header parser).
 */
#ifndef OP22_APP_H
#define OP22_APP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;      /* internal component name, e.g. addons\sounds\cont\cont2 */
    const char *desc;      /* resolved display text (may be Russian UTF-8) */
    unsigned flags;        /* bit0 = Fixed, bit1 = Exclusive (radio) */
    uint64_t size;         /* bytes this component contributes */
    const char *parent;    /* internal name of parent, or "" */
} op22_comp;

typedef struct {
    const char *dest;      /* relative destination path ({app}\ / {tmp}\ stripped) */
    int location;          /* index into op22_manifest.data */
    const char *components;/* component expression, "" = always installed */
    const char *tasks;     /* task expression, "" = always */
    uint64_t size;         /* uncompressed bytes */
    int is_tmp;            /* 1 = {tmp} file, not installed permanently */
} op22_file;

typedef struct {
    int      first_slice;        /* 0-based slice index */
    uint64_t offset;             /* absolute file offset, includes 12-byte slice header */
    uint64_t compressed_size;    /* raw LZMA1 bytes after the 9-byte chunk header */
    uint64_t uncompressed_size;
    uint8_t  sha1[20];
    int      call_optimize;      /* 1 = reverse Inno's 64 KiB CALL/JMP transform */
} op22_data;

typedef struct {
    const char *app_name;
    const char *app_version;
    const char *publisher;
    const char *copyright;
    const char *default_dir;     /* e.g. "OP-2.2" */
    int slices;
    int slices_per_disk;

    const op22_comp *comps; int ncomp;
    const op22_file *files; int nfile;
    const op22_data *data; int ndata;

    const uint8_t *wizard_bmp;       size_t wizard_bmp_len;
    const uint8_t *wizard_small_bmp; size_t wizard_small_bmp_len;

    /* Wizard strings (already resolved from the installer's message table) */
    const char *s_title;
    const char *s_lang_title, *s_lang_label;
    const char *s_dest_title, *s_dest_label, *s_space_required;
    const char *s_comp_title, *s_comp_label, *s_install_col, *s_size_col;
    const char *s_tasks_title, *s_tasks_label, *s_desktop_task, *s_common, *s_current;
    const char *s_ready_title, *s_ready_label;
    const char *s_installing, *s_finish_title, *s_finish_label;
    const char *s_next, *s_back, *s_cancel, *s_install, *s_browse, *s_finish_btn;
} op22_manifest;

/* Non-const only so ui.c can attach the RCDATA-loaded wizard artwork at
 * startup; all other fields are treated as read-only. */
extern op22_manifest g_manifest;

/* Component/task names for the Tasks page (task indices 0..2). */
#define OP22_TASK_DESKTOP 0
#define OP22_TASK_COMMON  1
#define OP22_TASK_CURRENT 2
#define OP22_NTASKS 3

#ifdef __cplusplus
}
#endif
#endif
