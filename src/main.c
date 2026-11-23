#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

#include "app.h"
#include "core.h"
#include "ui.h"

/* Locate the directory that holds the .bin payload slices.  Setup is a
 * single file placed next to them, but fall back to the parent directory if
 * the immediate directory has no *.bin files. */
static void find_source_dir(char *out, size_t outlen)
{
    wchar_t path[MAX_PATH];
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    out[0] = '\0';
    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return;
    PathRemoveFileSpecW(path);

    swprintf(pattern, MAX_PATH, L"%s\\*.bin", path);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
    } else {
        wchar_t parent[MAX_PATH];
        wcsncpy_s(parent, MAX_PATH, path, _TRUNCATE);
        if (PathRemoveFileSpecW(parent) && wcscmp(parent, path) != 0)
            wcsncpy_s(path, MAX_PATH, parent, _TRUNCATE);
    }

    WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)outlen, NULL, NULL);
}

/* GUI apps do not get a usable stdout by default.  Prefer inherited handles
 * (so a harness redirecting stdout still captures output), otherwise attach
 * to the parent console. */
static void setup_console(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE *fp = NULL;
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
        int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
        if (fd >= 0) {
            _dup2(fd, 1);
            _dup2(fd, 2);
            _close(fd);
            return;
        }
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
}

static long long cli_done, cli_total;
static int cli_last_pct = -1;

static void cli_progress(int done_bytes, int total_bytes, void *user)
{
    int pct;
    (void)user;
    cli_done = done_bytes;
    cli_total = total_bytes;
    if (total_bytes <= 0) return;
    pct = (int)(((long long)done_bytes * 100) / total_bytes);
    if (pct != cli_last_pct && (pct % 10 == 0 || pct == 100)) {
        cli_last_pct = pct;
        printf("  %d%% (%lld / %lld bytes)\n", pct,
               (long long)done_bytes, (long long)total_bytes);
        fflush(stdout);
    }
}

static int run_verify_only(const char *source_dir)
{
    char err[2048];
    int rc;
    err[0] = 0;
    printf("OP-2.2 MT verify-only\n");
    printf("source: %s\n", source_dir);
    rc = op22_verify_only(source_dir, cli_progress, NULL, err, sizeof(err));
    printf("result: %s (%lld / %lld bytes processed)\n",
           rc == 0 ? "OK" : "FAILED", cli_done, cli_total);
    if (err[0]) printf("details: %s\n", err);
    fflush(stdout);
    return rc == 0 ? 0 : 1;
}

/* Resolve a comma-separated list of component names to a bitmask.  Parents
 * of selected components are included, and exclusive siblings are collapsed
 * to a single choice (the last one listed), just like the wizard's radios. */
static unsigned parse_components(const wchar_t *list)
{
    unsigned mask = 0;
    wchar_t buf[512];
    const wchar_t *p = list;
    while (*p) {
        const wchar_t *end = wcschr(p, L',');
        size_t n = end ? (size_t)(end - p) : wcslen(p);
        char name[256];
        int i, found = -1;
        if (n >= sizeof(buf) / sizeof(buf[0])) n = sizeof(buf) / sizeof(buf[0]) - 1;
        wcsncpy_s(buf, sizeof(buf) / sizeof(buf[0]), p, n);
        buf[n] = 0;
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, name, sizeof(name), NULL, NULL);
        for (i = 0; i < g_manifest.ncomp; ++i)
            if (_stricmp(g_manifest.comps[i].name, name) == 0) { found = i; break; }
        if (found < 0) {
            printf("warning: unknown component '%s'\n", name);
        } else {
            mask |= (1u << found);
            /* Include every ancestor so the child is reachable in the UI. */
            while (g_manifest.comps[found].parent && g_manifest.comps[found].parent[0]) {
                const char *par = g_manifest.comps[found].parent;
                for (i = 0; i < g_manifest.ncomp; ++i)
                    if (strcmp(g_manifest.comps[i].name, par) == 0) {
                        mask |= (1u << i);
                        found = i;
                        break;
                    }
                if (i == g_manifest.ncomp) break;
            }
        }
        if (!end) break;
        p = end + 1;
    }
    /* Collapse exclusive siblings: keep the last one that was selected. */
    {
        int a, b;
        for (a = 0; a < g_manifest.ncomp; ++a) {
            if (!(g_manifest.comps[a].flags & 2)) continue;
            for (b = a + 1; b < g_manifest.ncomp; ++b) {
                if (!(g_manifest.comps[b].flags & 2)) continue;
                if (g_manifest.comps[a].parent && g_manifest.comps[b].parent &&
                    strcmp(g_manifest.comps[a].parent, g_manifest.comps[b].parent) == 0 &&
                    (mask & (1u << a)) && (mask & (1u << b)))
                    mask &= ~(1u << a);
            }
        }
    }
    return mask;
}

static int run_install_all(const char *source_dir, const wchar_t *output_wide,
                           unsigned comp_mask)
{
    char out[MAX_PATH];
    char err[2048];
    unsigned task_mask = (1u << OP22_NTASKS) - 1u; /* all tasks */
    int rc, rc2;

    WideCharToMultiByte(CP_UTF8, 0, output_wide, -1, out, MAX_PATH, NULL, NULL);
    err[0] = 0;
    printf("OP-2.2 MT install (component mask 0x%X)\n", comp_mask);
    printf("source: %s\noutput: %s\n", source_dir, out);
    rc = op22_install(source_dir, out, comp_mask, task_mask,
                      cli_progress, NULL, err, sizeof(err));
    printf("install: %s (%lld / %lld bytes processed)\n",
           rc == 0 ? "OK" : "FAILED", cli_done, cli_total);
    if (err[0]) printf("details: %s\n", err);
    if (rc != 0) {
        fflush(stdout);
        return 1;
    }
    err[0] = 0;
    rc2 = op22_create_shortcuts(out, task_mask, err, sizeof(err));
    printf("shortcuts: %s\n", rc2 == 0 ? "OK" : "WARNING");
    if (err[0]) printf("details: %s\n", err);
    fflush(stdout);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine,
                    int nCmdShow)
{
    char source_dir[MAX_PATH];
    int show_components = 0;
    int no_wizard = 0;
    int verify_only = 0;
    const wchar_t *output_dir = NULL;
    const wchar_t *source_override = NULL;
    const wchar_t *components = NULL;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int i;

    if (argv) {
        for (i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--show-components") == 0)
                show_components = 1;
            else if (_wcsicmp(argv[i], L"--no-wizard") == 0)
                no_wizard = 1;
            else if (_wcsicmp(argv[i], L"--verify-only") == 0)
                verify_only = 1;
            else if (_wcsicmp(argv[i], L"--output-dir") == 0 && i + 1 < argc)
                output_dir = argv[++i];
            else if (_wcsicmp(argv[i], L"--source-dir") == 0 && i + 1 < argc)
                source_override = argv[++i];
            else if (_wcsicmp(argv[i], L"--components") == 0 && i + 1 < argc)
                components = argv[++i];
        }
        LocalFree(argv);
    }

    if (source_override)
        WideCharToMultiByte(CP_UTF8, 0, source_override, -1,
                            source_dir, MAX_PATH, NULL, NULL);
    else
        find_source_dir(source_dir, sizeof(source_dir));

    if (verify_only) {
        setup_console();
        return run_verify_only(source_dir);
    }
    if (no_wizard) {
        setup_console();
        if (!output_dir) {
            printf("usage: OP-2.2 MT.exe --no-wizard --output-dir <path> "
                   "[--source-dir <path>] [--components name1,name2,...]\n");
            fflush(stdout);
            return 2;
        }
        return run_install_all(source_dir, output_dir,
                               components ? parse_components(components) : ~0u);
    }

    return ui_run(hInst, nCmdShow, source_dir, show_components);
}
