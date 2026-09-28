/*
 * mdzy - tiny, instant Markdown & plain-text viewer for Windows.
 *
 * One C file, Win32 + RichEdit (msftedit.dll), no runtime dependencies.
 * Markdown is converted to RTF by a small hand-written parser and streamed
 * into a read-only RichEdit control; plain text is streamed in as-is.
 */
#define _WIN32_WINNT 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <richedit.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "uuid.lib")

#define APP_NAME     L"mdzy"
#include "version.h"
#define APP_VERSION  L"" MDZY_VERSION
#define WND_CLASS    L"mdzy_main_window"
#define IDI_APP      101

#ifndef AURL_ENABLEURL
#define AURL_ENABLEURL 1
#endif
#ifndef AURL_ENABLEEMAILADDR
#define AURL_ENABLEEMAILADDR 2
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

enum {
    CMD_OPEN = 100, CMD_RELOAD, CMD_CLOSE, CMD_FIND, CMD_FINDNEXT, CMD_FINDPREV,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_RAW, CMD_THEME, CMD_THEME_AUTO,
    CMD_THEME_LIGHT, CMD_THEME_DARK, CMD_WRAP, CMD_TOPMOST, CMD_BACK, CMD_FORWARD,
    CMD_EDIT, CMD_FOLDER, CMD_COPYPATH, CMD_REGISTER, CMD_UNREGISTER, CMD_HELP,
    CMD_COPY, CMD_SELECTALL
};

#define TIMER_WATCH    1
#define TIMER_RELAYOUT 2

/* ======================================================================
 * Growable byte buffer
 * ====================================================================== */

typedef struct { char *p; size_t n, cap; } Buf;

static void bgrow(Buf *b, size_t add) {
    if (b->n + add + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->n + add + 1) nc *= 2;
    b->p = (char *)realloc(b->p, nc);
    b->cap = nc;
}
static void bput(Buf *b, const char *s, size_t n) {
    bgrow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bputc(Buf *b, char c) {
    bgrow(b, 1);
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}
static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if (k >= (int)sizeof tmp) k = (int)sizeof tmp - 1;
    bput(b, tmp, (size_t)k);
}
static void bhex(Buf *b, const unsigned char *d, size_t n) {
    static const char hx[] = "0123456789abcdef";
    bgrow(b, n * 2 + n / 64 + 2);
    char *o = b->p + b->n;
    for (size_t i = 0; i < n; i++) {
        *o++ = hx[d[i] >> 4];
        *o++ = hx[d[i] & 15];
        if ((i & 63) == 63) *o++ = '\n';
    }
    b->n = (size_t)(o - b->p);
    b->p[b->n] = 0;
}

/* ======================================================================
 * Small helpers
 * ====================================================================== */

static WCHAR *utf8_to_wide(const char *s, int n) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    WCHAR *w = (WCHAR *)malloc(((size_t)len + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, n, w, len);
    w[len] = 0;
    return w;
}
static char *wide_to_utf8(const WCHAR *w, int n, int *outLen) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)len + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, n, s, len, NULL, NULL);
    s[len] = 0;
    if (outLen) *outLen = len;
    return s;
}

static unsigned char *read_file(const WCHAR *path, size_t *outN, size_t limit) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || (unsigned long long)sz.QuadPart > limit) { CloseHandle(h); return NULL; }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *d = (unsigned char *)malloc(n + 2);
    size_t got = 0;
    while (got < n) {
        DWORD rd = 0, want = (DWORD)((n - got) > (1u << 30) ? (1u << 30) : (n - got));
        if (!ReadFile(h, d + got, want, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    d[got] = 0; d[got + 1] = 0;
    *outN = got;
    return d;
}

static int starts_ci(const char *s, int n, const char *p) {
    int k = (int)strlen(p);
    if (n < k) return 0;
    for (int i = 0; i < k; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)p[i])) return 0;
    return 1;
}
static int find_str(const char *s, int n, int from, const char *needle) {
    int k = (int)strlen(needle);
    for (int i = from; i + k <= n; i++)
        if (s[i] == needle[0] && !memcmp(s + i, needle, (size_t)k)) return i;
    return -1;
}
static int find_str_ci(const char *s, int n, int from, const char *needle) {
    int k = (int)strlen(needle);
    for (int i = from; i + k <= n; i++)
        if (starts_ci(s + i, n - i, needle)) return i;
    return -1;
}
static int run_of(const char *s, int n, int i, char ch) {
    int k = 0;
    while (i + k < n && s[i + k] == ch) k++;
    return k;
}
static int is_punct(int c) { return c > 32 && c < 127 && !isalnum(c); }
static int is_ws(int c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static int is_word(int c) { return c >= 0x80 || isalnum(c); }

static unsigned utf8_next(const unsigned char *s, int n, int *len) {
    unsigned c = s[0];
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && n >= 2) { *len = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && n >= 3) { *len = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && n >= 4) {
        *len = 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;
}

/* Percent-decode (UTF-8 aware) into a new wide string. */
static WCHAR *url_decode_wide(const char *s, int n) {
    Buf b = {0};
    for (int i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char h[3] = { s[i + 1], s[i + 2], 0 };
            bputc(&b, (char)strtol(h, NULL, 16));
            i += 2;
        } else {
            bputc(&b, s[i]);
        }
    }
    WCHAR *w = utf8_to_wide(b.p ? b.p : "", (int)b.n);
    free(b.p);
    return w;
}

/* ======================================================================
 * Colors
 * ====================================================================== */

enum {
    C_TEXT = 1, C_MUTED, C_LINK, C_CODEBG, C_BORDER, C_INLINEBG, C_STRIPE, C_QUOTE,
    C_KW, C_STR, C_COM, C_NUM, C_NOTE, C_TIP, C_IMPORTANT, C_WARNING, C_CAUTION, C_BG,
    C_COUNT
};

static const DWORD kLight[C_COUNT] = {
    0,
    0x1f2328, 0x59636e, 0x0969da, 0xf6f8fa, 0xd1d9e0, 0xeff1f3, 0xf6f8fa, 0xd1d9e0,
    0xcf222e, 0x0a3069, 0x59636e, 0x0550ae,
    0x0969da, 0x1a7f37, 0x8250df, 0x9a6700, 0xcf222e,
    0xffffff
};
/* RichEdit forces all text in a table cell to white when the cell shading is darker than about
 * RGB(45,51,59), which would wipe out syntax colors; dark code blocks therefore use #30363d
 * and dark tables are not shaded at all. */
static const DWORD kDark[C_COUNT] = {
    0,
    0xe6edf3, 0x9198a1, 0x4493f8, 0x30363d, 0x3d444d, 0x30363d, 0x30363d, 0x3d444d,
    0xff7b72, 0xa5d6ff, 0x9198a1, 0x79c0ff,
    0x4493f8, 0x3fb950, 0xab7df8, 0xd29922, 0xf85149,
    0x161b22
};
#define HEXRGB(x) RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)

/* ======================================================================
 * Markdown -> RTF
 * ====================================================================== */

typedef struct { const char *s; int n; int skip; } Line;
typedef struct { char *label; int ln; char *url; } Ref;
typedef struct { char *id; char *text; } Foot;
typedef struct { WCHAR *slug; WCHAR *text; } Head;

typedef struct {
    int indent;     /* left indent, twips */
    int quote;      /* inside blockquote */
    int intbl;      /* inside a table cell (blockquote) */
    int tight;      /* tight list paragraph spacing */
    int alert;      /* GitHub alert type (0 = none) */
} Ctx;

typedef struct {
    Buf out;
    int W;                  /* usable width at indent 0, twips */
    int B;                  /* body font size, half-points */
    const DWORD *pal;
    int dark;
    const WCHAR *baseDir;
    Ref *refs; int nrefs, caprefs;
    Foot *foots; int nfoots, capfoots;
    Head *heads; int nheads, capheads;
    int open;               /* a paragraph is open (needs \par or \cell) */
    int pend;               /* vertical space (twips) owed before the next block */
    int nblocks;
    int depth;              /* list nesting */
    int mkOn, mkHang;       /* pending list marker */
    unsigned mkCp;
    char mkTxt[16];
    int inLink, btn;
    int curIndent, curInTbl;
    int margin;             /* page margin, twips (content spans margin..W) */
} R;

static void blocks(R *r, Line *L, int n, Ctx c);
static void inl(R *r, const char *s, int n);

/* ---- RTF text output ---- */

static void rtf_cp(Buf *o, unsigned cp) {
    if (cp < 0x80) {
        if (cp == '\\' || cp == '{' || cp == '}') { bputc(o, '\\'); bputc(o, (char)cp); }
        else if (cp == '\t') bputs(o, "\\tab ");
        else if (cp >= 32) bputc(o, (char)cp);
        return;
    }
    if (cp == 0xA0) { bputs(o, "\\~"); return; }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        bprintf(o, "\\u%d?\\u%d?", (int)(short)(0xD800 + (cp >> 10)), (int)(short)(0xDC00 + (cp & 0x3FF)));
        return;
    }
    bprintf(o, "\\u%d?", (int)(short)cp);
}

/* Plain text; newlines become spaces (or \line when codeMode). */
static void rtf_text_ex(Buf *o, const char *s, int n, int codeMode) {
    int i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            int j = i;
            while (j < n && (unsigned char)s[j] >= 32 && (unsigned char)s[j] < 0x80 &&
                   s[j] != '\\' && s[j] != '{' && s[j] != '}')
                j++;
            if (j > i) { bput(o, s + i, (size_t)(j - i)); i = j; continue; }
            if (c == '\n') bputs(o, codeMode ? "\\line " : " ");
            else if (c != '\r') rtf_cp(o, c);
            i++;
            continue;
        }
        int len;
        unsigned cp = utf8_next((const unsigned char *)s + i, n - i, &len);
        rtf_cp(o, cp);
        i += len;
    }
}
static void rtf_text(Buf *o, const char *s, int n) { rtf_text_ex(o, s, n, 0); }

/* ---- plain-text extraction (for widths, anchors) ---- */

static int find_bracket(const char *s, int n, int p);

static void md_plain(const char *s, int n, Buf *out) {
    int i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && is_punct((unsigned char)s[i + 1])) { bputc(out, s[i + 1]); i += 2; continue; }
        if (c == '*' || c == '_' || c == '~' || c == '`') { i++; continue; }
        if (c == '!' && i + 1 < n && s[i + 1] == '[') { i++; continue; }
        if (c == '[') {
            int e = find_bracket(s, n, i);
            if (e > i) {
                md_plain(s + i + 1, e - i - 1, out);
                i = e + 1;
                if (i < n && s[i] == '(') {
                    int d = 0;
                    while (i < n) {
                        if (s[i] == '(') d++;
                        else if (s[i] == ')' && --d == 0) { i++; break; }
                        i++;
                    }
                } else if (i < n && s[i] == '[') {
                    int e2 = find_str(s, n, i, "]");
                    i = e2 < 0 ? n : e2 + 1;
                }
                continue;
            }
        }
        if (c == '<' && i + 1 < n && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
            int e = find_str(s, n, i, ">");
            if (e > 0) { i = e + 1; continue; }
        }
        bputc(out, c);
        i++;
    }
}

static int plain_len(const char *s, int n) {
    Buf b = {0};
    md_plain(s, n, &b);
    int cnt = 0;
    for (size_t i = 0; i < b.n; i++)
        if (((unsigned char)b.p[i] & 0xC0) != 0x80) cnt++;
    free(b.p);
    return cnt;
}

/* ---- inline scanning helpers ---- */

static int find_bt(const char *s, int n, int from, int m) {
    int j = from;
    while (j < n) {
        if (s[j] == '`') {
            int k = run_of(s, n, j, '`');
            if (k == m) return j;
            j += k;
        } else {
            j++;
        }
    }
    return -1;
}

static int find_bracket(const char *s, int n, int p) {
    int depth = 0;
    for (int j = p; j < n; j++) {
        char c = s[j];
        if (c == '\\') { j++; continue; }
        if (c == '`') {
            int m = run_of(s, n, j, '`');
            int e = find_bt(s, n, j + m, m);
            j = (e >= 0 ? e + m : j + m) - 1;
            continue;
        }
        if (c == '[') depth++;
        else if (c == ']' && --depth == 0) return j;
    }
    return -1;
}

static int find_closer(const char *s, int n, int from, char ch, int k) {
    for (int j = from; j < n; j++) {
        char c = s[j];
        if (c == '\\') { j++; continue; }
        if (c == '`') {
            int m = run_of(s, n, j, '`');
            int e = find_bt(s, n, j + m, m);
            j = (e >= 0 ? e + m : j + m) - 1;
            continue;
        }
        if (c != ch) continue;
        int m = run_of(s, n, j, ch);
        if (m >= k && j > from && !is_ws((unsigned char)s[j - 1])) {
            if (ch == '_' && j + m < n && is_word((unsigned char)s[j + m])) { j += m - 1; continue; }
            if (m == k) return j;
            if (m == 3) return j + m - k;
        }
        j += m - 1;
    }
    return -1;
}

/* ---- links & images ---- */

static void link_open(R *r, const char *url, int un) {
    Buf *o = &r->out;
    bputs(o, "{\\field{\\*\\fldinst{HYPERLINK \"");
    for (int i = 0; i < un;) {
        unsigned char c = (unsigned char)url[i];
        if (c == '"') { bputs(o, "%22"); i++; continue; }
        if (c == ' ') { bputs(o, "%20"); i++; continue; }
        if (c == '\\' && i + 1 < un && is_punct((unsigned char)url[i + 1])) { rtf_cp(o, (unsigned char)url[i + 1]); i += 2; continue; }
        if (c < 0x80) { rtf_cp(o, c); i++; continue; }
        int len;
        unsigned cp = utf8_next((const unsigned char *)url + i, un - i, &len);
        rtf_cp(o, cp);
        i += len;
    }
    /* The trailing space keeps the instruction different from the friendly text: RichEdit
     * restyles links whose text equals their URL with its own (theme-unaware) colors. */
    if (starts_ci(url, un, "mdzy:"))   /* app command: drawn as a button */
        bprintf(o, " \"}}{\\fldrslt{\\b\\cf%d\\highlight%d\\~\\~ ", C_BG, C_LINK);
    else
        bprintf(o, " \"}}{\\fldrslt{\\cf%d\\ul ", C_LINK);
    r->btn = starts_ci(url, un, "mdzy:");
}
static void link_close(R *r) {
    bputs(&r->out, r->btn ? " \\~\\~}}}" : "}}}");
    r->btn = 0;
}

static Ref *find_ref(R *r, const char *lab, int ln) {
    for (int i = 0; i < r->nrefs; i++) {
        Ref *f = &r->refs[i];
        if (f->ln != ln) continue;
        int ok = 1;
        for (int k = 0; k < ln && ok; k++)
            if (tolower((unsigned char)f->label[k]) != tolower((unsigned char)lab[k])) ok = 0;
        if (ok) return f;
    }
    return NULL;
}

static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static int be32(const unsigned char *p) { return (int)(((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]); }

static const char *img_probe(const unsigned char *d, size_t n, int *w, int *h) {
    if (n > 24 && !memcmp(d, "\x89PNG", 4)) {
        *w = be32(d + 16); *h = be32(d + 20);
        return "pngblip";
    }
    if (n > 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t p = 2;
        while (p + 9 < n) {
            if (d[p] != 0xFF) { p++; continue; }
            unsigned char m = d[p + 1];
            if (m == 0xFF) { p++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
            int len = be16(d + p + 2);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = be16(d + p + 5); *w = be16(d + p + 7);
                return "jpegblip";
            }
            p += 2 + (size_t)len;
        }
    }
    return NULL;
}

/* Any other WIC-decodable format (GIF, BMP, WebP, TIFF, ICO...) -> PNG bytes. */
static unsigned char *wic_to_png(const WCHAR *path, size_t *outN, int *w, int *h) {
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IStream *st = NULL;
    unsigned char *res = NULL;
    UINT W = 0, H = 0;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&f))) return NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) goto done;
    if (FAILED(IWICBitmapFrameDecode_GetSize(fr, &W, &H)) || !W || !H) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) goto done;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &st))) goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc))) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &fe, NULL))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(fe, NULL))) goto done;
    IWICBitmapFrameEncode_SetSize(fe, W, H);
    IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (FAILED(IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)cv, NULL))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(fe)) || FAILED(IWICBitmapEncoder_Commit(enc))) goto done;
    {
        STATSTG ss;
        HGLOBAL hg;
        if (SUCCEEDED(IStream_Stat(st, &ss, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(st, &hg))) {
            size_t n = (size_t)ss.cbSize.QuadPart;
            void *p = GlobalLock(hg);
            if (p) {
                res = (unsigned char *)malloc(n);
                memcpy(res, p, n);
                GlobalUnlock(hg);
                *outN = n; *w = (int)W; *h = (int)H;
            }
        }
    }
done:
    if (fe) IWICBitmapFrameEncode_Release(fe);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IStream_Release(st);
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(f);
    return res;
}

/* Resolve a Markdown link/image target to a local file path. */
static int resolve_local(const WCHAR *baseDir, const char *u, int un, WCHAR *out, int cap, WCHAR *frag, int fragCap) {
    if (frag) frag[0] = 0;
    while (un > 0 && is_ws((unsigned char)*u)) { u++; un--; }
    if (un <= 0) return 0;
    if (starts_ci(u, un, "http:") || starts_ci(u, un, "https:") || starts_ci(u, un, "data:") ||
        starts_ci(u, un, "mailto:") || starts_ci(u, un, "ftp:") || (un > 1 && u[0] == '/' && u[1] == '/'))
        return 0;
    if (starts_ci(u, un, "file:///")) { u += 8; un -= 8; }
    else if (starts_ci(u, un, "file://")) { u += 7; un -= 7; }
    int cut = un;
    for (int i = 0; i < un; i++)
        if (u[i] == '?' || u[i] == '#') {
            if (u[i] == '#' && frag) {
                WCHAR *f = url_decode_wide(u + i + 1, un - i - 1);
                lstrcpynW(frag, f, fragCap);
                free(f);
            }
            cut = i;
            break;
        }
    if (cut == 0) return 0;
    WCHAR *rel = url_decode_wide(u, cut);
    for (WCHAR *p = rel; *p; p++) if (*p == '/') *p = '\\';
    WCHAR tmp[MAX_PATH * 2];
    int isAbs = (rel[0] && rel[1] == ':') || (rel[0] == '\\' && rel[1] == '\\');
    if (isAbs) {
        lstrcpynW(tmp, rel, ARRAYSIZE(tmp));
    } else {
        WCHAR root[MAX_PATH * 2];
        lstrcpynW(root, baseDir ? baseDir : L".", ARRAYSIZE(root));
        const WCHAR *r2 = rel;
        if (rel[0] == '\\') {
            /* "/path" is relative to the repository root, like on GitHub. */
            WCHAR probe[MAX_PATH * 2];
            lstrcpynW(probe, root, ARRAYSIZE(probe));
            for (;;) {
                WCHAR git[MAX_PATH * 2];
                swprintf(git, ARRAYSIZE(git), L"%ls\\.git", probe);
                if (GetFileAttributesW(git) != INVALID_FILE_ATTRIBUTES) { lstrcpynW(root, probe, ARRAYSIZE(root)); break; }
                if (!PathRemoveFileSpecW(probe) || !probe[0]) break;
            }
            while (*r2 == '\\') r2++;
        }
        swprintf(tmp, ARRAYSIZE(tmp), L"%ls\\%ls", root, r2);
    }
    free(rel);
    DWORD k = GetFullPathNameW(tmp, (DWORD)cap, out, NULL);
    return k > 0 && k < (DWORD)cap;
}

static void emit_image(R *r, const char *src, int sn, const char *alt, int an, const char *wantW, int wn) {
    Buf *o = &r->out;
    WCHAR path[MAX_PATH * 2];
    if (resolve_local(r->baseDir, src, sn, path, ARRAYSIZE(path), NULL, 0)) {
        size_t n = 0;
        unsigned char *d = read_file(path, &n, 32u << 20);
        if (d) {
            int w = 0, h = 0;
            size_t pn = 0;
            unsigned char *png = NULL;
            const char *type = img_probe(d, n, &w, &h);
            if (!type) {
                png = wic_to_png(path, &pn, &w, &h);
                if (png) type = "pngblip";
            }
            if (type && w > 0 && h > 0) {
                long long maxw = r->W - (r->curInTbl ? r->margin + r->B * 16 : 0) - r->curIndent;
                long long gw = (long long)w * 15;
                if (wn > 0) {
                    int v = atoi(wantW);
                    int pct = memchr(wantW, '%', (size_t)wn) != NULL;
                    if (v > 0) gw = pct ? maxw * v / 100 : (long long)v * 15;
                }
                long long gh = (long long)h * gw / w;
                if (maxw > 300 && gw > maxw) { gh = gh * maxw / gw; gw = maxw; }
                bprintf(o, "{\\pict\\%s\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\n", type, w, h, (int)gw, (int)gh);
                if (png) bhex(o, png, pn); else bhex(o, d, n);
                bputs(o, "}");
                free(png);
                free(d);
                return;
            }
            free(png);
            free(d);
        }
    }
    bprintf(o, "{\\cf%d [", C_MUTED);
    if (an > 0) {
        rtf_text(o, alt, an);
    } else {
        int b = sn;
        while (b > 0 && src[b - 1] != '/' && src[b - 1] != '\\') b--;
        rtf_text(o, src + b, sn - b);
    }
    bputs(o, "]}");
}

static int try_link(R *r, const char *s, int n, int *pi, int isImg) {
    int p = *pi + (isImg ? 1 : 0);
    int close = find_bracket(s, n, p);
    if (close < 0) return 0;
    const char *txt = s + p + 1;
    int tn = close - p - 1;
    const char *url = NULL;
    int un = 0, end;

    if (!isImg && tn > 1 && txt[0] == '^') {   /* footnote reference */
        bprintf(&r->out, "{\\super\\cf%d ", C_LINK);
        rtf_text(&r->out, txt + 1, tn - 1);
        bputs(&r->out, "}");
        *pi = close + 1;
        return 1;
    }
    if (close + 1 < n && s[close + 1] == '(') {
        int q = close + 2;
        while (q < n && is_ws((unsigned char)s[q])) q++;
        if (q < n && s[q] == '<') {
            int e = q + 1;
            while (e < n && s[e] != '>' && s[e] != '\n') e++;
            if (e >= n || s[e] != '>') return 0;
            url = s + q + 1; un = e - q - 1; q = e + 1;
        } else {
            int st = q, depth = 0;
            while (q < n) {
                char ch = s[q];
                if (ch == '\\' && q + 1 < n) { q += 2; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') { if (depth == 0) break; depth--; }
                else if (is_ws((unsigned char)ch)) break;
                q++;
            }
            url = s + st; un = q - st;
        }
        while (q < n && is_ws((unsigned char)s[q])) q++;
        if (q < n && (s[q] == '"' || s[q] == '\'' || s[q] == '(')) {
            char cl = s[q] == '(' ? ')' : s[q];
            q++;
            while (q < n && s[q] != cl) q++;
            q++;
            while (q < n && is_ws((unsigned char)s[q])) q++;
        }
        if (q >= n || s[q] != ')') return 0;
        end = q + 1;
    } else {
        const char *lab = txt;
        int ln = tn;
        end = close + 1;
        if (close + 1 < n && s[close + 1] == '[') {
            int c2 = find_str(s, n, close + 2, "]");
            if (c2 < 0) return 0;
            if (c2 > close + 2) { lab = s + close + 2; ln = c2 - close - 2; }
            end = c2 + 1;
        }
        Ref *rf = find_ref(r, lab, ln);
        if (!rf) return 0;
        url = rf->url;
        un = (int)strlen(url);
    }
    if (isImg) {
        emit_image(r, url, un, txt, tn, NULL, 0);
    } else if (r->inLink) {
        inl(r, txt, tn);
    } else {
        link_open(r, url, un);
        r->inLink = 1;
        inl(r, txt, tn);
        r->inLink = 0;
        link_close(r);
    }
    *pi = end;
    return 1;
}

/* ---- inline HTML ---- */

static int get_attr(const char *a, int an, const char *name, const char **val, int *vn) {
    int k = (int)strlen(name);
    for (int i = 0; i + k < an; i++) {
        if ((i == 0 || is_ws((unsigned char)a[i - 1])) && starts_ci(a + i, an - i, name)) {
            int j = i + k;
            while (j < an && a[j] == ' ') j++;
            if (j >= an || a[j] != '=') continue;
            j++;
            while (j < an && a[j] == ' ') j++;
            if (j < an && (a[j] == '"' || a[j] == '\'')) {
                char q = a[j++];
                int st = j;
                while (j < an && a[j] != q) j++;
                *val = a + st; *vn = j - st;
            } else {
                int st = j;
                while (j < an && !is_ws((unsigned char)a[j]) && a[j] != '>' && a[j] != '/') j++;
                *val = a + st; *vn = j - st;
            }
            return 1;
        }
    }
    return 0;
}

static const char *fmt_tag(const char *name) {
    if (!strcmp(name, "b") || !strcmp(name, "strong")) return "{\\b ";
    if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite")) return "{\\i ";
    if (!strcmp(name, "s") || !strcmp(name, "del") || !strcmp(name, "strike")) return "{\\strike ";
    if (!strcmp(name, "u") || !strcmp(name, "ins")) return "{\\ul ";
    if (!strcmp(name, "sup")) return "{\\super ";
    if (!strcmp(name, "sub")) return "{\\sub ";
    if (!strcmp(name, "code") || !strcmp(name, "kbd") || !strcmp(name, "tt") || !strcmp(name, "samp")) return "code";
    return NULL;
}

static int try_html(R *r, const char *s, int n, int *pi, int *depth) {
    Buf *o = &r->out;
    int p = *pi;
    if (n - p >= 4 && !memcmp(s + p, "<!--", 4)) {
        int e = find_str(s, n, p + 4, "-->");
        *pi = e < 0 ? n : e + 3;
        return 1;
    }
    int q = p + 1;
    if (q < n && isalpha((unsigned char)s[q])) {   /* autolink <scheme:...> / <a@b> */
        int e = q;
        while (e < n && s[e] != '>' && s[e] != '<' && !is_ws((unsigned char)s[e])) e++;
        if (e < n && s[e] == '>') {
            const char *u = s + q;
            int un = e - q;
            int hasColon = memchr(u, ':', (size_t)un) != NULL;
            int hasAt = memchr(u, '@', (size_t)un) != NULL;
            if (hasColon && (starts_ci(u, un, "http") || starts_ci(u, un, "mailto:") || starts_ci(u, un, "ftp") || starts_ci(u, un, "file:"))) {
                if (r->inLink) rtf_text(o, u, un);
                else { link_open(r, u, un); rtf_text(o, u, un); link_close(r); }
                *pi = e + 1;
                return 1;
            }
            if (hasAt && !hasColon) {
                if (r->inLink) rtf_text(o, u, un);
                else {
                    Buf m = {0};
                    bputs(&m, "mailto:");
                    bput(&m, u, (size_t)un);
                    link_open(r, m.p, (int)m.n);
                    rtf_text(o, u, un);
                    link_close(r);
                    free(m.p);
                }
                *pi = e + 1;
                return 1;
            }
        }
    }
    int closing = 0;
    q = p + 1;
    if (q < n && s[q] == '/') { closing = 1; q++; }
    if (q >= n || !isalpha((unsigned char)s[q])) return 0;
    char name[16];
    int nl = 0;
    while (q < n && (isalnum((unsigned char)s[q]) || s[q] == '-')) {
        if (nl < 15) name[nl++] = (char)tolower((unsigned char)s[q]);
        q++;
    }
    name[nl] = 0;
    if (q < n && !(is_ws((unsigned char)s[q]) || s[q] == '/' || s[q] == '>')) return 0;
    int as = q;
    while (q < n && s[q] != '>') {
        if (s[q] == '"' || s[q] == '\'') {
            char qc = s[q++];
            while (q < n && s[q] != qc) q++;
        }
        q++;
    }
    if (q >= n) return 0;
    const char *attrs = s + as;
    int an = q - as;
    int tagEnd = q + 1;
    const char *ft = fmt_tag(name);

    if (!closing) {
        if (!strcmp(name, "br") || !strcmp(name, "hr")) {
            bputs(o, "\\line ");
        } else if (!strcmp(name, "img")) {
            const char *src = NULL, *alt = NULL, *wd = NULL;
            int sn = 0, altn = 0, wdn = 0;
            if (get_attr(attrs, an, "src", &src, &sn)) {
                get_attr(attrs, an, "alt", &alt, &altn);
                get_attr(attrs, an, "width", &wd, &wdn);
                char wbuf[16] = "";
                if (wdn > 0 && wdn < 15) { memcpy(wbuf, wd, (size_t)wdn); wbuf[wdn] = 0; }
                emit_image(r, src, sn, alt, altn, wbuf, wdn > 0 && wdn < 15 ? wdn : 0);
            }
        } else if (!strcmp(name, "a")) {
            int ce = find_str_ci(s, n, tagEnd, "</a>");
            const char *href = NULL;
            int hn = 0;
            if (ce >= 0) {
                if (get_attr(attrs, an, "href", &href, &hn) && hn > 0 && !r->inLink) {
                    link_open(r, href, hn);
                    r->inLink = 1;
                    inl(r, s + tagEnd, ce - tagEnd);
                    r->inLink = 0;
                    link_close(r);
                } else {
                    inl(r, s + tagEnd, ce - tagEnd);
                }
                *pi = ce + 4;
                return 1;
            }
        } else if (ft) {
            if (!strcmp(ft, "code")) bprintf(o, "{\\f1\\fs%d\\highlight%d ", r->B - 2, C_INLINEBG);
            else bputs(o, ft);
            (*depth)++;
        }
    } else if (ft && *depth > 0) {
        bputc(o, '}');
        (*depth)--;
    }
    *pi = tagEnd;
    return 1;
}

/* ---- entities ---- */

static const struct { const char *name; unsigned cp; } kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}, {"middot", 0xB7},
    {"deg", 0xB0}, {"times", 0xD7}, {"divide", 0xF7}, {"larr", 0x2190}, {"rarr", 0x2192},
    {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"check", 0x2713}, {"euro", 0x20AC},
    {"pound", 0xA3}, {"yen", 0xA5}, {"sect", 0xA7}, {"para", 0xB6}, {"plusmn", 0xB1},
    {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"shy", 0xAD}, {"zwj", 0x200D}, {"zwnj", 0x200C},
};

static int try_entity(R *r, const char *s, int n, int *pi) {
    int p = *pi + 1, e = p;
    while (e < n && e - p < 12 && (isalnum((unsigned char)s[e]) || s[e] == '#')) e++;
    if (e >= n || s[e] != ';' || e == p) return 0;
    unsigned cp = 0;
    if (s[p] == '#') {
        if (p + 1 < e && (s[p + 1] == 'x' || s[p + 1] == 'X')) cp = (unsigned)strtoul(s + p + 2, NULL, 16);
        else cp = (unsigned)strtoul(s + p + 1, NULL, 10);
    } else {
        for (size_t k = 0; k < ARRAYSIZE(kEntities); k++) {
            int kl = (int)strlen(kEntities[k].name);
            if (kl == e - p && !memcmp(kEntities[k].name, s + p, (size_t)kl)) { cp = kEntities[k].cp; break; }
        }
    }
    if (!cp || cp > 0x10FFFF) return 0;
    rtf_cp(&r->out, cp);
    *pi = e + 1;
    return 1;
}

/* ---- inline renderer ---- */

static void code_span(R *r, const char *s, int n) {
    if (n >= 2 && s[0] == ' ' && s[n - 1] == ' ') { s++; n -= 2; }
    bprintf(&r->out, "{\\f1\\fs%d\\highlight%d ", r->B - 2, C_INLINEBG);
    rtf_text(&r->out, s, n);
    bputc(&r->out, '}');
}

/* GFM "extended autolink": http://..., https://..., www.... Returns its length or 0. */
static int bare_url_len(const char *s, int n, int i) {
    if (i > 0 && (is_word((unsigned char)s[i - 1]) || s[i - 1] == '/' || s[i - 1] == '@')) return 0;
    int k;
    if (starts_ci(s + i, n - i, "https://")) k = 8;
    else if (starts_ci(s + i, n - i, "http://")) k = 7;
    else if (starts_ci(s + i, n - i, "www.")) k = 4;
    else return 0;
    int e = i + k;
    while (e < n && !is_ws((unsigned char)s[e]) && s[e] != '<' && s[e] != '`') e++;
    /* trailing punctuation is not part of the URL; keep balanced parentheses */
    for (;;) {
        char t = s[e - 1];
        if (strchr("?!.,:*_~'\";", t)) { e--; continue; }
        if (t == ')') {
            int open = 0, close = 0;
            for (int q = i; q < e; q++) { if (s[q] == '(') open++; else if (s[q] == ')') close++; }
            if (close > open) { e--; continue; }
        }
        break;
    }
    return e - i > k ? e - i : 0;
}

static void inl(R *r, const char *s, int n) {
    Buf *o = &r->out;
    int i = 0, htmlDepth = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if ((c == 'h' || c == 'H' || c == 'w' || c == 'W') && !r->inLink) {
            int ul = bare_url_len(s, n, i);
            if (ul) {
                if (c == 'w' || c == 'W') {
                    Buf u = {0};
                    bputs(&u, "http://");
                    bput(&u, s + i, (size_t)ul);
                    link_open(r, u.p, (int)u.n);
                    free(u.p);
                } else {
                    link_open(r, s + i, ul);
                }
                rtf_text(o, s + i, ul);
                link_close(r);
                i += ul;
                continue;
            }
        }
        if (c == '\\' && i + 1 < n) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d == '\n') { bputs(o, "\\line "); i += 2; continue; }
            if (is_punct(d)) { rtf_cp(o, d); i += 2; continue; }
        }
        if (c == '\n') {
            if (i >= 2 && s[i - 1] == ' ' && s[i - 2] == ' ') bputs(o, "\\line ");
            else bputc(o, ' ');
            i++;
            while (i < n && s[i] == ' ') i++;
            continue;
        }
        if (c == '`') {
            int m = run_of(s, n, i, '`');
            int e = find_bt(s, n, i + m, m);
            if (e >= 0) { code_span(r, s + i + m, e - i - m); i = e + m; continue; }
            rtf_text(o, s + i, m);
            i += m;
            continue;
        }
        if (c == '*' || c == '_') {
            int m = run_of(s, n, i, (char)c);
            if (m <= 3 && i + m < n && !is_ws((unsigned char)s[i + m]) &&
                !(c == '_' && i > 0 && is_word((unsigned char)s[i - 1]))) {
                int e = find_closer(s, n, i + m, (char)c, m);
                if (e >= 0) {
                    bputs(o, m == 1 ? "{\\i " : m == 2 ? "{\\b " : "{\\b\\i ");
                    inl(r, s + i + m, e - i - m);
                    bputc(o, '}');
                    i = e + m;
                    continue;
                }
            }
            rtf_text(o, s + i, m);
            i += m;
            continue;
        }
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            int e = find_str(s, n, i + 2, "~~");
            if (e > i + 2) {
                bputs(o, "{\\strike ");
                inl(r, s + i + 2, e - i - 2);
                bputc(o, '}');
                i = e + 2;
                continue;
            }
        }
        if (c == '!' && i + 1 < n && s[i + 1] == '[' && try_link(r, s, n, &i, 1)) continue;
        if (c == '[' && try_link(r, s, n, &i, 0)) continue;
        if (c == '<' && try_html(r, s, n, &i, &htmlDepth)) continue;
        if (c == '&' && try_entity(r, s, n, &i)) continue;
        int j = i + 1;
        while (j < n && !strchr("\\\n`*_~![<&hHwW", s[j])) j++;
        rtf_text(o, s + i, j - i);
        i = j;
    }
    while (htmlDepth-- > 0) bputc(o, '}');
}

/* ---- syntax highlighting (lightweight, keyword/string/comment/number) ---- */

static const char *kKwC =
    " if else for while do switch case break continue return goto default function func fn def class struct"
    " enum union interface impl trait import export from package module use mod namespace using new delete"
    " try catch finally throw throws var let const static public private protected internal extern typedef"
    " sizeof typeof instanceof void int char float double bool boolean string long short unsigned signed"
    " auto byte uint i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 usize isize true false null nil this self super"
    " async await yield lambda match where pub mut ref out in is as virtual override abstract final sealed"
    " readonly volatile inline template typename constexpr nullptr go defer chan select map range type"
    " val object companion when override open data suspend implements extends native synchronized ";
static const char *kKwHash =
    " if elif else fi then for while do done case esac in function return break continue def class import"
    " from as with try except finally raise pass lambda yield global nonlocal assert del not and or is None"
    " True False self async await print echo local export unset readonly shift exit source alias true false"
    " null nil end begin module require include puts unless until elsif param foreach switch ";
static const char *kKwSql =
    " select from where insert into values update set delete create table view index drop alter add column"
    " primary key foreign references join inner left right outer full on as and or not null is in exists"
    " between like order by group having limit offset union all distinct case when then else end begin"
    " commit rollback transaction declare int integer varchar text char date datetime timestamp boolean"
    " default unique check constraint with returning local function return if then elseif for do while"
    " repeat until nil true false ";

static int lang_class(const char *info, int n) {
    char w[24];
    int k = 0;
    while (k < n && k < 23 && !is_ws((unsigned char)info[k]) && info[k] != '{' && info[k] != ',') {
        w[k] = (char)tolower((unsigned char)info[k]);
        k++;
    }
    w[k] = 0;
    if (!k) return 0;
    static const char *cl[] = { "c", "h", "cpp", "c++", "cc", "cxx", "hpp", "cs", "csharp", "c#", "java", "js",
        "javascript", "jsx", "ts", "typescript", "tsx", "go", "golang", "rust", "rs", "swift", "kotlin", "kt",
        "php", "json", "jsonc", "json5", "css", "scss", "less", "scala", "dart", "groovy", "gradle", "objc",
        "objective-c", "zig", "glsl", "hlsl", "proto", "protobuf", "solidity", "sol", "vue", "svelte", NULL };
    static const char *hl[] = { "py", "python", "python3", "sh", "bash", "shell", "zsh", "fish", "console",
        "ps1", "powershell", "pwsh", "yaml", "yml", "toml", "rb", "ruby", "perl", "pl", "r", "dockerfile",
        "docker", "makefile", "make", "cmake", "conf", "nginx", "elixir", "ex", "exs", "nim", "tcl", "awk",
        "ini", "properties", "env", "gitignore", "julia", "jl", "coffee", NULL };
    static const char *sq[] = { "sql", "mysql", "postgres", "postgresql", "psql", "sqlite", "plsql", "tsql",
        "lua", "haskell", "hs", "elm", "ada", "vhdl", NULL };
    for (int i = 0; cl[i]; i++) if (!strcmp(w, cl[i])) return 1;
    for (int i = 0; hl[i]; i++) if (!strcmp(w, hl[i])) return 2;
    for (int i = 0; sq[i]; i++) if (!strcmp(w, sq[i])) return 3;
    return 0;
}

static int is_kw(const char *list, const char *s, int n, int ci) {
    char w[40];
    if (n > 36) return 0;
    w[0] = ' ';
    for (int i = 0; i < n; i++) w[i + 1] = ci ? (char)tolower((unsigned char)s[i]) : s[i];
    w[n + 1] = ' ';
    w[n + 2] = 0;
    return strstr(list, w) != NULL;
}

static void code_colored(R *r, int color, const char *s, int n) {
    bprintf(&r->out, "{\\cf%d ", color);
    rtf_text_ex(&r->out, s, n, 1);
    bputc(&r->out, '}');
}

static void code_hl(R *r, const char *s, int n, int cls) {
    Buf *o = &r->out;
    const char *kw = cls == 1 ? kKwC : cls == 2 ? kKwHash : kKwSql;
    int i = 0;
    while (i < n) {
        char c = s[i];
        char d = i + 1 < n ? s[i + 1] : 0;
        int prevWord = i > 0 && (is_word((unsigned char)s[i - 1]) || s[i - 1] == '$');
        /* comments */
        if ((cls == 1 && c == '/' && d == '/') || (cls == 2 && c == '#' && (i == 0 || is_ws((unsigned char)s[i - 1]))) ||
            (cls == 3 && c == '-' && d == '-')) {
            int e = i;
            while (e < n && s[e] != '\n') e++;
            code_colored(r, C_COM, s + i, e - i);
            i = e;
            continue;
        }
        if (cls == 1 && c == '/' && d == '*') {
            int e = find_str(s, n, i + 2, "*/");
            e = e < 0 ? n : e + 2;
            code_colored(r, C_COM, s + i, e - i);
            i = e;
            continue;
        }
        /* strings */
        if (c == '"' || c == '\'' || (c == '`' && cls == 1)) {
            if (c == '\'' && prevWord) { rtf_text_ex(o, s + i, 1, 1); i++; continue; }
            int e = i + 1;
            while (e < n && s[e] != c && s[e] != '\n') {
                if (s[e] == '\\' && e + 1 < n) e++;
                e++;
            }
            if (e < n && s[e] == c) e++;
            code_colored(r, C_STR, s + i, e - i);
            i = e;
            continue;
        }
        /* numbers */
        if (isdigit((unsigned char)c) && !prevWord) {
            int e = i + 1;
            while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '.' || s[e] == '_')) e++;
            code_colored(r, C_NUM, s + i, e - i);
            i = e;
            continue;
        }
        /* identifiers */
        if ((isalpha((unsigned char)c) || c == '_') && !prevWord) {
            int e = i + 1;
            while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
            if (is_kw(kw, s + i, e - i, cls == 3)) code_colored(r, C_KW, s + i, e - i);
            else rtf_text_ex(o, s + i, e - i, 1);
            i = e;
            continue;
        }
        int e = i + 1;
        while (e < n && ((unsigned char)s[e] & 0xC0) == 0x80) e++;
        rtf_text_ex(o, s + i, e - i, 1);
        i = e;
    }
}

/* ---- block-level output ---- */

/* Vertical space between blocks is emitted as separate empty paragraphs of exact height
 * (not \sb/\sa): RichEdit paints character highlights (inline code) over a line's
 * space-before/after, which made code spans look like tall boxes. Spacing collapses like
 * CSS margins: the gap is the larger of the previous block's "after" and the next "before". */
static void spacer_line(R *r, int intbl, int tw, const char *end) {
    bprintf(&r->out, "\\pard%s\\sb0\\sa0\\sl-%d\\plain\\fs2%s\n", intbl ? "\\intbl" : "", tw, end);
}

static void close_para(R *r) {
    if (r->open) { bputs(&r->out, "\\par\n"); r->open = 0; }
}

static void flush_space(R *r, Ctx *c, int before) {
    int tw = r->pend > before ? r->pend : before;
    r->pend = 0;
    if (tw >= 20) spacer_line(r, c->intbl, tw, "\\par");
}

/* space requested after the current block */
static void set_last_sa(R *r, int sa) { if (sa > r->pend) r->pend = sa; }

static void para_begin(R *r, Ctx *c, int sb, int sa, int align) {
    Buf *o = &r->out;
    close_para(r);
    flush_space(r, c, sb);
    r->pend = sa;
    bputs(o, "\\pard");
    if (c->intbl) bputs(o, "\\intbl");
    bprintf(o, "\\li%d\\ri%d\\sb0\\sa0\\sl276\\slmult1", c->indent, c->intbl ? 0 : r->margin);
    if (align == 1) bputs(o, "\\qc");
    else if (align == 2) bputs(o, "\\qr");
    if (r->mkOn) bprintf(o, "\\fi-%d\\tx%d", r->mkHang, c->indent);
    bprintf(o, "\\plain\\f0\\fs%d\\cf%d ", r->B, (c->quote && !c->alert) ? C_MUTED : C_TEXT);
    if (r->mkOn) {
        if (r->mkCp) rtf_cp(o, r->mkCp);
        else bputs(o, r->mkTxt);
        bputs(o, "\\tab ");
        r->mkOn = 0;
    }
    r->open = 1;
    r->nblocks++;
    r->curIndent = c->indent;
    r->curInTbl = c->intbl;
}

static int para_sa(R *r, Ctx *c) { return c->tight ? r->B * 3 : r->B * 10; }

static void flush_marker(R *r, Ctx *c) {
    if (r->mkOn) para_begin(r, c, 0, r->B * 2, 0);
}

/* Request vertical space after a block. */
static void spacer(R *r, Ctx *c, int tw) {
    (void)c;
    close_para(r);
    set_last_sa(r, tw);
}

/* Start of a table-based block: close the paragraph and emit pending space. */
static void block_start(R *r, Ctx *c) {
    close_para(r);
    flush_space(r, c, 0);
}

static void cell_borders(Buf *o, int top, int left, int bottom, int right, int tw) {
    bprintf(o, "\\clbrdrt\\brdrs\\brdrw%d\\brdrcf%d\\clbrdrl\\brdrs\\brdrw%d\\brdrcf%d"
               "\\clbrdrb\\brdrs\\brdrw%d\\brdrcf%d\\clbrdrr\\brdrs\\brdrw%d\\brdrcf%d",
            top == C_BG ? 10 : tw, top, left == C_BG ? 10 : tw, left,
            bottom == C_BG ? 10 : tw, bottom, right == C_BG ? 10 : tw, right);
}

static void rule_row(R *r, Ctx *c, int thick) {
    Buf *o = &r->out;
    block_start(r, c);
    bprintf(o, "\\pard\\plain\\trowd\\trgaph0\\trleft%d", c->indent);
    cell_borders(o, C_BORDER, C_BG, C_BG, C_BG, thick);
    bprintf(o, "\\cellx%d\\pard\\intbl\\sb0\\sa0\\sl-20\\plain\\fs2\\cell\\row\n", r->W);
}

static void hr_block(R *r, Ctx *c) {
    flush_marker(r, c);
    if (c->intbl) {
        para_begin(r, c, 0, para_sa(r, c), 0);
        bprintf(&r->out, "{\\cf%d ", C_BORDER);
        for (int i = 0; i < 24; i++) rtf_cp(&r->out, 0x2500);
        bputc(&r->out, '}');
        return;
    }
    close_para(r);
    spacer(r, c, r->B * 4);
    rule_row(r, c, 45);
    spacer(r, c, r->B * 10);
}

static void add_heading(R *r, const char *s, int n) {
    Buf p = {0};
    md_plain(s, n, &p);
    WCHAR *text = utf8_to_wide(p.p ? p.p : "", (int)p.n);
    free(p.p);
    size_t tl = wcslen(text);
    WCHAR *slug = (WCHAR *)malloc((tl + 16) * sizeof(WCHAR));
    size_t k = 0;
    for (size_t i = 0; i < tl; i++) {
        WCHAR ch = text[i];
        if (IsCharAlphaNumericW(ch) || ch == '-' || ch == '_') slug[k++] = ch;
        else if (ch == ' ') slug[k++] = '-';
    }
    slug[k] = 0;
    CharLowerBuffW(slug, (DWORD)k);
    int dup = 0;
    for (int i = 0; i < r->nheads; i++) {
        const WCHAR *h = r->heads[i].slug;
        size_t hl = wcslen(h);
        if (!wcsncmp(h, slug, k) && (hl == k || (h[k] == '-' && iswdigit(h[k + 1])))) dup++;
    }
    if (dup) swprintf(slug + k, 16, L"-%d", dup);
    if (r->nheads == r->capheads) {
        r->capheads = r->capheads ? r->capheads * 2 : 32;
        r->heads = (Head *)realloc(r->heads, sizeof(Head) * (size_t)r->capheads);
    }
    r->heads[r->nheads].slug = slug;
    r->heads[r->nheads].text = text;
    r->nheads++;
}

static void heading(R *r, Ctx *c, int lvl, const char *s, int n, int align) {
    static const int mul[7] = { 0, 200, 150, 125, 100, 88, 85 };
    int fs = r->B * mul[lvl] / 100;
    int sb = r->nblocks == 0 ? 0 : r->B * 11;
    int sa = lvl <= 2 && !c->intbl ? r->B * 3 : r->B * 7;
    add_heading(r, s, n);
    para_begin(r, c, sb, sa, align);
    bprintf(&r->out, "\\b\\fs%d%s ", fs, lvl == 6 ? "\\cf2" : "");
    inl(r, s, n);
    if (lvl <= 2 && !c->intbl) {
        rule_row(r, c, 15);
        spacer(r, c, r->B * 7);
    }
}

static void code_block(R *r, Ctx *c, Line *L, int n, const char *info, int in) {
    Buf *o = &r->out;
    int cls = lang_class(info, in);
    Buf code = {0};
    for (int i = 0; i < n; i++) {
        if (i) bputc(&code, '\n');
        bput(&code, L[i].s, (size_t)L[i].n);
    }
    flush_marker(r, c);
    if (c->intbl) {
        para_begin(r, c, 0, para_sa(r, c), 0);
        bprintf(o, "{\\f1\\fs%d\\highlight%d ", r->B - 2, C_CODEBG);
        if (cls) code_hl(r, code.p ? code.p : "", (int)code.n, cls);
        else rtf_text_ex(o, code.p ? code.p : "", (int)code.n, 1);
        bputc(o, '}');
        free(code.p);
        return;
    }
    block_start(r, c);
    bprintf(o, "\\pard\\plain\\trowd\\trgaph%d\\trleft%d", r->B * 8, c->indent);
    cell_borders(o, C_CODEBG, C_CODEBG, C_CODEBG, C_CODEBG, 10);
    bprintf(o, "\\clcbpat%d\\cellx%d", C_CODEBG, r->W);
    bprintf(o, "\\pard\\intbl\\sb%d\\sa%d\\sl240\\slmult1\\plain\\f1\\fs%d\\cf%d ",
            r->B * 7, r->B * 7, r->B - 2, C_TEXT);
    if (cls) code_hl(r, code.p ? code.p : "", (int)code.n, cls);
    else rtf_text_ex(o, code.p ? code.p : "", (int)code.n, 1);
    bputs(o, "\\cell\\row\n");
    spacer(r, c, r->B * 10);
    r->nblocks++;
    free(code.p);
}

/* ---- block detection helpers ---- */

static int ln_blank(const Line *l) {
    for (int i = 0; i < l->n; i++)
        if (l->s[i] != ' ') return 0;
    return 1;
}
static int ln_indent(const Line *l) {
    int i = 0;
    while (i < l->n && l->s[i] == ' ') i++;
    return i;
}
static int atx_level(const char *t, int n) {
    int k = 0;
    while (k < n && t[k] == '#') k++;
    return (k >= 1 && k <= 6 && (k == n || t[k] == ' ')) ? k : 0;
}
static int is_hr(const char *t, int n) {
    if (!n || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (t[i] == t[0]) cnt++;
        else if (t[i] != ' ') return 0;
    }
    return cnt >= 3;
}
static int fence_len(const char *t, int n, char *fc) {
    if (n < 3 || (t[0] != '`' && t[0] != '~')) return 0;
    int k = run_of(t, n, 0, t[0]);
    if (k < 3) return 0;
    if (t[0] == '`' && memchr(t + k, '`', (size_t)(n - k))) return 0;
    *fc = t[0];
    return k;
}
static int list_marker(const char *t, int n, int *ordered, int *num, char *mch, int *mlen) {
    if (n >= 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && (n == 1 || t[1] == ' ')) {
        *ordered = 0; *num = 0; *mch = t[0]; *mlen = 1;
        return 1;
    }
    int k = 0, v = 0;
    while (k < n && k < 9 && isdigit((unsigned char)t[k])) { v = v * 10 + (t[k] - '0'); k++; }
    if (k > 0 && k < n && (t[k] == '.' || t[k] == ')') && (k + 1 == n || t[k + 1] == ' ')) {
        *ordered = 1; *num = v; *mch = t[k]; *mlen = k + 1;
        return 1;
    }
    return 0;
}
static int setext_level(const char *t, int n) {
    if (!n || (t[0] != '=' && t[0] != '-')) return 0;
    int k = run_of(t, n, 0, t[0]);
    for (int i = k; i < n; i++) if (t[i] != ' ') return 0;
    return t[0] == '=' ? 1 : 2;
}

#define MAXCOLS 64

static int table_delim(const Line *l, int *cols, char *al) {
    const char *s = l->s;
    int n = l->n, i = 0, nc = 0, sawPipe = 0;
    while (i < n && s[i] == ' ') i++;
    if (i >= 4 || i >= n) return 0;
    if (s[i] == '|') { sawPipe = 1; i++; }
    while (i < n) {
        while (i < n && s[i] == ' ') i++;
        if (i >= n) break;
        int left = 0, right = 0, dashes = 0;
        if (s[i] == ':') { left = 1; i++; }
        while (i < n && s[i] == '-') { dashes++; i++; }
        if (i < n && s[i] == ':') { right = 1; i++; }
        while (i < n && s[i] == ' ') i++;
        if (!dashes) return 0;
        if (al && nc < MAXCOLS) al[nc] = (left && right) ? 'c' : right ? 'r' : 'l';
        nc++;
        if (i < n) {
            if (s[i] != '|') return 0;
            sawPipe = 1;
            i++;
        }
    }
    if (!nc || (!sawPipe && nc < 2)) return 0;
    if (cols) *cols = nc > MAXCOLS ? MAXCOLS : nc;
    return 1;
}

static const char *kHtmlBlockTags[] = {
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "div", "dl", "dd", "dt",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
    "hr", "html", "li", "main", "nav", "ol", "p", "picture", "pre", "section", "summary", "table", "tbody",
    "td", "th", "thead", "tr", "ul", "video", NULL
};
static int html_block_start(const char *t, int n) {
    if (n < 2 || t[0] != '<') return 0;
    int i = 1;
    if (t[i] == '/') i++;
    char name[16];
    int k = 0;
    while (i < n && k < 15 && isalnum((unsigned char)t[i])) name[k++] = (char)tolower((unsigned char)t[i++]);
    name[k] = 0;
    if (!k || (i < n && !(t[i] == ' ' || t[i] == '>' || t[i] == '/'))) return 0;
    for (int j = 0; kHtmlBlockTags[j]; j++)
        if (!strcmp(name, kHtmlBlockTags[j])) return 1;
    return 0;
}

static int interrupts(Line *L, int i, int n) {
    Line *l = &L[i];
    if (l->skip) return 1;
    int ind = ln_indent(l);
    if (ind >= 4) return 0;
    const char *t = l->s + ind;
    int tn = l->n - ind;
    char fc;
    if (!tn) return 0;
    if (atx_level(t, tn) || is_hr(t, tn) || fence_len(t, tn, &fc) || t[0] == '>') return 1;
    int o, num, ml;
    char mc;
    if (list_marker(t, tn, &o, &num, &mc, &ml) && ml < tn && (!o || num == 1)) return 1;
    if (tn >= 4 && !memcmp(t, "<!--", 4)) return 1;
    if (i + 1 < n && memchr(t, '|', (size_t)tn) && table_delim(&L[i + 1], NULL, NULL)) return 1;
    if (html_block_start(t, tn)) return 1;
    return 0;
}

/* ---- block renderers ---- */

typedef struct { Line *ln; int n, cap; int task; int hadBlank; } Item;

static void push_line(Line **arr, int *n, int *cap, const char *s, int len) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (Line *)realloc(*arr, sizeof(Line) * (size_t)*cap);
    }
    (*arr)[*n].s = s;
    (*arr)[*n].n = len < 0 ? 0 : len;
    (*arr)[*n].skip = 0;
    (*n)++;
}

static int list_block(R *r, Line *L, int n, int i, Ctx *c) {
    int ordered, num, mlen;
    char mch;
    Line *l0 = &L[i];
    int ind0 = ln_indent(l0);
    list_marker(l0->s + ind0, l0->n - ind0, &ordered, &num, &mch, &mlen);
    int start = num;
    Item *items = NULL;
    int ni = 0, capi = 0, loose = 0, prevTrailing = 0;

    flush_marker(r, c);
    while (i < n) {
        Line *l = &L[i];
        if (l->skip) break;
        int ind = ln_indent(l);
        if (ind >= 4) break;
        const char *t = l->s + ind;
        int tn = l->n - ind, o2, n2, ml2;
        char mc2;
        if (is_hr(t, tn) || !list_marker(t, tn, &o2, &n2, &mc2, &ml2) || o2 != ordered || mc2 != mch) break;
        if (ni > 0 && prevTrailing) loose = 1;
        int sp = 0;
        while (ml2 + sp < tn && t[ml2 + sp] == ' ') sp++;
        int restBlank = ml2 + sp >= tn;
        if (sp > 4 || restBlank) sp = 1;
        int co = ind + ml2 + sp;
        if (ni == capi) {
            capi = capi ? capi * 2 : 8;
            items = (Item *)realloc(items, sizeof(Item) * (size_t)capi);
        }
        Item *it = &items[ni++];
        memset(it, 0, sizeof *it);
        const char *fs = restBlank ? l->s + l->n : l->s + co;
        int fn = restBlank ? 0 : l->n - co;
        if (fn >= 3 && fs[0] == '[' && (fs[1] == ' ' || fs[1] == 'x' || fs[1] == 'X') && fs[2] == ']' && (fn == 3 || fs[3] == ' ')) {
            it->task = fs[1] == ' ' ? 1 : 2;
            int k = fn >= 4 ? 4 : 3;
            fs += k;
            fn -= k;
        }
        push_line(&it->ln, &it->n, &it->cap, fs, fn);
        int j = i + 1, sawBlank = 0;
        while (j < n) {
            Line *m = &L[j];
            if (m->skip) break;
            if (ln_blank(m)) { push_line(&it->ln, &it->n, &it->cap, m->s, 0); sawBlank = 1; j++; continue; }
            int mi = ln_indent(m);
            if (mi >= co) {
                if (sawBlank) it->hadBlank = 1;
                push_line(&it->ln, &it->n, &it->cap, m->s + co, m->n - co);
                sawBlank = 0;
                j++;
                continue;
            }
            int oo, nn, mm;
            char cc;
            if (!sawBlank && !interrupts(L, j, n) && !list_marker(m->s + mi, m->n - mi, &oo, &nn, &cc, &mm) &&
                it->n > 0 && it->ln[it->n - 1].n > 0) {
                push_line(&it->ln, &it->n, &it->cap, m->s + mi, m->n - mi);
                j++;
                continue;
            }
            break;
        }
        int trailing = 0;
        while (it->n > 0 && ln_blank(&it->ln[it->n - 1])) { it->n--; trailing = 1; }
        if (it->hadBlank) {
            /* a blank line only counts if it separates two blocks directly inside the item */
            loose = 1;
        }
        prevTrailing = trailing;
        i = j;
    }

    for (int k = 0; k < ni; k++) {
        Ctx k2 = *c;
        int li = ordered ? r->B * 20 : r->B * 18;
        k2.indent = c->indent + li;
        k2.tight = !loose;
        r->mkHang = ordered ? r->B * 16 : r->B * 11;
        if (items[k].task) { r->mkCp = items[k].task == 2 ? 0x2611 : 0x2610; }
        else if (ordered) { snprintf(r->mkTxt, sizeof r->mkTxt, "%d%c", start + k, mch); r->mkCp = 0; }
        else r->mkCp = r->depth == 0 ? 0x2022 : r->depth == 1 ? 0x25E6 : 0x25AA;
        r->mkOn = 1;
        r->depth++;
        blocks(r, items[k].ln, items[k].n, k2);
        r->depth--;
        if (r->mkOn) para_begin(r, &k2, 0, para_sa(r, &k2), 0);
        free(items[k].ln);
    }
    free(items);
    set_last_sa(r, para_sa(r, c));
    return i;
}

static void quote_block(R *r, Ctx *c, Line *L, int n) {
    Buf *o = &r->out;
    int alert = 0, skipFirst = 0;
    static const char *alerts[] = { "[!NOTE]", "[!TIP]", "[!IMPORTANT]", "[!WARNING]", "[!CAUTION]" };
    static const char *titles[] = { "Note", "Tip", "Important", "Warning", "Caution" };
    static const unsigned icons[] = { 0x24D8, 0x2600, 0x2757, 0x26A0, 0x26D4 };
    if (n > 0) {
        Line *f = &L[0];
        int ind = ln_indent(f);
        int len = f->n - ind;
        while (len > 0 && f->s[ind + len - 1] == ' ') len--;
        for (int a = 0; a < 5; a++)
            if (len == (int)strlen(alerts[a]) && starts_ci(f->s + ind, len, alerts[a])) { alert = a + 1; skipFirst = 1; }
    }
    flush_marker(r, c);
    if (c->intbl) {   /* nested quote: indent inside the existing cell */
        Ctx k = *c;
        k.indent += r->B * 16;
        k.quote = 1;
        blocks(r, L + skipFirst, n - skipFirst, k);
        return;
    }
    int bar = alert ? C_NOTE + alert - 1 : C_QUOTE;
    block_start(r, c);
    bprintf(o, "\\pard\\plain\\trowd\\trgaph%d\\trleft%d", r->B * 10, c->indent);
    cell_borders(o, C_BG, bar, C_BG, C_BG, 60);
    bprintf(o, "\\cellx%d\n", r->W);
    Ctx k = *c;
    k.intbl = 1;
    k.indent = 0;
    k.quote = 1;
    k.tight = 0;
    k.alert = alert;
    r->pend = r->B * 5;   /* top padding inside the cell */
    if (alert) {
        para_begin(r, &k, 0, r->B * 4, 0);
        bprintf(o, "{\\b\\cf%d ", bar);
        rtf_cp(o, icons[alert - 1]);
        bprintf(o, "  %s}", titles[alert - 1]);
    }
    blocks(r, L + skipFirst, n - skipFirst, k);
    close_para(r);
    spacer_line(r, 1, r->B * 5, "\\cell\\row");   /* bottom padding doubles as the cell's last paragraph */
    r->pend = 0;
    spacer(r, c, r->B * 10);
}

typedef struct { const char *s; int n; } Cell;

static int split_row(const Line *l, Cell *cells, int max) {
    const char *s = l->s;
    int n = l->n;
    while (n > 0 && s[n - 1] == ' ') n--;
    int i = 0;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '|') i++;
    if (n > i && s[n - 1] == '|' && !(n >= 2 && s[n - 2] == '\\')) n--;
    int cnt = 0, st = i;
    for (; i <= n; i++) {
        if (i == n || (s[i] == '|' && !(i > 0 && s[i - 1] == '\\'))) {
            int a = st, b = i;
            while (a < b && s[a] == ' ') a++;
            while (b > a && s[b - 1] == ' ') b--;
            if (cnt < max) { cells[cnt].s = s + a; cells[cnt].n = b - a; cnt++; }
            st = i + 1;
        }
    }
    return cnt;
}

static void cell_inline(R *r, const char *s, int n) {
    Buf tmp = {0};
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '|') continue;
        bputc(&tmp, s[i]);
    }
    inl(r, tmp.p ? tmp.p : "", (int)tmp.n);
    free(tmp.p);
}

static int table_block(R *r, Line *L, int n, int i, Ctx *c) {
    Buf *o = &r->out;
    int ncols = 0;
    char al[MAXCOLS];
    table_delim(&L[i + 1], &ncols, al);
    int first = i, j = i + 2;
    while (j < n && !L[j].skip && !ln_blank(&L[j]) && memchr(L[j].s, '|', (size_t)L[j].n) && !interrupts(L, j, n)) j++;
    int nrows = j - first - 1;   /* header + body rows */
    Cell *cells = (Cell *)calloc((size_t)nrows * ncols, sizeof(Cell));
    for (int rI = 0; rI < nrows; rI++) {
        Line *ln = &L[rI == 0 ? first : first + 1 + rI];
        split_row(ln, cells + (size_t)rI * ncols, ncols);
    }
    flush_marker(r, c);
    if (c->intbl) {   /* no nested tables: flatten */
        for (int rI = 0; rI < nrows; rI++) {
            para_begin(r, c, 0, r->B * 3, 0);
            if (rI == 0) bputs(o, "\\b ");
            for (int k = 0; k < ncols; k++) {
                Cell *ce = &cells[(size_t)rI * ncols + k];
                if (k) bputs(o, "  |  ");
                cell_inline(r, ce->s, ce->n);
            }
        }
        free(cells);
        return j;
    }
    /* column widths from content */
    int avail = r->W - c->indent;
    int gap = r->B * 6;
    int charTw = r->B * 11 / 2;
    int nat[MAXCOLS], w[MAXCOLS];
    long long sum = 0;
    for (int k = 0; k < ncols; k++) {
        int mx = 3;
        for (int rI = 0; rI < nrows; rI++) {
            Cell *ce = &cells[(size_t)rI * ncols + k];
            int pl = plain_len(ce->s, ce->n) + (rI == 0 ? 1 : 0);
            if (pl > mx) mx = pl;
        }
        nat[k] = mx * charTw + gap * 2 + 60;
        sum += nat[k];
    }
    if (sum <= avail) {
        for (int k = 0; k < ncols; k++) w[k] = nat[k];
    } else {
        int minw = charTw * 4 + gap * 2;
        long long fixed = 0, flex = 0;
        for (int k = 0; k < ncols; k++) {
            if (nat[k] * (long long)avail / sum < minw) fixed += minw;
            else flex += nat[k];
        }
        long long rest = avail - fixed;
        if (rest < 0) rest = 0;
        for (int k = 0; k < ncols; k++) {
            if (nat[k] * (long long)avail / sum < minw) w[k] = minw;
            else w[k] = flex ? (int)(nat[k] * rest / flex) : minw;
        }
    }
    block_start(r, c);
    for (int rI = 0; rI < nrows; rI++) {
        bprintf(o, "\\pard\\plain\\trowd\\trgaph%d\\trleft%d", gap, c->indent);
        int x = c->indent;
        for (int k = 0; k < ncols; k++) {
            cell_borders(o, C_BORDER, C_BORDER, C_BORDER, C_BORDER, 10);
            if (!r->dark && (rI == 0 || rI % 2 == 0)) bprintf(o, "\\clcbpat%d", C_STRIPE);
            x += w[k];
            bprintf(o, "\\cellx%d", x);
        }
        bputc(o, '\n');
        for (int k = 0; k < ncols; k++) {
            Cell *ce = &cells[(size_t)rI * ncols + k];
            char a = al[k];
            spacer_line(r, 1, r->B * 4, "\\par");   /* cell padding, see spacer_line() */
            bprintf(o, "\\pard\\intbl\\sb0\\sa0%s\\plain\\f0\\fs%d\\cf%d%s ",
                    a == 'c' ? "\\qc" : a == 'r' ? "\\qr" : "", r->B, C_TEXT, rI == 0 ? "\\b" : "");
            r->curIndent = c->indent;
            r->curInTbl = 1;
            cell_inline(r, ce->s, ce->n);
            bputs(o, "\\par\n");
            spacer_line(r, 1, r->B * 4, "\\cell");
        }
        bputs(o, "\\row\n");
    }
    spacer(r, c, r->B * 10);
    r->nblocks++;
    free(cells);
    return j;
}

static int html_visible(const char *s, int n) {
    int in = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '<') {
            if (starts_ci(s + i, n - i, "<img") || starts_ci(s + i, n - i, "<hr")) return 1;
            in = 1;
        } else if (s[i] == '>') {
            in = 0;
        } else if (!in && !is_ws((unsigned char)s[i])) {
            return 1;
        }
    }
    return 0;
}

static void html_block(R *r, Ctx *c, Line *L, int n) {
    Buf j = {0};
    for (int i = 0; i < n; i++) {
        if (i) bputc(&j, '\n');
        int ind = ln_indent(&L[i]);
        bput(&j, L[i].s + ind, (size_t)(L[i].n - ind));
    }
    const char *s = j.p ? j.p : "";
    int len = (int)j.n;
    int center = find_str_ci(s, len, 0, "align=\"center\"") >= 0 || find_str_ci(s, len, 0, "align=center") >= 0 ||
                 find_str_ci(s, len, 0, "<center") >= 0;
    int right = find_str_ci(s, len, 0, "align=\"right\"") >= 0;
    int align = center ? 1 : right ? 2 : 0;
    if (len >= 3 && s[0] == '<' && tolower((unsigned char)s[1]) == 'h' && s[2] >= '1' && s[2] <= '6' &&
        (len == 3 || s[3] == '>' || s[3] == ' ')) {
        heading(r, c, s[2] - '0', s, len, align);
    } else if (starts_ci(s, len, "<hr") && !html_visible(s + 3, len - 3)) {
        hr_block(r, c);
    } else if (html_visible(s, len)) {
        para_begin(r, c, 0, para_sa(r, c), align);
        inl(r, s, len);
    }
    free(j.p);
}

static void paragraph(R *r, Ctx *c, Line *L, int n) {
    Buf j = {0};
    for (int i = 0; i < n; i++) {
        if (i) bputc(&j, '\n');
        int ind = ln_indent(&L[i]);
        int len = L[i].n - ind;
        if (i == n - 1) while (len > 0 && L[i].s[ind + len - 1] == ' ') len--;
        bput(&j, L[i].s + ind, (size_t)len);
    }
    para_begin(r, c, 0, para_sa(r, c), 0);
    inl(r, j.p ? j.p : "", (int)j.n);
    free(j.p);
}

static void blocks(R *r, Line *L, int n, Ctx c) {
    int i = 0;
    while (i < n) {
        Line *l = &L[i];
        if (l->skip || ln_blank(l)) { i++; continue; }
        int ind = ln_indent(l);
        if (ind >= 4) {   /* indented code */
            int j = i, last = i;
            while (j < n && (ln_blank(&L[j]) || ln_indent(&L[j]) >= 4)) {
                if (!ln_blank(&L[j])) last = j;
                j++;
            }
            Line *cl = NULL;
            int cn = 0, cc = 0;
            for (int k = i; k <= last; k++)
                push_line(&cl, &cn, &cc, L[k].n >= 4 ? L[k].s + 4 : L[k].s, L[k].n >= 4 ? L[k].n - 4 : 0);
            code_block(r, &c, cl, cn, "", 0);
            free(cl);
            i = last + 1;
            continue;
        }
        const char *t = l->s + ind;
        int tn = l->n - ind;
        char fc;
        int fl = fence_len(t, tn, &fc);
        if (fl) {
            const char *info = t + fl;
            int infn = tn - fl;
            while (infn > 0 && *info == ' ') { info++; infn--; }
            int j = i + 1;
            Line *cl = NULL;
            int cn = 0, cc = 0;
            while (j < n) {
                Line *m = &L[j];
                int mi = ln_indent(m);
                if (mi < 4 && run_of(m->s, m->n, mi, fc) >= fl) {
                    int k = mi + run_of(m->s, m->n, mi, fc);
                    while (k < m->n && m->s[k] == ' ') k++;
                    if (k == m->n) break;
                }
                int strip = mi < ind ? mi : ind;
                push_line(&cl, &cn, &cc, m->s + strip, m->n - strip);
                j++;
            }
            code_block(r, &c, cl, cn, info, infn);
            free(cl);
            i = j < n ? j + 1 : j;
            continue;
        }
        int lv = atx_level(t, tn);
        if (lv) {
            const char *h = t + lv;
            int hn = tn - lv;
            while (hn > 0 && *h == ' ') { h++; hn--; }
            while (hn > 0 && h[hn - 1] == ' ') hn--;
            int k = hn;
            while (k > 0 && h[k - 1] == '#') k--;
            if (k == 0 || h[k - 1] == ' ') { hn = k; while (hn > 0 && h[hn - 1] == ' ') hn--; }
            heading(r, &c, lv, h, hn, 0);
            i++;
            continue;
        }
        if (is_hr(t, tn)) { hr_block(r, &c); i++; continue; }
        if (t[0] == '>') {
            Line *ql = NULL;
            int qn = 0, qc = 0, j = i;
            while (j < n && !L[j].skip && !ln_blank(&L[j])) {
                Line *m = &L[j];
                int mi = ln_indent(m);
                const char *mt = m->s + mi;
                int mn = m->n - mi;
                if (mi < 4 && mn > 0 && mt[0] == '>') {
                    int k = 1;
                    if (k < mn && mt[k] == ' ') k++;
                    push_line(&ql, &qn, &qc, mt + k, mn - k);
                } else {
                    if (interrupts(L, j, n)) break;
                    push_line(&ql, &qn, &qc, mt, mn);
                }
                j++;
            }
            quote_block(r, &c, ql, qn);
            free(ql);
            i = j;
            continue;
        }
        {
            int o, num, ml;
            char mc;
            if (list_marker(t, tn, &o, &num, &mc, &ml)) { i = list_block(r, L, n, i, &c); continue; }
        }
        if (tn >= 4 && !memcmp(t, "<!--", 4)) {
            int j = i;
            while (j < n && find_str(L[j].s, L[j].n, j == i ? ind + 4 : 0, "-->") < 0) j++;
            i = j + 1;
            continue;
        }
        if (i + 1 < n && memchr(t, '|', (size_t)tn) && table_delim(&L[i + 1], NULL, NULL)) {
            i = table_block(r, L, n, i, &c);
            continue;
        }
        if (html_block_start(t, tn)) {
            int j = i;
            while (j < n && !L[j].skip && !ln_blank(&L[j])) j++;
            html_block(r, &c, L + i, j - i);
            i = j;
            continue;
        }
        /* paragraph (possibly a setext heading) */
        int j = i + 1, setext = 0;
        while (j < n && !L[j].skip && !ln_blank(&L[j])) {
            int mi = ln_indent(&L[j]);
            if (mi < 4) {
                setext = setext_level(L[j].s + mi, L[j].n - mi);
                if (setext) break;
            }
            if (interrupts(L, j, n)) break;
            j++;
        }
        if (setext) {
            Buf hb = {0};
            for (int k = i; k < j; k++) {
                if (k > i) bputc(&hb, ' ');
                int mi = ln_indent(&L[k]);
                int len = L[k].n - mi;
                while (len > 0 && L[k].s[mi + len - 1] == ' ') len--;
                bput(&hb, L[k].s + mi, (size_t)len);
            }
            heading(r, &c, setext, hb.p ? hb.p : "", (int)hb.n, 0);
            free(hb.p);
            i = j + 1;
            continue;
        }
        paragraph(r, &c, L + i, j - i);
        i = j;
    }
}

/* Collect [label]: url definitions and [^id]: footnotes. */
static void prepass(R *r, Line *L, int n) {
    int inFence = 0, fl = 0;
    char fc = 0;
    for (int i = 0; i < n; i++) {
        Line *l = &L[i];
        int ind = ln_indent(l);
        const char *t = l->s + ind;
        int tn = l->n - ind;
        if (inFence) {
            if (ind < 4 && run_of(t, tn, 0, fc) >= fl) inFence = 0;
            continue;
        }
        char c2;
        int k;
        if (ind < 4 && (k = fence_len(t, tn, &c2)) != 0) { inFence = 1; fc = c2; fl = k; continue; }
        if (ind >= 4 || tn < 4 || t[0] != '[') continue;
        int e = 1;
        while (e < tn && t[e] != ']') e++;
        if (e >= tn - 1 || e < 2 || t[e + 1] != ':') continue;
        const char *rest = t + e + 2;
        int rn = tn - e - 2;
        while (rn > 0 && *rest == ' ') { rest++; rn--; }
        if (t[1] == '^') {
            if (r->nfoots == r->capfoots) {
                r->capfoots = r->capfoots ? r->capfoots * 2 : 8;
                r->foots = (Foot *)realloc(r->foots, sizeof(Foot) * (size_t)r->capfoots);
            }
            Foot *f = &r->foots[r->nfoots++];
            f->id = (char *)malloc((size_t)e - 1);
            memcpy(f->id, t + 2, (size_t)e - 2);
            f->id[e - 2] = 0;
            f->text = (char *)malloc((size_t)rn + 1);
            memcpy(f->text, rest, (size_t)rn);
            f->text[rn] = 0;
            l->skip = 1;
            continue;
        }
        int un = 0;
        const char *u = rest;
        if (rn > 0 && *u == '<') {
            u++;
            while (un < rn - 1 && u[un] != '>') un++;
        } else {
            while (un < rn && u[un] != ' ') un++;
        }
        if (!un) continue;
        if (r->nrefs == r->caprefs) {
            r->caprefs = r->caprefs ? r->caprefs * 2 : 16;
            r->refs = (Ref *)realloc(r->refs, sizeof(Ref) * (size_t)r->caprefs);
        }
        Ref *rf = &r->refs[r->nrefs++];
        rf->ln = e - 1;
        rf->label = (char *)malloc((size_t)e);
        memcpy(rf->label, t + 1, (size_t)e - 1);
        rf->label[e - 1] = 0;
        rf->url = (char *)malloc((size_t)un + 1);
        memcpy(rf->url, u, (size_t)un);
        rf->url[un] = 0;
        l->skip = 1;
    }
}

typedef struct {
    int margin, W, B;   /* twips: content spans margin..W */
    const DWORD *pal;
    int dark;
    const WCHAR *baseDir;
} MdOpts;

static char *md_to_rtf(const char *src, size_t srcLen, const MdOpts *opt, size_t *outLen, Head **heads, int *nheads) {
    /* normalize: drop CR, expand tabs to 4-column stops */
    Buf ex = {0};
    bgrow(&ex, srcLen + srcLen / 8 + 16);
    int col = 0;
    for (size_t i = 0; i < srcLen; i++) {
        char ch = src[i];
        if (ch == '\r') continue;
        if (ch == '\n') { bputc(&ex, '\n'); col = 0; continue; }
        if (ch == '\t') { int k = 4 - (col % 4); while (k--) { bputc(&ex, ' '); col++; } continue; }
        bputc(&ex, ch);
        if (((unsigned char)ch & 0xC0) != 0x80) col++;
    }
    Line *L = NULL;
    int nl = 0, capl = 0;
    {
        const char *p = ex.p ? ex.p : "";
        const char *end = p + ex.n;
        while (p < end) {
            const char *e = memchr(p, '\n', (size_t)(end - p));
            if (!e) e = end;
            push_line(&L, &nl, &capl, p, (int)(e - p));
            p = e + 1;
        }
    }

    R r;
    memset(&r, 0, sizeof r);
    r.W = opt->W;
    r.margin = opt->margin;
    r.B = opt->B;
    r.pal = opt->pal;
    r.dark = opt->dark;
    r.baseDir = opt->baseDir;
    Buf *o = &r.out;
    bgrow(o, ex.n * 2 + 4096);
    bputs(o, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss\\fcharset0 Segoe UI;}{\\f1\\fmodern\\fcharset0 Consolas;}}{\\colortbl;");
    for (int k = 1; k < C_COUNT; k++) {
        DWORD v = opt->pal[k];
        bprintf(o, "\\red%d\\green%d\\blue%d;", (int)((v >> 16) & 255), (int)((v >> 8) & 255), (int)(v & 255));
    }
    bprintf(o, "}\\viewkind4\\f0\\fs%d\\cf%d\n", r.B, C_TEXT);

    prepass(&r, L, nl);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.indent = r.margin;
    spacer(&r, &c, r.B * 12);
    int first = 0;
    /* YAML front matter */
    if (nl > 1 && L[0].n >= 3 && !memcmp(L[0].s, "---", 3) && ln_blank(&(Line){ L[0].s + 3, L[0].n - 3, 0 })) {
        for (int k = 1; k < nl && k < 400; k++) {
            if ((L[k].n >= 3 && (!memcmp(L[k].s, "---", 3) || !memcmp(L[k].s, "...", 3))) &&
                ln_blank(&(Line){ L[k].s + 3, L[k].n - 3, 0 })) {
                code_block(&r, &c, L + 1, k - 1, "yaml", 4);
                first = k + 1;
                break;
            }
        }
    }
    blocks(&r, L + first, nl - first, c);

    if (r.nfoots) {
        close_para(&r);
        spacer(&r, &c, r.B * 6);
        rule_row(&r, &c, 15);
        spacer(&r, &c, r.B * 4);
        for (int k = 0; k < r.nfoots; k++) {
            para_begin(&r, &c, 0, r.B * 3, 0);
            bprintf(o, "\\fs%d\\cf%d {\\super ", r.B - 3, C_MUTED);
            rtf_text(o, r.foots[k].id, (int)strlen(r.foots[k].id));
            bputs(o, "} ");
            inl(&r, r.foots[k].text, (int)strlen(r.foots[k].text));
        }
    }
    close_para(&r);
    bputs(o, "}");

    for (int k = 0; k < r.nrefs; k++) { free(r.refs[k].label); free(r.refs[k].url); }
    for (int k = 0; k < r.nfoots; k++) { free(r.foots[k].id); free(r.foots[k].text); }
    free(r.refs);
    free(r.foots);
    free(L);
    free(ex.p);
    *heads = r.heads;
    *nheads = r.nheads;
    *outLen = o->n;
    return o->p;
}

/* ======================================================================
 * Application state
 * ====================================================================== */

static HINSTANCE g_hInst;
static HWND g_hMain, g_hEdit, g_hFind;
static WNDPROC g_editProc;
static UINT g_findMsg;
static FINDREPLACEW g_fr;
static WCHAR g_findText[256];
static HACCEL g_hAccel;

static WCHAR g_path[MAX_PATH * 2];   /* current file; empty = built-in help page */
static int g_isMd, g_raw;
static char *g_src;                  /* UTF-8 source (markdown) */
static size_t g_srcLen;
static WCHAR *g_wtext;               /* UTF-16 text (plain view) */
static size_t g_wlen;
static FILETIME g_mtime;
static ULONGLONG g_fsize;
static Head *g_heads;
static int g_nheads;

static int g_zoom = 100, g_theme = 0, g_dark = 0, g_wrap = 1, g_topmost = 0;
static int g_contentPx, g_layoutPx;
static int g_dpi = 96;
static WCHAR g_ini[MAX_PATH];

#define HIST_MAX 64
static WCHAR *g_back[HIST_MAX], *g_fwd[HIST_MAX];
static int g_nBack, g_nFwd;

#ifdef MDZY_DEV
static FILE *g_devLog;
static const WCHAR *g_devRtf;
#endif

static const char kHelpMd[] =
    "# mdzy " MDZY_VERSION "\n"
    "\n"
    "**mdzy** stands for **MD easy**: a tiny, instant Markdown & text viewer. Drop a `.md` or `.txt` file "
    "on this window, or press **Ctrl+O**.\n"
    "\n"
    "<p align=\"center\"><a href=\"mdzy:register\">Open .md files with mdzy</a></p>\n"
    "\n"
    "## Keyboard\n"
    "\n"
    "| Keys | Action |\n"
    "|---|---|\n"
    "| `Ctrl+O` | Open a file |\n"
    "| `F5` / `Ctrl+R` | Reload (files also reload automatically when they change) |\n"
    "| `Ctrl+F`, `F3`, `Shift+F3` | Find, find next / previous |\n"
    "| `Ctrl+U` | Toggle Markdown source view |\n"
    "| `Ctrl+Plus` / `Ctrl+Minus` / `Ctrl+0` | Zoom in / out / reset (also `Ctrl+Wheel`) |\n"
    "| `Ctrl+D` | Toggle dark / light theme |\n"
    "| `Alt+Z` | Toggle word wrap (plain text) |\n"
    "| `Ctrl+T` | Always on top |\n"
    "| `Alt+Left` / `Backspace`, `Alt+Right` | Back / forward between linked documents |\n"
    "| `Ctrl+E` | Open the file in an editor |\n"
    "| `Ctrl+Shift+C` | Copy the file path |\n"
    "| `F1` | This page |\n"
    "| `Esc` / `Ctrl+W` | Close |\n"
    "\n"
    "Right-click anywhere for the menu.\n"
    "\n"
    "## File associations\n"
    "\n"
    "The button at the top of this page, or right-click > **File associations > Register**, adds mdzy "
    "to *Open with* for `.md`, `.markdown` and `.txt` (per-user, no admin rights needed). Windows then "
    "asks you to confirm the default app in *Settings > Default apps*.\n"
    "\n"
    "From a terminal: `mdzy.exe --register` or `mdzy.exe --unregister`.\n"
    "\n"
    "## Markdown support\n"
    "\n"
    "Headings, paragraphs, **bold**, *italic*, ~~strikethrough~~, `inline code`, fenced code blocks with "
    "light syntax highlighting, block quotes and GitHub alerts, ordered / unordered / task lists, tables, "
    "links (relative `.md` links open in place), local images (PNG, JPEG, GIF, BMP, WebP...), footnotes, "
    "YAML front matter and common inline HTML.\n"
    "\n"
    "> [!TIP]\n"
    "> Settings are stored in `%APPDATA%\\mdzy\\mdzy.ini`, or in `mdzy.ini` next to the exe if that file "
    "exists (portable mode).\n"
    "\n"
    "---\n"
    "\n"
    "mdzy (MD easy) " MDZY_VERSION " - <https://github.com/vladcherry/mdzy>\n";

/* ======================================================================
 * Settings
 * ====================================================================== */

static void InitIniPath(void) {
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    swprintf(g_ini, MAX_PATH, L"%ls\\mdzy.ini", exe);
    if (GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES) return;   /* portable mode */
    WCHAR app[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, app)))
        swprintf(g_ini, MAX_PATH, L"%ls\\mdzy\\mdzy.ini", app);
}
static int IniInt(const WCHAR *key, int def) { return (int)GetPrivateProfileIntW(L"mdzy", key, def, g_ini); }
static void IniSetInt(const WCHAR *key, int v) {
    WCHAR b[32];
    swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"mdzy", key, b, g_ini);
}

static void SaveSettings(void) {
    WCHAR dir[MAX_PATH];
    lstrcpynW(dir, g_ini, MAX_PATH);
    PathRemoveFileSpecW(dir);
    CreateDirectoryW(dir, NULL);
    WINDOWPLACEMENT wp = { sizeof wp };
    GetWindowPlacement(g_hMain, &wp);
    IniSetInt(L"x", wp.rcNormalPosition.left);
    IniSetInt(L"y", wp.rcNormalPosition.top);
    IniSetInt(L"w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    IniSetInt(L"h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    IniSetInt(L"max", wp.showCmd == SW_SHOWMAXIMIZED);
    IniSetInt(L"zoom", g_zoom);
    IniSetInt(L"theme", g_theme);
    IniSetInt(L"wrap", g_wrap);
    IniSetInt(L"topmost", g_topmost);
}

/* ======================================================================
 * File associations (per-user, HKCU)
 * ====================================================================== */

static const WCHAR *kMdExts[] = { L".md", L".markdown", L".mdown", L".mkd", L".mkdn", L".mdwn", L".mdtxt", L".mdtext" };
static const WCHAR *kTxtExts[] = { L".txt" };
#define CLS L"Software\\Classes\\"

static void RegStr(const WCHAR *sub, const WCHAR *name, const WCHAR *val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
}
static void RegNone(const WCHAR *sub, const WCHAR *name) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_NONE, NULL, 0);
    RegCloseKey(k);
}
static void RegDelValue(const WCHAR *sub, const WCHAR *name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}

static void RegisterProgId(const WCHAR *progId, const WCHAR *desc, const WCHAR *icon, const WCHAR *cmd) {
    WCHAR k[256];
    swprintf(k, 256, CLS L"%ls", progId);
    RegStr(k, NULL, desc);
    swprintf(k, 256, CLS L"%ls\\DefaultIcon", progId);
    RegStr(k, NULL, icon);
    swprintf(k, 256, CLS L"%ls\\shell\\open\\command", progId);
    RegStr(k, NULL, cmd);
}

static void RegisterAssoc(void) {
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16], icon[MAX_PATH + 8], k[256];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" \"%%1\"", exe);
    swprintf(icon, ARRAYSIZE(icon), L"%ls,0", exe);

    RegisterProgId(L"mdzy.md", L"Markdown Document", icon, cmd);
    RegisterProgId(L"mdzy.txt", L"Text Document", icon, cmd);

    RegStr(CLS L"Applications\\mdzy.exe", L"FriendlyAppName", L"mdzy");
    RegStr(CLS L"Applications\\mdzy.exe\\shell\\open\\command", NULL, cmd);
    RegStr(CLS L"Applications\\mdzy.exe\\DefaultIcon", NULL, icon);

    RegStr(L"Software\\mdzy\\Capabilities", L"ApplicationName", L"mdzy");
    RegStr(L"Software\\mdzy\\Capabilities", L"ApplicationDescription", L"Tiny, instant Markdown & text viewer");
    RegStr(L"Software\\mdzy\\Capabilities", L"ApplicationIcon", icon);
    RegStr(L"Software\\RegisteredApplications", L"mdzy", L"Software\\mdzy\\Capabilities");

    for (size_t i = 0; i < ARRAYSIZE(kMdExts) + ARRAYSIZE(kTxtExts); i++) {
        int isMd = i < ARRAYSIZE(kMdExts);
        const WCHAR *ext = isMd ? kMdExts[i] : kTxtExts[i - ARRAYSIZE(kMdExts)];
        const WCHAR *prog = isMd ? L"mdzy.md" : L"mdzy.txt";
        swprintf(k, 256, CLS L"%ls\\OpenWithProgids", ext);
        RegNone(k, prog);
        RegStr(CLS L"Applications\\mdzy.exe\\SupportedTypes", ext, L"");
        RegStr(L"Software\\mdzy\\Capabilities\\FileAssociations", ext, prog);
        if (isMd) {
            /* Claim the default only if no one else has. */
            WCHAR cur[128] = L"";
            DWORD sz = sizeof cur;
            LSTATUS st = RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL, cur, &sz);
            if (st != ERROR_SUCCESS || !cur[0]) {
                swprintf(k, 256, CLS L"%ls", ext);
                RegStr(k, NULL, prog);
            }
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static void UnregisterAssoc(void) {
    WCHAR k[256];
    RegDeleteTreeW(HKEY_CURRENT_USER, CLS L"mdzy.md");
    RegDeleteTreeW(HKEY_CURRENT_USER, CLS L"mdzy.txt");
    RegDeleteTreeW(HKEY_CURRENT_USER, CLS L"Applications\\mdzy.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\mdzy");
    RegDelValue(L"Software\\RegisteredApplications", L"mdzy");
    for (size_t i = 0; i < ARRAYSIZE(kMdExts) + ARRAYSIZE(kTxtExts); i++) {
        int isMd = i < ARRAYSIZE(kMdExts);
        const WCHAR *ext = isMd ? kMdExts[i] : kTxtExts[i - ARRAYSIZE(kMdExts)];
        swprintf(k, 256, CLS L"%ls\\OpenWithProgids", ext);
        RegDelValue(k, isMd ? L"mdzy.md" : L"mdzy.txt");
        swprintf(k, 256, CLS L"%ls", ext);
        WCHAR cur[128] = L"";
        DWORD sz = sizeof cur;
        if (RegGetValueW(HKEY_CURRENT_USER, k, NULL, RRF_RT_REG_SZ, NULL, cur, &sz) == ERROR_SUCCESS &&
            !wcscmp(cur, L"mdzy.md"))
            RegDelValue(k, NULL);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

/* ======================================================================
 * Document loading & rendering
 * ====================================================================== */

static const DWORD *Pal(void) { return g_dark ? kDark : kLight; }
static int IsMdView(void) { return g_isMd && !g_raw; }

static int IsMdPath(const WCHAR *path) {
    const WCHAR *ext = PathFindExtensionW(path);
    for (size_t i = 0; i < ARRAYSIZE(kMdExts); i++)
        if (!_wcsicmp(ext, kMdExts[i])) return 1;
    return !_wcsicmp(ext, L".rmd") || !_wcsicmp(ext, L".qmd");
}

static WCHAR *DecodeText(const unsigned char *d, size_t n, size_t *outLen) {
    size_t off = 0;
    int u16 = 0, be = 0;
    WCHAR *w;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) off = 3;
    else if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { u16 = 1; off = 2; }
    else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { u16 = 1; be = 1; off = 2; }
    else if (n >= 4 && d[0] && !d[1] && d[2] && !d[3]) u16 = 1;
    if (u16) {
        size_t cnt = (n - off) / 2;
        w = (WCHAR *)malloc((cnt + 1) * sizeof(WCHAR));
        memcpy(w, d + off, cnt * 2);
        if (be) for (size_t i = 0; i < cnt; i++) w[i] = (WCHAR)((w[i] >> 8) | (w[i] << 8));
        w[cnt] = 0;
        *outLen = cnt;
    } else {
        UINT cp = CP_UTF8;
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)d + off, (int)(n - off), NULL, 0);
        if (len == 0 && n > off) {
            cp = CP_ACP;
            len = MultiByteToWideChar(CP_ACP, 0, (const char *)d + off, (int)(n - off), NULL, 0);
        }
        w = (WCHAR *)malloc(((size_t)len + 1) * sizeof(WCHAR));
        MultiByteToWideChar(cp, 0, (const char *)d + off, (int)(n - off), w, len);
        w[len] = 0;
        *outLen = (size_t)len;
    }
    for (size_t i = 0; i < *outLen; i++) if (!w[i]) w[i] = ' ';
    return w;
}

typedef struct { const char *p; size_t n, pos; } StreamSrc;

static DWORD CALLBACK StreamCb(DWORD_PTR ck, LPBYTE buf, LONG cb, LONG *pcb) {
    StreamSrc *s = (StreamSrc *)ck;
    size_t left = s->n - s->pos;
    LONG k = (LONG)(left < (size_t)cb ? left : (size_t)cb);
    memcpy(buf, s->p + s->pos, (size_t)k);
    s->pos += (size_t)k;
    *pcb = k;
    return 0;
}
static void StreamIn(const void *p, size_t n, UINT fmt) {
    StreamSrc s = { (const char *)p, n, 0 };
    EDITSTREAM es = { (DWORD_PTR)&s, 0, StreamCb };
    SendMessageW(g_hEdit, EM_STREAMIN, fmt, (LPARAM)&es);
}

static void UpdateTitle(void) {
    WCHAR t[MAX_PATH * 2 + 64];
    if (g_path[0])
        swprintf(t, ARRAYSIZE(t), L"%ls%ls \x2014 mdzy " APP_VERSION, PathFindFileNameW(g_path), g_raw ? L" [source]" : L"");
    else
        lstrcpyW(t, L"mdzy " APP_VERSION L" \x2014 MD easy");
    SetWindowTextW(g_hMain, t);
}

/* Page margins live inside the document (paragraph indents, table offsets) rather than in an
 * EM_SETRECT inset: RichEdit never repaints an inset, so stale pixels would pile up there. */
static int g_padPx;

static int PxToTw(int px) { return (int)((long long)px * 1440 / g_dpi * 100 / g_zoom); }

static void UpdateRect(void) {
    RECT rc;
    GetClientRect(g_hMain, &rc);
    int w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
    int pad = MulDiv(28, g_dpi, 96);
    int maxw = MulDiv(MulDiv(920, g_dpi, 96), g_zoom, 100);
    if (w - 2 * pad > maxw) pad = (w - maxw) / 2;
    if (w - 2 * pad < 100) pad = (w - 100) / 2 > 0 ? (w - 100) / 2 : 0;
    g_padPx = pad;
    g_contentPx = w;
}

static void FreeHeads(void) {
    for (int i = 0; i < g_nheads; i++) { free(g_heads[i].slug); free(g_heads[i].text); }
    free(g_heads);
    g_heads = NULL;
    g_nheads = 0;
}

static void ApplyPlainFormat(void) {
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT |
                CFM_BACKCOLOR | CFM_CHARSET | CFM_OFFSET;
    cf.dwEffects = CFE_AUTOBACKCOLOR;
    cf.yHeight = 210;
    cf.crTextColor = HEXRGB(Pal()[C_TEXT]);
    cf.bCharSet = DEFAULT_CHARSET;
    lstrcpyW(cf.szFaceName, L"Consolas");
    SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_STARTINDENT | PFM_RIGHTINDENT | PFM_OFFSET | PFM_ALIGNMENT | PFM_SPACEBEFORE |
                PFM_SPACEAFTER | PFM_LINESPACING | PFM_TABSTOPS;
    pf.wAlignment = PFA_LEFT;
    pf.dxStartIndent = pf.dxRightIndent = MulDiv(10, 1440, 96);
    pf.cTabCount = MAX_TAB_STOPS;
    for (int i = 0; i < MAX_TAB_STOPS; i++) pf.rgxTabs[i] = (i + 1) * 462;   /* 4 Consolas columns */
    SendMessageW(g_hEdit, EM_SETSEL, 0, -1);
    SendMessageW(g_hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(g_hEdit, EM_SETSEL, 0, 0);
}

static void ScrollToCp(LONG cp) {
    LONG line = (LONG)SendMessageW(g_hEdit, EM_EXLINEFROMCHAR, 0, cp);
    LONG first = (LONG)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(g_hEdit, EM_LINESCROLL, 0, line - first);
}

/* keep: 0 = top, 1 = keep the first visible character, 2 = keep relative scroll position */
static void Render(int keep) {
    LONG cpTop = 0;
    double ratio = 0;
    if (keep == 1) {
        LONG line = (LONG)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
        cpTop = (LONG)SendMessageW(g_hEdit, EM_LINEINDEX, line, 0);
    } else if (keep == 2) {
        SCROLLINFO si = { sizeof si, SIF_ALL };
        GetScrollInfo(g_hEdit, SB_VERT, &si);
        if (si.nMax > (int)si.nPage) ratio = (double)si.nPos / (double)(si.nMax - (int)si.nPage + 1);
    }
    SendMessageW(g_hEdit, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hEdit, EM_SETREADONLY, FALSE, 0);   /* pictures are dropped in read-only mode */
    SendMessageW(g_hEdit, EM_SETBKGNDCOLOR, 0, HEXRGB(Pal()[C_BG]));
    UpdateRect();
    FreeHeads();
    if (IsMdView()) {
        WCHAR base[MAX_PATH * 2];
        if (g_path[0]) { lstrcpynW(base, g_path, ARRAYSIZE(base)); PathRemoveFileSpecW(base); }
        else { GetModuleFileNameW(NULL, base, MAX_PATH); PathRemoveFileSpecW(base); }
        MdOpts o;
        o.margin = PxToTw(g_padPx);
        o.W = PxToTw(g_contentPx - g_padPx) - 40;
        if (o.W < o.margin + 1440) o.W = o.margin + 1440;
        o.B = 22;
        o.pal = Pal();
        o.dark = g_dark;
        o.baseDir = base;
        size_t rn = 0;
        char *rtf = md_to_rtf(g_src ? g_src : "", g_srcLen, &o, &rn, &g_heads, &g_nheads);
#ifdef MDZY_DEV
        if (g_devRtf) {
            FILE *f = _wfopen(g_devRtf, L"wb");
            if (f) { fwrite(rtf, 1, rn, f); fclose(f); }
        }
#endif
        SendMessageW(g_hEdit, EM_AUTOURLDETECT, 0, 0);   /* the parser makes its own links */
        StreamIn(rtf, rn, SF_RTF);
        free(rtf);
        g_layoutPx = g_contentPx;
    } else {
        SendMessageW(g_hEdit, EM_AUTOURLDETECT, g_wlen < (4u << 20) ? AURL_ENABLEURL | AURL_ENABLEEMAILADDR : 0, 0);
        /* Format the empty document first so the streamed text simply inherits it;
         * reformatting afterwards is extremely slow on large files. */
        SETTEXTEX st = { ST_DEFAULT, 1200 };
        SendMessageW(g_hEdit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)L"");
        ApplyPlainFormat();
        DWORD t0 = GetTickCount();
        StreamIn(g_wtext ? g_wtext : L"", g_wlen * sizeof(WCHAR), SF_TEXT | SF_UNICODE);
        (void)t0;
#ifdef MDZY_DEV
        if (g_devRtf) {
            FILE *f = _wfopen(g_devRtf, L"a");
            if (f) { fprintf(f, "stream %lu ms\n", GetTickCount() - t0); fclose(f); }
        }
#endif
    }
    SendMessageW(g_hEdit, EM_SETTARGETDEVICE, 0, (!IsMdView() && !g_wrap) ? 1 : 0);
    SendMessageW(g_hEdit, EM_SETREADONLY, TRUE, 0);
    SendMessageW(g_hEdit, EM_SETSEL, 0, 0);
    if (keep == 1 && cpTop > 0) {
        ScrollToCp(cpTop);
    } else if (keep == 2 && ratio > 0) {
        SCROLLINFO si = { sizeof si, SIF_ALL };
        GetScrollInfo(g_hEdit, SB_VERT, &si);
        POINT pt = { 0, (LONG)(ratio * (si.nMax - (int)si.nPage + 1)) };
        SendMessageW(g_hEdit, EM_SETSCROLLPOS, 0, (LPARAM)&pt);
    }
    SendMessageW(g_hEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hEdit, NULL, TRUE);
    UpdateTitle();
}

static void SetSourceHelp(void) {
    free(g_src);
    free(g_wtext);
    g_srcLen = sizeof kHelpMd - 1;
    g_src = (char *)malloc(g_srcLen + 1);
    memcpy(g_src, kHelpMd, g_srcLen + 1);
    g_wtext = utf8_to_wide(g_src, (int)g_srcLen);
    g_wlen = wcslen(g_wtext);
    g_isMd = 1;
    g_path[0] = 0;
}

static BOOL StatFile(const WCHAR *path, FILETIME *mt, ULONGLONG *sz) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return FALSE;
    *mt = fa.ftLastWriteTime;
    *sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return TRUE;
}

static void PushHist(WCHAR **stack, int *n, const WCHAR *path) {
    if (*n == HIST_MAX) { free(stack[0]); memmove(stack, stack + 1, sizeof(WCHAR *) * (HIST_MAX - 1)); (*n)--; }
    stack[(*n)++] = _wcsdup(path);
}
static void ClearHist(WCHAR **stack, int *n) {
    while (*n > 0) free(stack[--(*n)]);
}

/* Load a file into the viewer. keep: see Render(). */
static BOOL LoadFile(const WCHAR *path, int keep, BOOL quiet) {
    WCHAR full[MAX_PATH * 2];
    if (!GetFullPathNameW(path, ARRAYSIZE(full), full, NULL)) lstrcpynW(full, path, ARRAYSIZE(full));
    size_t n = 0;
    unsigned char *d = read_file(full, &n, (size_t)512 << 20);
    if (!d) {
        if (!quiet) {
            WCHAR msg[MAX_PATH * 2 + 64];
            swprintf(msg, ARRAYSIZE(msg), L"Cannot open file:\n%ls", full);
            MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONWARNING);
        }
        return FALSE;
    }
    size_t wl = 0;
    WCHAR *w = DecodeText(d, n, &wl);
    free(d);
    free(g_wtext);
    free(g_src);
    g_wtext = w;
    g_wlen = wl;
    g_isMd = IsMdPath(full);
    g_src = NULL;
    g_srcLen = 0;
    if (g_isMd) {
        int ul = 0;
        g_src = wide_to_utf8(w, (int)wl, &ul);
        g_srcLen = (size_t)ul;
    } else {
        g_raw = 0;
    }
    if (_wcsicmp(full, g_path)) g_raw = 0;
    lstrcpynW(g_path, full, ARRAYSIZE(g_path));
    StatFile(full, &g_mtime, &g_fsize);
    Render(keep);
    if (!quiet) SHAddToRecentDocs(SHARD_PATHW, full);
    return TRUE;
}

static void ShowHelp(void) {
    SetSourceHelp();
    g_raw = 0;
    Render(0);
}

/* Navigate to a document, recording history. */
static void Navigate(const WCHAR *path) {
    WCHAR prev[MAX_PATH * 2];
    lstrcpynW(prev, g_path, ARRAYSIZE(prev));
    int hadHelp = !prev[0];
    if (LoadFile(path, 0, FALSE)) {
        if (!hadHelp && _wcsicmp(prev, g_path)) PushHist(g_back, &g_nBack, prev);
        else if (hadHelp && g_nBack == 0) PushHist(g_back, &g_nBack, L"");
        ClearHist(g_fwd, &g_nFwd);
    }
}

static void GoHistory(int dir) {
    WCHAR **from = dir < 0 ? g_back : g_fwd, **to = dir < 0 ? g_fwd : g_back;
    int *nf = dir < 0 ? &g_nBack : &g_nFwd, *nt = dir < 0 ? &g_nFwd : &g_nBack;
    if (*nf == 0) return;
    WCHAR *p = from[--(*nf)];
    PushHist(to, nt, g_path);
    if (!p[0]) ShowHelp();
    else if (!LoadFile(p, 0, FALSE)) (*nt)--;
    free(p);
}

static void ScrollToAnchor(const WCHAR *frag) {
    const WCHAR *text = NULL;
    for (int i = 0; i < g_nheads; i++)
        if (!lstrcmpiW(g_heads[i].slug, frag)) { text = g_heads[i].text; break; }
    if (!text || !text[0]) return;
    FINDTEXTEXW ft;
    ft.chrg.cpMin = 0;
    ft.chrg.cpMax = -1;
    ft.lpstrText = text;
    for (;;) {
        LONG pos = (LONG)SendMessageW(g_hEdit, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, (LPARAM)&ft);
        if (pos < 0) break;
        CHARFORMAT2W cf;
        memset(&cf, 0, sizeof cf);
        cf.cbSize = sizeof cf;
        SendMessageW(g_hEdit, EM_SETSEL, pos, pos + 1);
        SendMessageW(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        if (cf.dwEffects & CFE_BOLD) {
            SendMessageW(g_hEdit, EM_SETSEL, pos, pos);
            ScrollToCp(pos);
            return;
        }
        ft.chrg.cpMin = ft.chrgText.cpMax;
    }
    SendMessageW(g_hEdit, EM_SETSEL, 0, 0);
}

static int IsViewablePath(const WCHAR *p) {
    const WCHAR *ext = PathFindExtensionW(p);
    static const WCHAR *txt[] = { L".txt", L".text", L".log", L".ini", L".cfg", L".conf", L".csv", L".tsv", L".json",
                                  L".yaml", L".yml", L".toml", L".xml", L".nfo", L".diz", L"", NULL };
    if (IsMdPath(p)) return 1;
    for (int i = 0; txt[i]; i++) if (!_wcsicmp(ext, txt[i])) return 1;
    return 0;
}

static void OpenLink(const WCHAR *url) {
    while (*url == ' ') url++;
    if (!url[0]) return;
    if (url[0] == '#') {
        int ul = 0;
        char *u8 = wide_to_utf8(url + 1, -1, &ul);
        WCHAR *frag = url_decode_wide(u8, (int)strlen(u8));
        ScrollToAnchor(frag);
        free(frag);
        free(u8);
        return;
    }
    if (!_wcsnicmp(url, L"mdzy:", 5)) {   /* in-document app commands, e.g. the README button */
        if (!_wcsicmp(url + 5, L"register") &&
            MessageBoxW(g_hMain, L"Register mdzy as a viewer for .md, .markdown and .txt files?",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
            PostMessageW(g_hMain, WM_COMMAND, CMD_REGISTER, 0);
        return;
    }
    if (!_wcsnicmp(url, L"http://", 7) || !_wcsnicmp(url, L"https://", 8) || !_wcsnicmp(url, L"mailto:", 7) ||
        !_wcsnicmp(url, L"ftp://", 6)) {
        ShellExecuteW(g_hMain, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    /* another URI scheme (not a drive letter / file:) */
    const WCHAR *colon = wcschr(url, ':');
    if (colon && colon - url > 1 && _wcsnicmp(url, L"file:", 5)) {
        int ok = 1;
        for (const WCHAR *p = url; p < colon; p++) if (!iswalnum(*p) && *p != '+' && *p != '-' && *p != '.') ok = 0;
        if (ok) {
            WCHAR msg[2200];
            swprintf(msg, ARRAYSIZE(msg), L"Open this link?\n\n%ls", url);
            if (MessageBoxW(g_hMain, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
                ShellExecuteW(g_hMain, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            return;
        }
    }
    WCHAR base[MAX_PATH * 2], path[MAX_PATH * 2], frag[256];
    if (g_path[0]) { lstrcpynW(base, g_path, ARRAYSIZE(base)); PathRemoveFileSpecW(base); }
    else GetCurrentDirectoryW(ARRAYSIZE(base), base);
    int ul = 0;
    char *u8 = wide_to_utf8(url, -1, &ul);
    int ok = resolve_local(base, u8, (int)strlen(u8), path, ARRAYSIZE(path), frag, ARRAYSIZE(frag));
    free(u8);
    if (!ok) return;
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        WCHAR msg[MAX_PATH * 2 + 64];
        swprintf(msg, ARRAYSIZE(msg), L"File not found:\n%ls", path);
        MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONWARNING);
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        ShellExecuteW(g_hMain, L"explore", path, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (IsViewablePath(path)) {
        Navigate(path);
        if (frag[0]) ScrollToAnchor(frag);
        return;
    }
    if (AssocIsDangerous(PathFindExtensionW(path))) {
        WCHAR msg[MAX_PATH * 2 + 128];
        swprintf(msg, ARRAYSIZE(msg), L"This link points to a program or script:\n\n%ls\n\nRun it?", path);
        if (MessageBoxW(g_hMain, msg, APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    ShellExecuteW(g_hMain, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* The URL of a hyperlink: RichEdit keeps the field instruction ("HYPERLINK "url"")
 * as hidden text in front of the friendly name. */
static WCHAR *GetRange(LONG a, LONG b) {
    if (a < 0) a = 0;
    if (b <= a) return _wcsdup(L"");
    WCHAR *buf = (WCHAR *)calloc((size_t)(b - a) + 2, sizeof(WCHAR));
    TEXTRANGEW tr;
    tr.chrg.cpMin = a;
    tr.chrg.cpMax = b;
    tr.lpstrText = buf;
    SendMessageW(g_hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    return buf;
}

static void CopyUrl(const WCHAR *s, WCHAR *out, int cap) {
    const WCHAR *e = wcschr(s, '"');
    int k = e ? (int)(e - s) : (int)wcslen(s);
    while (k > 0 && s[k - 1] == ' ') k--;
    if (k >= cap) k = cap - 1;
    memcpy(out, s, (size_t)k * sizeof(WCHAR));
    out[k] = 0;
}

/* The URL of a hyperlink. RichEdit keeps the field instruction (HYPERLINK "url") as hidden
 * text right before the friendly name; depending on the API the range may or may not include it.
 * Character offsets inside returned text are not reliable (tables add hidden delimiters), so
 * this only looks at the start of the range or the tail of the text preceding it. */
static int GetLinkUrl(CHARRANGE cr, WCHAR *out, int cap) {
    static const WCHAR kHl[] = L"HYPERLINK \"";
    out[0] = 0;
    if (cr.cpMax - cr.cpMin <= 0 || cr.cpMax - cr.cpMin > 8192) return 0;
    WCHAR *vis = GetRange(cr.cpMin, cr.cpMax);
    if (!wcsncmp(vis, kHl, 11)) {
        CopyUrl(vis + 11, out, cap);
    } else {
        WCHAR *pre = GetRange(cr.cpMin - 2100, cr.cpMin);
        size_t pl = wcslen(pre);
        WCHAR *p = NULL, *q = pre;
        while ((q = wcsstr(q, kHl)) != NULL) { p = q; q++; }
        if (p && pl > 0 && pre[pl - 1] == '"' && wcschr(p + 11, '"') == pre + pl - 1) CopyUrl(p + 11, out, cap);
        else lstrcpynW(out, vis, cap);
        free(pre);
    }
    free(vis);
    for (WCHAR *s = out; *s; s++) if (*s == '\r' || *s == '\n') *s = ' ';
    for (size_t k = wcslen(out); k > 0 && out[k - 1] == ' '; k--) out[k - 1] = 0;
    return out[0] != 0;
}

/* ======================================================================
 * Theme, zoom, find
 * ====================================================================== */

static int SystemDark(void) {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v == 0;
}

static void ApplyTheme(void) {
    g_dark = g_theme == 2 || (g_theme == 0 && SystemDark());
    BOOL d = g_dark;
    DwmSetWindowAttribute(g_hMain, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof d);
    SetWindowTheme(g_hEdit, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SendMessageW(g_hEdit, EM_SETBKGNDCOLOR, 0, HEXRGB(Pal()[C_BG]));
}

static void ApplyZoom(void) {
    if (g_zoom < 30) g_zoom = 30;
    if (g_zoom > 500) g_zoom = 500;
    SendMessageW(g_hEdit, EM_SETZOOM, g_zoom == 100 ? 0 : (WPARAM)g_zoom, g_zoom == 100 ? 0 : 100);
    if (IsMdView()) Render(1);
}

static void DoFind(int down) {
    if (!g_findText[0]) return;
    CHARRANGE sel;
    SendMessageW(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&sel);
    DWORD fl = (down ? FR_DOWN : 0) | (g_fr.Flags & (FR_MATCHCASE | FR_WHOLEWORD));
    FINDTEXTEXW ft;
    ft.lpstrText = g_findText;
    ft.chrg.cpMin = down ? sel.cpMax : sel.cpMin;
    ft.chrg.cpMax = down ? -1 : 0;
    LONG pos = (LONG)SendMessageW(g_hEdit, EM_FINDTEXTEXW, fl, (LPARAM)&ft);
    if (pos < 0) {   /* wrap around */
        ft.chrg.cpMin = down ? 0 : GetWindowTextLengthW(g_hEdit);
        ft.chrg.cpMax = down ? -1 : 0;
        pos = (LONG)SendMessageW(g_hEdit, EM_FINDTEXTEXW, fl, (LPARAM)&ft);
    }
    if (pos < 0) { MessageBeep(MB_ICONASTERISK); return; }
    SendMessageW(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
}

static void OpenFind(void) {
    if (g_hFind) { SetFocus(g_hFind); return; }
    CHARRANGE sel;
    SendMessageW(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&sel);
    if (sel.cpMax > sel.cpMin && sel.cpMax - sel.cpMin < (LONG)ARRAYSIZE(g_findText)) {
        TEXTRANGEW tr = { sel, g_findText };
        SendMessageW(g_hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    }
    memset(&g_fr, 0, sizeof g_fr);
    g_fr.lStructSize = sizeof g_fr;
    g_fr.hwndOwner = g_hMain;
    g_fr.lpstrFindWhat = g_findText;
    g_fr.wFindWhatLen = ARRAYSIZE(g_findText);
    g_fr.Flags = FR_DOWN;
    g_hFind = FindTextW(&g_fr);
}

/* ======================================================================
 * Commands & window procedures
 * ====================================================================== */

static void OpenDialog(void) {
    WCHAR file[MAX_PATH * 2] = L"", dir[MAX_PATH * 2] = L"";
    if (g_path[0]) { lstrcpynW(dir, g_path, ARRAYSIZE(dir)); PathRemoveFileSpecW(dir); }
    OPENFILENAMEW of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = g_hMain;
    of.lpstrFilter = L"Markdown and text\0*.md;*.markdown;*.mdown;*.mkd;*.mkdn;*.mdwn;*.txt;*.text;*.log\0"
                     L"Markdown\0*.md;*.markdown;*.mdown;*.mkd;*.mkdn;*.mdwn\0Text\0*.txt;*.text;*.log\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = ARRAYSIZE(file);
    of.lpstrInitialDir = dir[0] ? dir : NULL;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) Navigate(file);
}

static void CopyToClipboard(const WCHAR *s) {
    if (!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        memcpy(GlobalLock(h), s, n);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void OnCommand(int id) {
    switch (id) {
    case CMD_OPEN: OpenDialog(); break;
    case CMD_RELOAD: if (g_path[0]) LoadFile(g_path, 1, TRUE); break;
    case CMD_CLOSE: DestroyWindow(g_hMain); break;
    case CMD_FIND: OpenFind(); break;
    case CMD_FINDNEXT:
    case CMD_FINDPREV:
        if (!g_findText[0]) OpenFind();
        else DoFind(id == CMD_FINDNEXT);
        break;
    case CMD_ZOOMIN: g_zoom += g_zoom >= 200 ? 25 : 10; ApplyZoom(); break;
    case CMD_ZOOMOUT: g_zoom -= g_zoom > 200 ? 25 : 10; ApplyZoom(); break;
    case CMD_ZOOMRESET: g_zoom = 100; ApplyZoom(); break;
    case CMD_RAW:
        if (g_isMd) { g_raw = !g_raw; Render(2); }
        break;
    case CMD_THEME: g_theme = g_dark ? 1 : 2; ApplyTheme(); Render(1); break;
    case CMD_THEME_AUTO: g_theme = 0; ApplyTheme(); Render(1); break;
    case CMD_THEME_LIGHT: g_theme = 1; ApplyTheme(); Render(1); break;
    case CMD_THEME_DARK: g_theme = 2; ApplyTheme(); Render(1); break;
    case CMD_WRAP:
        g_wrap = !g_wrap;
        if (!IsMdView()) SendMessageW(g_hEdit, EM_SETTARGETDEVICE, 0, g_wrap ? 0 : 1);
        break;
    case CMD_TOPMOST:
        g_topmost = !g_topmost;
        SetWindowPos(g_hMain, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        break;
    case CMD_BACK: GoHistory(-1); break;
    case CMD_FORWARD: GoHistory(1); break;
    case CMD_EDIT:
        if (g_path[0] && (INT_PTR)ShellExecuteW(g_hMain, L"edit", g_path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            WCHAR arg[MAX_PATH * 2 + 4];
            swprintf(arg, ARRAYSIZE(arg), L"\"%ls\"", g_path);
            ShellExecuteW(g_hMain, NULL, L"notepad.exe", arg, NULL, SW_SHOWNORMAL);
        }
        break;
    case CMD_FOLDER:
        if (g_path[0]) {
            WCHAR arg[MAX_PATH * 2 + 16];
            swprintf(arg, ARRAYSIZE(arg), L"/select,\"%ls\"", g_path);
            ShellExecuteW(g_hMain, NULL, L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
        }
        break;
    case CMD_COPYPATH: if (g_path[0]) CopyToClipboard(g_path); break;
    case CMD_REGISTER:
        RegisterAssoc();
        if (MessageBoxW(g_hMain,
                        L"mdzy is now available in \"Open with\" for .md, .markdown and .txt files.\n\n"
                        L"Windows asks you to confirm the default app yourself. Open Default Apps settings now?",
                        APP_NAME, MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(g_hMain, L"open", L"ms-settings:defaultapps?registeredAppUser=mdzy", NULL, NULL, SW_SHOWNORMAL);
        break;
    case CMD_UNREGISTER:
        UnregisterAssoc();
        MessageBoxW(g_hMain, L"File associations removed.", APP_NAME, MB_ICONINFORMATION);
        break;
    case CMD_HELP:
        if (g_path[0]) { PushHist(g_back, &g_nBack, g_path); ClearHist(g_fwd, &g_nFwd); }
        ShowHelp();
        break;
    case CMD_COPY: SendMessageW(g_hEdit, WM_COPY, 0, 0); break;
    case CMD_SELECTALL: SendMessageW(g_hEdit, EM_SETSEL, 0, -1); break;
    }
}

static void ShowContextMenu(int x, int y) {
    CHARRANGE sel;
    SendMessageW(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&sel);
    HMENU m = CreatePopupMenu(), th = CreatePopupMenu(), as = CreatePopupMenu();
    UINT hasFile = g_path[0] ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(m, MF_STRING | hasFile, CMD_RELOAD, L"&Reload\tF5");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (sel.cpMax > sel.cpMin ? 0 : MF_GRAYED), CMD_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING, CMD_SELECTALL, L"Select &all\tCtrl+A");
    AppendMenuW(m, MF_STRING, CMD_FIND, L"&Find...\tCtrl+F");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_nBack ? 0 : MF_GRAYED), CMD_BACK, L"&Back\tAlt+Left");
    AppendMenuW(m, MF_STRING | (g_nFwd ? 0 : MF_GRAYED), CMD_FORWARD, L"Forw&ard\tAlt+Right");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_isMd ? 0 : MF_GRAYED) | (g_raw ? MF_CHECKED : 0), CMD_RAW, L"Show &source\tCtrl+U");
    AppendMenuW(m, MF_STRING | (IsMdView() ? MF_GRAYED : 0) | (g_wrap ? MF_CHECKED : 0), CMD_WRAP, L"&Word wrap\tAlt+Z");
    AppendMenuW(th, MF_STRING | (g_theme == 0 ? MF_CHECKED : 0), CMD_THEME_AUTO, L"&System");
    AppendMenuW(th, MF_STRING | (g_theme == 1 ? MF_CHECKED : 0), CMD_THEME_LIGHT, L"&Light");
    AppendMenuW(th, MF_STRING | (g_theme == 2 ? MF_CHECKED : 0), CMD_THEME_DARK, L"&Dark");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"&Theme\tCtrl+D");
    AppendMenuW(m, MF_STRING, CMD_ZOOMIN, L"Zoom &in\tCtrl++");
    AppendMenuW(m, MF_STRING, CMD_ZOOMOUT, L"Zoom ou&t\tCtrl+-");
    AppendMenuW(m, MF_STRING | (g_zoom == 100 ? MF_GRAYED : 0), CMD_ZOOMRESET, L"Reset &zoom\tCtrl+0");
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), CMD_TOPMOST, L"Always on to&p\tCtrl+T");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | hasFile, CMD_EDIT, L"Open in &editor\tCtrl+E");
    AppendMenuW(m, MF_STRING | hasFile, CMD_FOLDER, L"Show in fo&lder");
    AppendMenuW(m, MF_STRING | hasFile, CMD_COPYPATH, L"Copy pat&h\tCtrl+Shift+C");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(as, MF_STRING, CMD_REGISTER, L"&Register mdzy for .md / .txt...");
    AppendMenuW(as, MF_STRING, CMD_UNREGISTER, L"&Unregister");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)as, L"File a&ssociations");
    AppendMenuW(m, MF_STRING, CMD_HELP, L"&Help\tF1");
    AppendMenuW(m, MF_STRING, CMD_CLOSE, L"&Close\tEsc");
    if (x == -1 && y == -1) {
        RECT rc;
        GetWindowRect(g_hEdit, &rc);
        x = rc.left + 40;
        y = rc.top + 40;
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_hMain, NULL);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

static void OnDrop(HDROP hd) {
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    WCHAR p[MAX_PATH * 2], exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    for (UINT i = 0; i < n; i++) {
        if (!DragQueryFileW(hd, i, p, ARRAYSIZE(p))) continue;
        if (i == 0) {
            Navigate(p);
        } else {
            WCHAR arg[MAX_PATH * 2 + 4];
            swprintf(arg, ARRAYSIZE(arg), L"\"%ls\"", p);
            ShellExecuteW(NULL, NULL, exe, arg, NULL, SW_SHOWNORMAL);
        }
    }
    DragFinish(hd);
    SetForegroundWindow(g_hMain);
}

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETFOCUS: {
        LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp);
        HideCaret(h);
        return r;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp);
        HideCaret(h);
        return r;
    }
    case WM_KEYDOWN:
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) {
            int ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            switch (wp) {
            case VK_UP: SendMessageW(h, WM_VSCROLL, SB_LINEUP, 0); return 0;
            case VK_DOWN: SendMessageW(h, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
            case VK_PRIOR: SendMessageW(h, WM_VSCROLL, SB_PAGEUP, 0); return 0;
            case VK_NEXT:
            case VK_SPACE: SendMessageW(h, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
            case VK_HOME: if (!ctrl || 1) { SendMessageW(h, WM_VSCROLL, SB_TOP, 0); return 0; } break;
            case VK_END: SendMessageW(h, WM_VSCROLL, SB_BOTTOM, 0); return 0;
            case VK_LEFT: if (!ctrl) { SendMessageW(h, WM_HSCROLL, SB_LINELEFT, 0); return 0; } break;
            case VK_RIGHT: if (!ctrl) { SendMessageW(h, WM_HSCROLL, SB_LINERIGHT, 0); return 0; } break;
            }
        } else if (wp == VK_SPACE) {
            SendMessageW(h, WM_VSCROLL, SB_PAGEUP, 0);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp >= 32) return 0;   /* no beeps on typing in read-only mode */
        break;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            OnCommand(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? CMD_ZOOMIN : CMD_ZOOMOUT);
            return 0;
        }
        break;
    case WM_DROPFILES:
        OnDrop((HDROP)wp);
        return 0;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_findMsg && g_findMsg) {
        FINDREPLACEW *fr = (FINDREPLACEW *)lp;
        if (fr->Flags & FR_DIALOGTERM) g_hFind = NULL;
        else if (fr->Flags & FR_FINDNEXT) DoFind((fr->Flags & FR_DOWN) != 0);
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        g_hEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_NOHIDESEL |
                                  ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_SAVESEL,
                                  0, 0, 0, 0, h, (HMENU)1, g_hInst, NULL);
        if (!g_hEdit) return -1;
        SendMessageW(g_hEdit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
        SendMessageW(g_hEdit, EM_SETUNDOLIMIT, 0, 0);
        SendMessageW(g_hEdit, EM_SETEVENTMASK, 0, ENM_LINK);
        SendMessageW(g_hEdit, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
        RevokeDragDrop(g_hEdit);
        DragAcceptFiles(g_hEdit, TRUE);
        g_editProc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        return 0;
    }
    case WM_SIZE:
        MoveWindow(g_hEdit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        UpdateRect();
        if (IsMdView() && g_contentPx != g_layoutPx) SetTimer(h, TIMER_RELAYOUT, 120, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_RELAYOUT) {
            KillTimer(h, TIMER_RELAYOUT);
            if (IsMdView() && g_contentPx != g_layoutPx) Render(1);
        } else if (wp == TIMER_WATCH && g_path[0]) {
            FILETIME mt;
            ULONGLONG sz;
            if (StatFile(g_path, &mt, &sz) && (CompareFileTime(&mt, &g_mtime) || sz != g_fsize))
                LoadFile(g_path, 1, TRUE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFOCUS:
        SetFocus(g_hEdit);
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == g_hEdit && nm->code == EN_LINK) {
            ENLINK *el = (ENLINK *)lp;
            if (el->msg == WM_LBUTTONUP) {
                CHARRANGE sel;
                SendMessageW(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&sel);
                if (sel.cpMax > sel.cpMin) return 0;   /* the user was selecting text */
                WCHAR url[2100];
                if (GetLinkUrl(el->chrg, url, ARRAYSIZE(url))) {
#ifdef MDZY_DEV
                    if (g_devLog) { fwprintf(g_devLog, L"click: %ls\n", url); fflush(g_devLog); return 1; }
#endif
                    OpenLink(url);
                }
                return 1;
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        ShowContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_BROWSER_BACKWARD: GoHistory(-1); return TRUE;
        case APPCOMMAND_BROWSER_FORWARD: GoHistory(1); return TRUE;
        }
        break;
    case WM_DROPFILES:
        OnDrop((HDROP)wp);
        return 0;
    case WM_SETTINGCHANGE:
        if (g_theme == 0 && lp && !lstrcmpiW((const WCHAR *)lp, L"ImmersiveColorSet")) {
            int was = g_dark;
            ApplyTheme();
            if (was != g_dark) Render(1);
        }
        break;
    case WM_DESTROY:
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

#ifdef MDZY_DEV
static void Pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG m;
    while ((int)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}
static void DevShot(const WCHAR *out) {
    RECT r;
    DwmGetWindowAttribute(g_hMain, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, w, h);
    SelectObject(mdc, bm);
    BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY);
    BITMAPINFOHEADER bi = { sizeof bi, w, -h, 1, 24, BI_RGB };
    int stride = (w * 3 + 3) & ~3;
    char *px = (char *)malloc((size_t)stride * h);
    GetDIBits(mdc, bm, 0, (UINT)h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof bi + (size_t)stride * h), 0, 0, sizeof fh + sizeof bi };
    FILE *f = _wfopen(out, L"wb");
    if (f) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        fwrite(px, (size_t)stride * h, 1, f);
        fclose(f);
    }
    free(px);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}
static void DevLinkTest(void) {
    LONG len = GetWindowTextLengthW(g_hEdit);
    LONG runStart = -1;
    int clicked = 0;
    for (LONG cp = 0; cp <= len; cp++) {
        int isLink = 0;
        if (cp < len) {
            CHARFORMAT2W cf;
            memset(&cf, 0, sizeof cf);
            cf.cbSize = sizeof cf;
            SendMessageW(g_hEdit, EM_SETSEL, cp, cp + 1);
            SendMessageW(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
            isLink = (cf.dwEffects & CFE_LINK) != 0;
        }
        if (isLink && runStart < 0) runStart = cp;
        if (!isLink && runStart >= 0) {
            CHARRANGE cr = { runStart, cp };
            WCHAR url[2100];
            GetLinkUrl(cr, url, ARRAYSIZE(url));
            fwprintf(g_devLog, L"run %ld-%ld: %ls\n", runStart, cp, url);
            if (!clicked) {
                clicked = 1;
                SendMessageW(g_hEdit, EM_SETSEL, 0, 0);
                POINTL pt;
                SendMessageW(g_hEdit, EM_POSFROMCHAR, (WPARAM)&pt, cp - 1);
                LPARAM xy = MAKELPARAM(pt.x + 2, pt.y + 6);
                SendMessageW(g_hEdit, WM_LBUTTONDOWN, MK_LBUTTON, xy);
                SendMessageW(g_hEdit, WM_LBUTTONUP, 0, xy);
            }
            runStart = -1;
        }
    }
    fflush(g_devLog);
}
#endif

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow) {
    (void)hPrev;
    (void)cmdLine;
    g_hInst = hInst;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR *file = NULL;
#ifdef MDZY_DEV
    const WCHAR *shot = NULL, *linkLog = NULL, *bench = NULL;
    int shotW = 900, shotH = 900, scrollPages = 0, devTheme = -1;
#endif
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--register")) { RegisterAssoc(); return 0; }
        if (!_wcsicmp(argv[i], L"--unregister")) { UnregisterAssoc(); return 0; }
#ifdef MDZY_DEV
        if (!_wcsicmp(argv[i], L"--shot") && i + 1 < argc) { shot = argv[++i]; continue; }
        if (!_wcsicmp(argv[i], L"--size") && i + 1 < argc) { swscanf(argv[++i], L"%dx%d", &shotW, &shotH); continue; }
        if (!_wcsicmp(argv[i], L"--scroll") && i + 1 < argc) { scrollPages = _wtoi(argv[++i]); continue; }
        if (!_wcsicmp(argv[i], L"--dark")) { devTheme = 2; continue; }
        if (!_wcsicmp(argv[i], L"--light")) { devTheme = 1; continue; }
        if (!_wcsicmp(argv[i], L"--linktest") && i + 1 < argc) { linkLog = argv[++i]; continue; }
        if (!_wcsicmp(argv[i], L"--rtf") && i + 1 < argc) { g_devRtf = argv[++i]; continue; }
        if (!_wcsicmp(argv[i], L"--bench") && i + 1 < argc) { bench = argv[++i]; continue; }
#endif
        if (!file) file = argv[i];
    }

    OleInitialize(NULL);
    LoadLibraryW(L"msftedit.dll");
    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);

    InitIniPath();
    g_zoom = IniInt(L"zoom", 100);
    g_theme = IniInt(L"theme", 0);
    g_wrap = IniInt(L"wrap", 1);
    g_topmost = IniInt(L"topmost", 0);
#ifdef MDZY_DEV
    if (devTheme >= 0) g_theme = devTheme;
    if (shot) g_zoom = 100;
#endif

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    /* window placement: saved, validated against the current monitors, cascaded if another window is there */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = IniInt(L"w", MulDiv(900, g_dpi, 96)), hgt = IniInt(L"h", MulDiv(1000, g_dpi, 96));
    if (hgt > wa.bottom - wa.top) hgt = wa.bottom - wa.top;
    int x = IniInt(L"x", wa.left + (wa.right - wa.left - w) / 2), y = IniInt(L"y", wa.top + (wa.bottom - wa.top - hgt) / 2);
    RECT test = { x, y, x + w, y + 60 };
    if (!MonitorFromRect(&test, MONITOR_DEFAULTTONULL)) {
        x = wa.left + (wa.right - wa.left - w) / 2;
        y = wa.top + (wa.bottom - wa.top - hgt) / 2;
    }
    HWND other = FindWindowW(WND_CLASS, NULL);
    if (other) {
        RECT orc;
        GetWindowRect(other, &orc);
        if (abs(orc.left - x) < 16 && abs(orc.top - y) < 16) { x += MulDiv(28, g_dpi, 96); y += MulDiv(28, g_dpi, 96); }
    }
#ifdef MDZY_DEV
    if (shot) { x = 40; y = 40; w = shotW; hgt = shotH; }
#endif
    g_hMain = CreateWindowExW(WS_EX_ACCEPTFILES, WND_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW, x, y, w, hgt,
                              NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;
    g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    ApplyTheme();
    SendMessageW(g_hEdit, EM_SETZOOM, g_zoom == 100 ? 0 : (WPARAM)g_zoom, g_zoom == 100 ? 0 : 100);

    if (!file || !LoadFile(file, 0, FALSE)) ShowHelp();

    int max = IniInt(L"max", 0);
#ifdef MDZY_DEV
    if (shot) max = 0;
#endif
    ShowWindow(g_hMain, max ? SW_SHOWMAXIMIZED : nShow);
    if (g_topmost) SetWindowPos(g_hMain, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    UpdateWindow(g_hMain);
    SetTimer(g_hMain, TIMER_WATCH, 1000, NULL);

    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'O', CMD_OPEN }, { FVIRTKEY, VK_F5, CMD_RELOAD }, { FCONTROL | FVIRTKEY, 'R', CMD_RELOAD },
        { FCONTROL | FVIRTKEY, 'W', CMD_CLOSE }, { FVIRTKEY, VK_ESCAPE, CMD_CLOSE },
        { FCONTROL | FVIRTKEY, 'F', CMD_FIND }, { FVIRTKEY, VK_F3, CMD_FINDNEXT }, { FSHIFT | FVIRTKEY, VK_F3, CMD_FINDPREV },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS, CMD_ZOOMIN }, { FCONTROL | FVIRTKEY, VK_ADD, CMD_ZOOMIN },
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, CMD_ZOOMOUT }, { FCONTROL | FVIRTKEY, VK_SUBTRACT, CMD_ZOOMOUT },
        { FCONTROL | FVIRTKEY, '0', CMD_ZOOMRESET }, { FCONTROL | FVIRTKEY, VK_NUMPAD0, CMD_ZOOMRESET },
        { FCONTROL | FVIRTKEY, 'U', CMD_RAW }, { FCONTROL | FVIRTKEY, 'D', CMD_THEME }, { FALT | FVIRTKEY, 'Z', CMD_WRAP },
        { FCONTROL | FVIRTKEY, 'T', CMD_TOPMOST }, { FALT | FVIRTKEY, VK_LEFT, CMD_BACK }, { FVIRTKEY, VK_BACK, CMD_BACK },
        { FALT | FVIRTKEY, VK_RIGHT, CMD_FORWARD }, { FCONTROL | FVIRTKEY, 'E', CMD_EDIT }, { FVIRTKEY, VK_F1, CMD_HELP },
        { FCONTROL | FSHIFT | FVIRTKEY, 'C', CMD_COPYPATH },
    };
    g_hAccel = CreateAcceleratorTableW(acc, (int)ARRAYSIZE(acc));

#ifdef MDZY_DEV
    if (bench) {   /* time from process creation to first painted window */
        FILETIME cr, ex, k, u, now;
        GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u);
        GetSystemTimePreciseAsFileTime(&now);
        ULONGLONG a = ((ULONGLONG)cr.dwHighDateTime << 32) | cr.dwLowDateTime;
        ULONGLONG b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        FILE *f = _wfopen(bench, L"a");
        if (f) { fprintf(f, "%.1f ms\n", (double)(b - a) / 10000.0); fclose(f); }
        DestroyWindow(g_hMain);
        return 0;
    }
    if (shot || linkLog) {
        SetWindowPos(g_hMain, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(g_hMain);
        Pump(400);
        for (int i = 0; i < scrollPages; i++) SendMessageW(g_hEdit, WM_VSCROLL, SB_PAGEDOWN, 0);
        Pump(200);
        if (linkLog) {
            g_devLog = _wfopen(linkLog, L"w, ccs=UTF-8");
            DevLinkTest();
            Pump(200);
            fclose(g_devLog);
            g_devLog = NULL;
        }
        if (shot) DevShot(shot);
        DestroyWindow(g_hMain);
        return 0;
    }
#endif

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_hFind && IsDialogMessageW(g_hFind, &msg)) continue;
        if (TranslateAcceleratorW(g_hMain, g_hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return 0;
}
