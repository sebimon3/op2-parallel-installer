#ifndef OP22_UI_H
#define OP22_UI_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run the wizard.  source_dir is UTF-8; show_components jumps straight to
 * the Select Components page (used by the screenshot harness). */
int ui_run(HINSTANCE hInst, int nCmdShow, const char *source_dir,
           int show_components);

#ifdef __cplusplus
}
#endif

#endif
