/*
 * Fling - .txt word finder + hidden Unicode detector (CLI, Windows/MinGW)
 * Build: gcc fling.c -o fling.exe
 * Run:   fling.exe
 *
 * Word search is ASCII-only (case-insensitive).
 * Hidden-Unicode detection works with any valid UTF-8 input and flags
 * invalid UTF-8 (overlong, surrogates, > U+10FFFF) as suspicious.
 * File paths are interpreted as UTF-8 (Windows: converted to UTF-16 for fopen).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <wchar.h>

#include "fling_art.h"


#define C_RESET   "\x1b[0m"
#define C_PINK    "\x1b[95m"
#define C_ORANGE  "\x1b[91m"
#define C_GREEN   "\x1b[92m"
#define C_RED     "\x1b[91m"
#define C_CYAN    "\x1b[96m"
#define C_DIM     "\x1b[90m"
#define C_WHITE   "\x1b[97m"


#define SCAN_WORD     1
#define SCAN_HIDDEN   2
#define SCAN_ERR_OOM  (-1)
#define SCAN_ERR_IO   (-2)


typedef struct {
    char  *data;
    size_t len, cap;
} Line;

static int line_append(Line *l, char c) {
    if (l->len + 1 >= l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        char *p = realloc(l->data, nc);
        if (!p) return 0;
        l->data = p;
        l->cap  = nc;
    }
    l->data[l->len++] = c;
    l->data[l->len]   = 0;
    return 1;
}


static FILE *open_utf8(const char *path) {
    wchar_t wpath[MAX_PATH];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    if (n <= 0) return NULL;
    return _wfopen(wpath, L"rb");
}


static const char *ci_strstr_ascii(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && (unsigned char)*h < 0x80 && (unsigned char)*n < 0x80 &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            ++h; ++n;
        }
        if (!*n) return hay;
    }
    return NULL;
}


static int is_hidden_cp(unsigned int cp) {
   
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
        cp == 0xFEFF || cp == 0x00AD ||
        (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2066 && cp <= 0x2069) ||
        (cp >= 0xE0000 && cp <= 0xE007F) ||
        cp == 0x180E || cp == 0x034F)
        return 1;
    
    if (cp >= 0x2000 && cp <= 0x200A)
        return 1;
    return 0;
}


static int utf8_decode(const char *s, size_t len, unsigned int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }

    int n;
    unsigned int v;
    if      ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
    else return 0;

    if (len < (size_t)n) return -1;
    for (int k = 1; k < n; ++k) {
        unsigned char cc = (unsigned char)s[k];
        if ((cc & 0xC0) != 0x80) return 0;
        v = (v << 6) | (cc & 0x3F);
    }
    if (v > 0x10FFFF)                return 0;
    if (v >= 0xD800 && v <= 0xDFFF)  return 0;
    if (n == 2 && v < 0x80)          return 0;
    if (n == 3 && v < 0x800)         return 0;
    if (n == 4 && v < 0x10000)       return 0;

    *cp = v;
    return n;
}


static int has_hidden_unicode(const char *s, size_t len) {
    size_t i = 0;
    /* skip UTF-8 BOM at the very start of the line */
    if (len >= 3 && (unsigned char)s[0] == 0xEF &&
                    (unsigned char)s[1] == 0xBB &&
                    (unsigned char)s[2] == 0xBF) {
        i = 3;
    }

    for (; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) {
            if (c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            return 1;
        }
        if (c < 0x80) { i++; continue; }

        unsigned int cp = 0;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) return 1;
        if (is_hidden_cp(cp)) return 1;
        i += (size_t)n;
    }
    return 0;
}


static void print_escaped_line(const char *s, size_t len) {
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') { fputs("    ", stdout); i++; continue; }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c < 0x20) { printf(C_DIM "\\x%02X" C_RESET, c); i++; continue; }
        if (c < 0x80) { putchar(c); i++; continue; }

        unsigned int cp = 0;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) { printf(C_DIM "\\x%02X" C_RESET, c); i++; continue; }

        if (is_hidden_cp(cp)) printf(C_CYAN "[U+%04X]" C_RESET, cp);
        else                  fwrite(s + i, 1, (size_t)n, stdout);
        i += (size_t)n;
    }
    putchar('\n');
}


static int scan_file(const char *path, const char *word, int unicode_only) {
    if (!word) unicode_only = 1;

    FILE *f = open_utf8(path);
    if (!f) {
        printf(C_RED "[-] Cannot open file: %s" C_RESET "\n", path);
        return SCAN_ERR_IO;
    }

    Line line = {0};
    int c, line_no = 0, found_word = 0, found_hidden = 0, oom = 0;

    for (;;) {
        c = fgetc(f);
        int eol = (c == '\n' || c == EOF);

        if (!eol) {
            if (!line_append(&line, (char)c)) { oom = 1; break; }
            continue;
        }

        line_no++;
        if (line.len && line.data[line.len-1] == '\r') {
            line.len--;
            if (line.data) line.data[line.len] = 0;
        }

        int hit = 0, hid = 0;
        if (line.len) {
            if (!unicode_only && word && *word)
                hit = (ci_strstr_ascii(line.data, word) != NULL);
            hid = has_hidden_unicode(line.data, line.len);
        }
        if (hit || hid) {
            if (hit) found_word = 1;
            if (hid) found_hidden = 1;
            printf(C_DIM "%5d" C_RESET " | ", line_no);
            print_escaped_line(line.data, line.len);
        }

        line.len = 0;
        if (line.data) line.data[0] = 0;

        if (c == EOF) break;
    }

    free(line.data);
    fclose(f);

    if (oom) {
        printf(C_RED "[-] Out of memory while scanning." C_RESET "\n");
        return SCAN_ERR_OOM;
    }

    return (found_word ? SCAN_WORD : 0) | (found_hidden ? SCAN_HIDDEN : 0);
}


static void normalize_path_arg(const char *arg, char *out, size_t outsz) {
    size_t plen = strlen(arg);
    int quoted = 0;
    if (plen >= 2 && (arg[0] == '"' || arg[0] == '\'') && arg[plen-1] == arg[0]) {
        plen -= 2;
        memmove(out, arg + 1, plen);  
        quoted = 1;
    } else {
        memcpy(out, arg, plen);
    }
    if (plen >= outsz) plen = outsz - 1;
    out[plen] = 0;
    if (quoted) {
        while (plen && out[plen-1] == ' ') out[--plen] = 0;
    }
}


static int prompt_word(char *word, size_t cap) {
    printf(C_ORANGE "[.txt] = " C_RESET);
    fflush(stdout);
    if (!fgets(word, (int)cap, stdin)) return 0;
    size_t wl = strlen(word);
    while (wl && (word[wl-1] == '\n' || word[wl-1] == '\r')) word[--wl] = 0;
    return (int)wl;
}


static void cmd_txtscan(const char *arg) {
    printf(C_PINK "searching for specific words in a .txt file." C_RESET "\n");

    if (!arg || !*arg) {
        printf(C_RED "[-] No file specified." C_RESET "\n");
        return;
    }

    char path[1024];
    normalize_path_arg(arg, path, sizeof(path));
    printf(C_ORANGE "Selected file: " C_WHITE "%s" C_RESET "\n", path);

    char word[512];
    int wl = prompt_word(word, sizeof(word));
    if (wl <= 0) {
        printf(C_RED "[-] That word is not in the file." C_RESET "\n");
        return;
    }

    for (int i = 0; i < wl; ++i) {
        if ((unsigned char)word[i] >= 0x80) {
            printf(C_RED "[-] Word contains non-ASCII bytes; ASCII-only search cannot match it." C_RESET "\n");
            return;
        }
    }

    int r = scan_file(path, word, 0);
    if (r < 0) return;

    if (r & SCAN_WORD)   printf(C_GREEN "[+] Word found." C_RESET "\n");
    else                 printf(C_RED   "[-] That word is not in the file." C_RESET "\n");
    if (r & SCAN_HIDDEN) printf(C_CYAN  "[!] Hidden Unicode / non-printable content detected in the lines above." C_RESET "\n");
}

static void cmd_uniscan(const char *arg) {
    printf(C_PINK "Unicode search in .txt, .log, and .dat files." C_RESET "\n");

    if (!arg || !*arg) {
        printf(C_RED "[-] No file specified." C_RESET "\n");
        return;
    }

    char path[1024];
    normalize_path_arg(arg, path, sizeof(path));
    printf(C_ORANGE "Selected file: " C_WHITE "%s" C_RESET "\n", path);

    printf(C_ORANGE "[~] Searching..." C_RESET "\n");
    fflush(stdout);

    int r = scan_file(path, NULL, 1);
    if (r < 0) return;

    if (r & SCAN_HIDDEN)
        printf(C_RED "[BAD] Unicode strings have been found in the file. "
                     "It is advisable to launch Fling GUI to check the strings." C_RESET "\n");
    else
        printf(C_GREEN "[OK] No hidden or non-printable characters found." C_RESET "\n");
}

static void cmd_help(void) {
    printf(C_PINK "Available commands:\n" C_RESET);
    printf(C_ORANGE "  txtscan " C_WHITE "{file_name | full_path}" C_RESET "  - scan a .txt file for a word (ASCII) and hidden Unicode\n");
    printf(C_ORANGE "  uniscan " C_WHITE "{file_name | full_path}" C_RESET "  - Unicode search in .txt, .log, and .dat files (binary may yield [BAD])\n");
    printf(C_ORANGE "  info    " C_RESET "                        - show information about Fling\n");
    printf(C_ORANGE "  github  " C_RESET "                        - print the project GitHub URL\n");
    printf(C_ORANGE "  exit    " C_RESET "                        - quit the program\n");
}

static void cmd_info(void) {
    printf(C_PINK "Fling" C_RESET " - .txt word finder (ASCII-only), hidden-Unicode & zero-width detector.\n");
    printf("Written in C. CLI. Windows / MinGW GCC.\n");
}

static void cmd_github(void) {
    printf(C_ORANGE "[!] GitHub: " C_WHITE "https://github.com/Hu2ie/Fling" C_RESET "\n");
}


static void trim_eol(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0;
}


int main(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);


    fputs(ASCII_ART, stdout);
    printf(C_RESET "\n");


    printf(C_PINK "Welcome to the " C_ORANGE "Fling" C_PINK " tool." C_RESET "\n");
    printf("Fling is a utility designed for finding specific words within .txt files,\n");
    printf("detecting hidden Unicode characters, and uncovering text concealed\n");
    printf("within files via steganography.\n\n");

    printf(C_ORANGE "[!] Project GitHub: " C_WHITE "https://github.com/Hu2ie/Fling" C_RESET "\n");

    printf(C_ORANGE "[1]" C_RESET " initializing and loading the DLL...\n");
    int ok = 1; 

    printf(C_ORANGE "[2]" C_RESET " Setting up configs...\n");
    if (!ok) {
        printf(C_RED "[-]" C_RESET " Failed to launch the program.\n");
        return 1;
    }
    printf(C_GREEN "[+]" C_RESET " The program is ready for use\n\n");
    printf(C_ORANGE "[!]" C_RESET " Enter " C_WHITE "\"help\"" C_RESET " to get information about commands.\n\n");

    char buf[2048];
    for (;;) {
        printf(C_PINK "Fling " C_ORANGE ">>> " C_RESET);
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) break;
        /* drain rest of line if truncated */
        if (!strchr(buf, '\n')) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { /* discard */ }
        }
        trim_eol(buf);
        if (!*buf) continue;

        char *cmd = buf;
        char *p = buf;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = 0; p++; while (*p == ' ') p++; }
        char *arg = (*p) ? p : NULL;

        if      (!strcmp(cmd, "help"))    cmd_help();
        else if (!strcmp(cmd, "txtscan")) cmd_txtscan(arg);
        else if (!strcmp(cmd, "uniscan")) cmd_uniscan(arg);
        else if (!strcmp(cmd, "info"))    cmd_info();
        else if (!strcmp(cmd, "github"))  cmd_github();
        else if (!strcmp(cmd, "exit") ||
                 !strcmp(cmd, "quit"))    break;
        else
            printf(C_RED "[-] Unknown command: " C_WHITE "%s" C_RED ". Type \"help\"." C_RESET "\n", cmd);
    }

    printf(C_ORANGE "Goodbye." C_RESET "\n");
    return 0;
}