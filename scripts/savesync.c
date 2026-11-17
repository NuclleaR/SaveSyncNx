/*
 * savesync.c — SaveSyncNx: Switch save backup to Koofr (Checkpoint script)
 *
 * Backs up the account and device saves of every game in Checkpoint's list to
 * Koofr over WebDAV, uploading a save only when it changed since its last
 * upload, and keeps every version in the cloud. Restore downloads any version
 * into Checkpoint's backup folder for that game; Checkpoint's own Restore then
 * writes it into the game. Nothing is ever written into a save by this script.
 *
 * Install: copy this file to sdmc:/switch/Checkpoint/scripts/universal/
 * under the name savesync.c (that is the file "Update script..." replaces).
 * Run: Checkpoint -> Minus -> Scripts -> savesync.
 *
 * On-SD layout (relative to app_root()):
 *   config/savesync.vault         Koofr email + app password, sealed (device_seal)
 *   config/savesync-profiles.txt  profile names, one per line
 *   config/savesync-state.txt     fingerprint of the last upload of each save
 *   savesync/                     temporary files of a run, emptied every run
 *
 * Cloud layout (under KOOFR_URL):
 *   SaveSyncNx/saves/0x<TITLEID> <name>/<YYYYMMDD-HHMMSS> <profile>.zip
 *   SaveSyncNx/device/0x<TITLEID> <name>/<YYYYMMDD-HHMMSS>.zip
 *   SaveSyncNx/.incoming/                 uploads in progress
 *
 * Design: docs/superpowers/specs/2026-11-12-savesyncnx-design.md
 *
 * Copyright (C) 2026 the SaveSyncNx authors.
 *
 * Parts of this file are adapted from Checkpoint's
 * scripts/common/universal/webdav.c (Base64, URL building, name sanitising,
 * WebDAV calls and the vault handling), Copyright (C) 2017-2026 Bernardo
 * Giordano, FlagBrew, GPLv3 or later with Additional Terms 7.b and 7.c.
 * Modified: rewritten for Koofr, a fixed server URL, MOVE-based uploads and
 * unbounded listings.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. See the LICENSE file.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define SAVESYNC_VERSION "v0.1.0"

#ifndef KOOFR_URL
#define KOOFR_URL "https://app.koofr.net/dav/Koofr"
#endif

#ifndef GITHUB_API
#define GITHUB_API "https://api.github.com/repos/NuclleaR/SaveSyncNx/releases/latest"
#endif

#ifndef GITHUB_RAW
#define GITHUB_RAW "https://raw.githubusercontent.com/NuclleaR/SaveSyncNx"
#endif

#define PATHN   512  /* an SD path                                             */
#define URLN    512  /* the base URL                                           */
#define USERN   128
#define PWN     128
#define AUTHN   512  /* "Authorization: Basic " + base64(user:password)        */
#define URLBUFN 1536 /* a built request URL: base + encoded segments           */
#define NAMEN   512  /* one decoded path segment (a game folder or a version)  */
#define HDRN    2560 /* the request header block: auth + Destination + extras  */
#define LINEN   1024 /* one line of a config file                              */

#define FATAL_NET   1
#define FATAL_LOGIN 2
#define FATAL_FULL  3

#define PROPFIND_BODY "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>"

/* ---- globals ------------------------------------------------------------ */

char g_root[PATHN]; /* app_root()        */
char g_cfg[PATHN];  /* <root>/config     */
char g_work[PATHN]; /* <root>/savesync   */

char g_base[URLN]; /* KOOFR_URL, no trailing '/' */
char g_user[USERN];
char g_pw[PWN];
char g_auth[AUTHN]; /* "Authorization: Basic ...", built by build_auth() */

int g_fatal = 0;  /* 0, FATAL_NET, FATAL_LOGIN or FATAL_FULL: why the run must stop */
int g_status = 0; /* the last response's status (negative: transfer error) */
char g_date[64];  /* the last response's Date header, "" when it had none */
char g_hbuf[HDRN];

struct names {
    int count;
    int cap;
    char** items;
};

/* ---- SD paths ----------------------------------------------------------- */

/* Delete everything inside the SD folder `dir` (not the folder itself).
 * read_directory gives full paths without telling files from folders, so a
 * failed remove() is what marks a folder: empty it, then rmdir it. */
void rm_tree(char* dir)
{
    struct directory* d = read_directory(dir);
    if (d == NULL) {
        return;
    }
    int i = 0;
    while (i < d->count) {
        if (remove(d->files[i]) != 0) {
            rm_tree(d->files[i]);
            rmdir(d->files[i]);
        }
        i = i + 1;
    }
    delete_directory(d);
}

void init_paths(void)
{
    char* root = app_root();
    strncpy(g_root, root, PATHN - 1);
    g_root[PATHN - 1] = '\0';
    free(root);
    sprintf(g_cfg, "%s/config", g_root);
    sprintf(g_work, "%s/savesync", g_root);
    strcpy(g_base, KOOFR_URL);
    g_date[0] = '\0';
    sd_mkdirs(g_cfg);
    sd_mkdirs(g_work);
    rm_tree(g_work);
}

/* ---- text --------------------------------------------------------------- */

void rstrip_slash(char* s)
{
    int n = strlen(s);
    if (n > 0 && s[n - 1] == '/') {
        s[n - 1] = '\0';
    }
}

/* The final path component of `full` (everything after the last '/'). */
void basename_of(char* full, char* dst, int size)
{
    int last = -1;
    int i    = 0;
    while (full[i] != '\0') {
        if (full[i] == '/') {
            last = i;
        }
        i = i + 1;
    }
    int j = 0;
    while (full[last + 1 + j] != '\0' && j < size - 1) {
        dst[j] = full[last + 1 + j];
        j      = j + 1;
    }
    dst[j] = '\0';
}

/* base64 of `src` into dst[size] (HTTP Basic). Never emits half a quad. */
void b64_encode(char* src, char* dst, int size)
{
    char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int n     = strlen(src);
    int i     = 0;
    int j     = 0;
    while (i < n && j + 4 < size) {
        int b0   = src[i] & 0xFF;
        int b1   = 0;
        int b2   = 0;
        int have = 1;
        if (i + 1 < n) {
            b1   = src[i + 1] & 0xFF;
            have = 2;
        }
        if (i + 2 < n) {
            b2   = src[i + 2] & 0xFF;
            have = 3;
        }
        dst[j]     = tbl[(b0 >> 2) & 0x3F];
        dst[j + 1] = tbl[((b0 << 4) | (b1 >> 4)) & 0x3F];
        if (have > 1) {
            dst[j + 2] = tbl[((b1 << 2) | (b2 >> 6)) & 0x3F];
        }
        else {
            dst[j + 2] = '=';
        }
        if (have > 2) {
            dst[j + 3] = tbl[b2 & 0x3F];
        }
        else {
            dst[j + 3] = '=';
        }
        j = j + 4;
        i = i + 3;
    }
    dst[j] = '\0';
}

int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Percent-decode `src` into dst[size]. A stray '%' is kept as it is. */
void pct_decode(char* src, char* dst, int size)
{
    int i = 0;
    int j = 0;
    while (src[i] != '\0' && j < size - 1) {
        int hi = -1;
        int lo = -1;
        if (src[i] == '%' && src[i + 1] != '\0') {
            hi = hexval(src[i + 1]);
            if (hi >= 0) {
                lo = hexval(src[i + 2]);
            }
        }
        if (hi >= 0 && lo >= 0) {
            dst[j] = hi * 16 + lo;
            i      = i + 3;
        }
        else {
            dst[j] = src[i];
            i      = i + 1;
        }
        j = j + 1;
    }
    dst[j] = '\0';
}

/* Does `s` hold `pat` at index `at`? */
int has_at(char* s, int at, char* pat)
{
    int k = 0;
    while (pat[k] != '\0') {
        if (s[at + k] != pat[k]) {
            return 0;
        }
        k = k + 1;
    }
    return 1;
}

/* XML-unescape `src` into dst[size]: the five named entities and &#N; / &#xH;
 * below 128. A server that leaves '&' unencoded in an href writes it as &amp;. */
void xml_unescape(char* src, char* dst, int size)
{
    int i = 0;
    int j = 0;
    while (src[i] != '\0' && j < size - 1) {
        int c    = src[i];
        int skip = 1;
        if (c == '&') {
            if (has_at(src, i, "&amp;")) {
                skip = 5;
            }
            else if (has_at(src, i, "&lt;")) {
                c    = '<';
                skip = 4;
            }
            else if (has_at(src, i, "&gt;")) {
                c    = '>';
                skip = 4;
            }
            else if (has_at(src, i, "&quot;")) {
                c    = '"';
                skip = 6;
            }
            else if (has_at(src, i, "&apos;")) {
                c    = '\'';
                skip = 6;
            }
            else if (src[i + 1] == '#') {
                int k    = i + 2;
                int base = 10;
                int v    = 0;
                if (src[k] == 'x' || src[k] == 'X') {
                    base = 16;
                    k    = k + 1;
                }
                while (hexval(src[k]) >= 0 && hexval(src[k]) < base) {
                    v = v * base + hexval(src[k]);
                    k = k + 1;
                }
                if (src[k] == ';' && k > i + 2 && v > 0 && v < 128) {
                    c    = v;
                    skip = k + 1 - i;
                }
            }
        }
        dst[j] = c;
        j      = j + 1;
        i      = i + skip;
    }
    dst[j] = '\0';
}

/* `src` as one safe path segment: / \ : * ? " < > |, control characters and
 * trailing dots become '_' (Windows drops a trailing dot, so a folder named
 * like that could never be matched again). "" becomes "untitled". */
void sanitize_name(char* src, char* dst, int size)
{
    int i = 0;
    int j = 0;
    while (src[i] != '\0' && j < size - 1) {
        int c = src[i] & 0xFF;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 32) {
            dst[j] = '_';
        }
        else {
            dst[j] = src[i];
        }
        i = i + 1;
        j = j + 1;
    }
    dst[j] = '\0';
    while (j > 0 && dst[j - 1] == '.') {
        dst[j - 1] = '_';
        j          = j - 1;
    }
    if (dst[0] == '\0') {
        strcpy(dst, "untitled");
    }
}

/* Append '/' and the percent-encoded `segment` (every byte but A-Z a-z 0-9
 * - . _ ~ encoded) to the URL in `dst`. 0, with `dst` untouched, if the result
 * would not fit `limit` bytes: a clipped URL would address the wrong file. */
int url_add(char* dst, char* segment, int limit)
{
    char* enc = url_encode(segment);
    int have  = strlen(dst);
    int ok    = 0;
    if (have + 1 + strlen(enc) + 1 <= limit) {
        dst[have]     = '/';
        dst[have + 1] = '\0';
        strcat(dst, enc);
        ok = 1;
    }
    free(enc);
    return ok;
}

/* The path part of an absolute URL ("/dav/Koofr/x" of "https://h/dav/Koofr/x")
 * into dst[size], or a copy of `url` when it is already a path. */
void url_path(char* url, char* dst, int size)
{
    int start = 0;
    if (has_at(url, 0, "http://") || has_at(url, 0, "https://")) {
        int slashes = 0;
        start       = -1;
        int i       = 0;
        while (url[i] != '\0' && start < 0) {
            if (url[i] == '/') {
                slashes = slashes + 1;
                if (slashes == 3) {
                    start = i;
                }
            }
            i = i + 1;
        }
        if (start < 0) {
            dst[0] = '\0';
            return;
        }
    }
    int j = 0;
    while (url[start + j] != '\0' && j < size - 1) {
        dst[j] = url[start + j];
        j      = j + 1;
    }
    dst[j] = '\0';
}

/* ---- name lists --------------------------------------------------------- */

void names_add(struct names* n, char* s)
{
    if (n->count == n->cap) {
        int cap = n->cap * 2;
        if (cap < 16) {
            cap = 16;
        }
        char** grown = (char**)malloc(cap * sizeof(char*));
        int i        = 0;
        while (i < n->count) {
            grown[i] = n->items[i];
            i        = i + 1;
        }
        if (n->items != NULL) {
            free(n->items);
        }
        n->items = grown;
        n->cap   = cap;
    }
    n->items[n->count] = strdup(s);
    n->count           = n->count + 1;
}

void names_free(struct names* n)
{
    int i = 0;
    while (i < n->count) {
        free(n->items[i]);
        i = i + 1;
    }
    if (n->items != NULL) {
        free(n->items);
    }
    n->items = NULL;
    n->count = 0;
    n->cap   = 0;
}

/* ---- WebDAV ------------------------------------------------------------- */

void build_auth(void)
{
    char pair[USERN + PWN + 2];
    char enc[AUTHN];
    sprintf(pair, "%s:%s", g_user, g_pw);
    b64_encode(pair, enc, AUTHN - 32);
    sprintf(g_auth, "Authorization: Basic %s", enc);
}

/* g_auth plus `extra` ("" for none) into g_hbuf. */
void build_headers(char* extra)
{
    if (extra[0] != '\0') {
        snprintf(g_hbuf, HDRN, "%s\n%s", g_auth, extra);
    }
    else {
        strcpy(g_hbuf, g_auth);
    }
}

/* What every answer leaves behind: its Date (HTTP/2 sends the key lowercase)
 * and, for the statuses that end a run, the reason in g_fatal. */
void note_response(int st, char* respHeaders)
{
    g_status  = st;
    g_date[0] = '\0';
    if (respHeaders != NULL) {
        char* v = http_header_value(respHeaders, "Date");
        if (v[0] == '\0') {
            free(v);
            v = http_header_value(respHeaders, "date");
        }
        strncpy(g_date, v, 63);
        g_date[63] = '\0';
        free(v);
        free(respHeaders);
    }
    if (st == 401 || st == 403) {
        g_fatal = FATAL_LOGIN;
    }
    else if (st == 507) {
        g_fatal = FATAL_FULL;
    }
    else if (st < 0) {
        g_fatal = FATAL_NET;
    }
}

/* One authenticated request. Returns the HTTP status or a negative transfer
 * error. With `out` NULL the body is dropped; otherwise the caller frees *out. */
int dav_call(char* method, char* url, char* headers, char* body, char** out, int* outSize)
{
    char* resp  = NULL;
    char* hdrs  = NULL;
    int n       = 0;
    build_headers(headers);
    int st = web_request(method, url, g_hbuf, body, strlen(body), &resp, &n, &hdrs);
    note_response(st, hdrs);
    if (out != NULL) {
        *out     = resp;
        *outSize = n;
    }
    else if (resp != NULL) {
        free(resp);
    }
    return st;
}

/* Find-or-create the folder at `url`: 1 when it exists afterwards. */
int dav_mkcol(char* url)
{
    int st = dav_call("MKCOL", url, "", "", NULL, NULL);
    if (st == 201 || st == 405) {
        return 1;
    }
    char line[64];
    sprintf(line, "MKCOL failed (%d)", st);
    script_log(line);
    return 0;
}

/* Children of the folder at `url`, appended to `out` as decoded leaf names
 * (no trailing '/', never "." or ".."), however many there are. 1 when the
 * listing worked.
 * The folder itself is in a Depth 1 answer too; it is recognised by its path,
 * not by its position. Scans with strstr: an interpreted per-byte loop over a
 * listing of hundreds of versions would be slow. */
int dav_list(char* url, struct names* out)
{
    char* body = NULL;
    int n      = 0;
    int st     = dav_call("PROPFIND", url, "Depth: 1\nContent-Type: application/xml; charset=utf-8", PROPFIND_BODY, &body, &n);
    if (st != 207 || body == NULL) {
        if (body != NULL) {
            free(body);
        }
        char line[64];
        sprintf(line, "PROPFIND failed (%d)", st);
        script_log(line);
        return 0;
    }

    char raw[URLBUFN];
    char tmp[URLBUFN];
    char self[URLBUFN];
    char leaf[NAMEN];
    url_path(url, raw, URLBUFN);
    pct_decode(raw, self, URLBUFN);
    rstrip_slash(self);

    char* open  = "<D:href>";
    char* close = "</D:href>";
    if (strstr(body, open) == NULL) {
        open  = "<d:href>";
        close = "</d:href>";
    }
    int skip = strlen(open);
    char* p  = strstr(body, open);
    while (p != NULL) {
        int k = 0;
        while (p[skip + k] != '\0' && p[skip + k] != '<' && k < URLBUFN - 1) {
            tmp[k] = p[skip + k];
            k      = k + 1;
        }
        tmp[k] = '\0';
        xml_unescape(tmp, raw, URLBUFN);
        url_path(raw, tmp, URLBUFN);
        pct_decode(tmp, raw, URLBUFN);
        rstrip_slash(raw);
        if (strcmp(raw, self) != 0) {
            basename_of(raw, leaf, NAMEN);
            /* "." and ".." are never a child: a URL built from one names the
             * folder itself or its parent, which the cleanup would delete */
            if (leaf[0] != '\0' && strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0) {
                names_add(out, leaf);
            }
        }
        p = strstr(p, close);
        if (p != NULL) {
            p = strstr(p, open);
        }
    }
    free(body);
    return 1;
}

/* Stream the SD file `path` to `url` with PUT. Returns the status. */
int dav_put_file(char* path, char* url)
{
    char* resp = NULL;
    char* hdrs = NULL;
    int n      = 0;
    build_headers("Content-Type: application/octet-stream");
    int st = web_upload_file("PUT", url, g_hbuf, path, &resp, &n, &hdrs);
    if (resp != NULL) {
        free(resp);
    }
    if (st == -3) {
        /* the local file could not be read: not a network failure */
        if (hdrs != NULL) {
            free(hdrs);
        }
        script_log("upload: cannot read the local file");
        return st;
    }
    note_response(st, hdrs);
    return st;
}

/* MOVE without overwriting: 201, or 412 when `toUrl` is taken. */
int dav_move(char* fromUrl, char* toUrl)
{
    char extra[URLBUFN + 64];
    snprintf(extra, URLBUFN + 64, "Destination: %s\nOverwrite: F", toUrl);
    return dav_call("MOVE", fromUrl, extra, "", NULL, NULL);
}

int dav_delete(char* url)
{
    return dav_call("DELETE", url, "", "", NULL, NULL);
}

/* GET `url` into the SD file `path`. 1 when the whole body was written. */
int dav_get_file(char* url, char* path)
{
    char* body = NULL;
    int n      = 0;
    int st     = dav_call("GET", url, "", "", &body, &n);
    if (st != 200 || body == NULL) {
        if (body != NULL) {
            free(body);
        }
        char line[64];
        sprintf(line, "download failed (%d)", st);
        script_log(line);
        return 0;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        free(body);
        script_log("download: cannot write the SD file");
        return 0;
    }
    int wrote = 0;
    if (n > 0) {
        wrote = fwrite(body, 1, n, f);
    }
    if (fclose(f) != 0) {
        wrote = -1; /* the final flush failed: the file may be short */
    }
    free(body);
    if (wrote != n) {
        remove(path);
        script_log("download: the SD file was written short");
        return 0;
    }
    return 1;
}

/* The size of the file at `url` as Koofr reports it (PROPFIND Depth 0,
 * getcontentlength), or -1 when it cannot be had. */
int dav_size(char* url)
{
    char* body = NULL;
    int n      = 0;
    int st     = dav_call("PROPFIND", url, "Depth: 0\nContent-Type: application/xml; charset=utf-8", PROPFIND_BODY, &body, &n);
    if (st != 207 || body == NULL) {
        if (body != NULL) {
            free(body);
        }
        char line[64];
        sprintf(line, "PROPFIND failed (%d)", st);
        script_log(line);
        return -1;
    }
    char* tag = "getcontentlength>";
    int skip  = strlen(tag);
    char* p   = strstr(body, tag);
    int size  = -1;
    if (p != NULL && p[skip] >= '0' && p[skip] <= '9') {
        size  = 0;
        int k = 0;
        while (p[skip + k] >= '0' && p[skip + k] <= '9') {
            size = size * 10 + (p[skip + k] - '0');
            k    = k + 1;
        }
    }
    free(body);
    return size;
}

/* The size of the SD file `path`, or -1 when it cannot be opened. */
int sd_size(char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    int n = ftell(f);
    fclose(f);
    return n;
}

/* ---- change detection --------------------------------------------------- */

#define ZTAILN 65557 /* the end-of-central-directory record (22) + the longest zip comment */

unsigned int g_crctab[256];
int g_crcready = 0;

void crc_init(void)
{
    unsigned int n = 0;
    while (n < 256) {
        unsigned int c = n;
        int k          = 0;
        while (k < 8) {
            if (c & 1) {
                c = 0xEDB88320 ^ (c >> 1);
            }
            else {
                c = c >> 1;
            }
            k = k + 1;
        }
        g_crctab[n] = c;
        n           = n + 1;
    }
    g_crcready = 1;
}

/* CRC-32 (zip's), chainable: crc32_buf(b, nb, crc32_buf(a, na, 0)) is the
 * CRC of a followed by b. */
unsigned int crc32_buf(char* data, int len, unsigned int crc)
{
    if (!g_crcready) {
        crc_init();
    }
    unsigned int c = crc ^ 0xFFFFFFFF;
    int i          = 0;
    while (i < len) {
        c = g_crctab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        i = i + 1;
    }
    return c ^ 0xFFFFFFFF;
}

unsigned int le16(char* b, int at)
{
    return (b[at] & 0xFF) | ((b[at + 1] & 0xFF) << 8);
}

unsigned int le32(char* b, int at)
{
    unsigned int hi = b[at + 3] & 0xFF;
    return (b[at] & 0xFF) | ((b[at + 1] & 0xFF) << 8) | ((b[at + 2] & 0xFF) << 16) | (hi << 24);
}

/* `size` bytes of `f` from `offset`, malloc'd, or NULL if they are not all there. */
char* read_at(FILE* f, int offset, int size)
{
    char* buf = (char*)malloc(size + 1);
    fseek(f, offset, SEEK_SET);
    if (fread(buf, 1, size, f) != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

void names_init(struct names* n)
{
    n->count = 0;
    n->cap   = 0;
    n->items = NULL;
}

/* Sort the strings of `n` by strcmp (shell sort: no recursion, no extra memory). */
void sort_names(struct names* n)
{
    int gap = n->count / 2;
    while (gap > 0) {
        int i = gap;
        while (i < n->count) {
            char* tmp = n->items[i];
            int j     = i;
            while (j >= gap && strcmp(n->items[j - gap], tmp) > 0) {
                n->items[j] = n->items[j - gap];
                j           = j - gap;
            }
            n->items[j] = tmp;
            i           = i + 1;
        }
        gap = gap / 2;
    }
}

/* What a save is, for change detection: from the zip's central directory, one
 * "<crc32> <size> <path>" line per entry, sorted, so order and timestamps do
 * not count. out = "<entries>:<total bytes>:<crc32 of the sorted lines>".
 * Only the tail and the directory of the zip are read. 1 OK, 0 not a zip. */
int zip_fingerprint(char* zipPath, char* out, int outSize)
{
    out[0]  = '\0';
    FILE* f = fopen(zipPath, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    int size = ftell(f);
    int tail = size;
    if (tail > ZTAILN) {
        tail = ZTAILN;
    }
    char* t = NULL;
    if (tail >= 22) {
        t = read_at(f, size - tail, tail);
    }
    if (t == NULL) {
        fclose(f);
        return 0;
    }
    /* zip_dir writes no comment, so this is normally found at once */
    int at = tail - 22;
    while (at >= 0 && !(t[at] == 'P' && t[at + 1] == 'K' && t[at + 2] == 5 && t[at + 3] == 6)) {
        at = at - 1;
    }
    if (at < 0) {
        free(t);
        fclose(f);
        return 0;
    }
    int count  = le16(t, at + 10);
    int cdSize = le32(t, at + 12);
    int cdOff  = le32(t, at + 16);
    free(t);
    char* cd = NULL;
    if (cdSize > 0 && cdOff >= 0 && cdOff + cdSize <= size) {
        cd = read_at(f, cdOff, cdSize);
    }
    fclose(f);
    if (count > 0 && cd == NULL) {
        return 0;
    }

    struct names lines;
    names_init(&lines);
    char line[NAMEN + 32];
    char name[NAMEN];
    unsigned int total = 0;
    int pos            = 0;
    int ok             = 1;
    int i              = 0;
    while (i < count && ok) {
        if (pos + 46 > cdSize || !(cd[pos] == 'P' && cd[pos + 1] == 'K' && cd[pos + 2] == 1 && cd[pos + 3] == 2)) {
            ok = 0;
        }
        else {
            unsigned int crc   = le32(cd, pos + 16);
            unsigned int usize = le32(cd, pos + 24);
            int nlen           = le16(cd, pos + 28);
            int skip           = 46 + nlen + le16(cd, pos + 30) + le16(cd, pos + 32);
            if (pos + 46 + nlen > cdSize || nlen >= NAMEN) {
                ok = 0;
            }
            else {
                int k = 0;
                while (k < nlen) {
                    name[k] = cd[pos + 46 + k];
                    k       = k + 1;
                }
                name[nlen] = '\0';
                sprintf(line, "%08x %u %s\n", crc, usize, name);
                names_add(&lines, line);
                total = total + usize;
                pos   = pos + skip;
            }
        }
        i = i + 1;
    }
    if (cd != NULL) {
        free(cd);
    }
    if (ok) {
        sort_names(&lines);
        unsigned int c = 0;
        i              = 0;
        while (i < lines.count) {
            c = crc32_buf(lines.items[i], strlen(lines.items[i]), c);
            i = i + 1;
        }
        snprintf(out, outSize, "%d:%u:%08x", count, total, c);
    }
    names_free(&lines);
    return ok;
}

/* ---- state: the fingerprint of each save's last upload ------------------ */

struct names g_skeys;
struct names g_svals;
int g_state_ready = 0;

void state_paths(char* path, char* tmp)
{
    sprintf(path, "%s/savesync-state.txt", g_cfg);
    sprintf(tmp, "%s/savesync-state.txt.tmp", g_cfg);
}

/* Read config/savesync-state.txt ("<key>\t<fingerprint>" lines; malformed
 * ones are skipped). Returns how many entries were read. */
int state_load(void)
{
    char path[PATHN];
    char tmp[PATHN];
    char line[LINEN];
    if (g_state_ready) {
        names_free(&g_skeys);
        names_free(&g_svals);
    }
    names_init(&g_skeys);
    names_init(&g_svals);
    g_state_ready = 1;
    state_paths(path, tmp);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        /* a state_set cut off between its remove and its rename */
        f = fopen(tmp, "r");
        if (f == NULL) {
            return 0;
        }
    }
    while (fgets(line, LINEN, f) != NULL) {
        chomp(line);
        int tab = -1;
        int i   = 0;
        while (line[i] != '\0' && tab < 0) {
            if (line[i] == '\t') {
                tab = i;
            }
            i = i + 1;
        }
        if (tab > 0 && line[tab + 1] != '\0') {
            char val[LINEN];
            int k = 0;
            while (line[tab + 1 + k] != '\0') {
                val[k] = line[tab + 1 + k];
                k      = k + 1;
            }
            val[k]    = '\0';
            line[tab] = '\0';
            names_add(&g_skeys, line);
            names_add(&g_svals, val);
        }
    }
    fclose(f);
    return g_skeys.count;
}

/* The stored fingerprint for `key` (borrowed), or NULL. */
char* state_get(char* key)
{
    int i = 0;
    while (g_state_ready && i < g_skeys.count) {
        if (strcmp(g_skeys.items[i], key) == 0) {
            return g_svals.items[i];
        }
        i = i + 1;
    }
    return NULL;
}

/* Store `fp` for `key` and rewrite the state file at once, through a .tmp
 * file and a rename, so an abort never leaves half a file. 1 on success. */
int state_set(char* key, char* fp)
{
    char path[PATHN];
    char tmp[PATHN];
    if (!g_state_ready) {
        state_load();
    }
    int i     = 0;
    int found = 0;
    while (i < g_skeys.count && !found) {
        if (strcmp(g_skeys.items[i], key) == 0) {
            free(g_svals.items[i]);
            g_svals.items[i] = strdup(fp);
            found            = 1;
        }
        i = i + 1;
    }
    if (!found) {
        names_add(&g_skeys, key);
        names_add(&g_svals, fp);
    }
    state_paths(path, tmp);
    FILE* f = fopen(tmp, "w");
    if (f == NULL) {
        script_log("state: cannot write savesync-state.txt.tmp");
        return 0;
    }
    i = 0;
    while (i < g_skeys.count) {
        fputs(g_skeys.items[i], f);
        fputs("\t", f);
        fputs(g_svals.items[i], f);
        fputs("\n", f);
        i = i + 1;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        script_log("state: savesync-state.txt.tmp was written short");
        return 0;
    }
    /* The Switch's FAT rename does not replace an existing file. */
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            script_log("state: cannot rename savesync-state.txt.tmp");
            return 0;
        }
    }
    return 1;
}

/* ---- cloud layout and uploads ------------------------------------------- */

#define MAX_COPIES 999 /* "<version> (n).zip": far beyond any real clash */

char g_stamp[20]; /* YYYYMMDD-HHMMSS (UTC) for this run's version names */
char g_url_root[URLN];
char g_url_saves[URLN];
char g_url_device[URLN];
char g_url_incoming[URLN];

/* Up to `maxDigits` decimal digits of `s` from `at` into *out. Returns the
 * index after them, or -1 if there was no digit. */
int read_int(char* s, int at, int maxDigits, int* out)
{
    int v = 0;
    int k = 0;
    while (k < maxDigits && s[at + k] >= '0' && s[at + k] <= '9') {
        v = v * 10 + (s[at + k] - '0');
        k = k + 1;
    }
    if (k == 0) {
        return -1;
    }
    *out = v;
    return at + k;
}

/* "Thu, 12 Nov 2026 18:45:01 GMT" (an HTTP Date) -> "20261112-184501".
 * 0, with out = "", if `date` is not in that form. */
int stamp_from_date(char* date, char* out)
{
    char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int day      = 0;
    int mon      = 0;
    int year     = 0;
    int hh       = 0;
    int mm       = 0;
    int ss       = 0;
    out[0]       = '\0';
    int i        = 0;
    while (date[i] != '\0' && date[i] != ',') {
        i = i + 1;
    }
    if (date[i] != ',') {
        return 0;
    }
    i = i + 1;
    while (date[i] == ' ') {
        i = i + 1;
    }
    i = read_int(date, i, 2, &day);
    if (i < 0 || date[i] != ' ') {
        return 0;
    }
    i     = i + 1;
    int m = 0;
    while (m < 12 && mon == 0) {
        if (date[i] == months[m * 3] && date[i + 1] == months[m * 3 + 1] && date[i + 2] == months[m * 3 + 2]) {
            mon = m + 1;
        }
        m = m + 1;
    }
    if (mon == 0 || date[i + 3] != ' ') {
        return 0;
    }
    i = read_int(date, i + 4, 4, &year);
    if (i < 0 || date[i] != ' ') {
        return 0;
    }
    i = read_int(date, i + 1, 2, &hh);
    if (i < 0 || date[i] != ':') {
        return 0;
    }
    i = read_int(date, i + 1, 2, &mm);
    if (i < 0 || date[i] != ':') {
        return 0;
    }
    i = read_int(date, i + 1, 2, &ss);
    if (i < 0 || day < 1 || day > 31 || year < 2000 || hh > 23 || mm > 59 || ss > 60) {
        return 0;
    }
    sprintf(out, "%04d%02d%02d-%02d%02d%02d", year, mon, day, hh, mm, ss);
    return 1;
}

/* The URLs of SaveSyncNx/ and its saves/, device/ and .incoming/. */
void cloud_urls(void)
{
    strcpy(g_url_root, g_base);
    url_add(g_url_root, "SaveSyncNx", URLN);
    strcpy(g_url_saves, g_url_root);
    url_add(g_url_saves, "saves", URLN);
    strcpy(g_url_device, g_url_root);
    url_add(g_url_device, "device", URLN);
    strcpy(g_url_incoming, g_url_root);
    url_add(g_url_incoming, ".incoming", URLN);
}

/* Make sure SaveSyncNx/, saves/, device/ and .incoming/ exist, and take this
 * run's version time from Koofr's Date (the console clock only if there is
 * none). 1 when the folders are there. */
int cloud_prepare(void)
{
    cloud_urls();
    if (!dav_mkcol(g_url_root) || !dav_mkcol(g_url_saves) || !dav_mkcol(g_url_device) || !dav_mkcol(g_url_incoming)) {
        return 0;
    }
    if (!stamp_from_date(g_date, g_stamp)) {
        int now      = time(NULL);
        struct tm* t = gmtime(&now);
        strftime(g_stamp, 20, "%Y%m%d-%H%M%S", t);
        script_log("version time: console clock (Koofr sent no Date)");
    }
    return 1;
}

/* Delete whatever an earlier run left in .incoming/: the only deletion this
 * script makes, and only of its own unfinished uploads. Failures are logged. */
void cloud_cleanup_incoming(void)
{
    struct names n;
    char u[URLBUFN];
    char line[96];
    names_init(&n);
    if (!dav_list(g_url_incoming, &n)) {
        script_log("cleanup: cannot list .incoming");
        return;
    }
    int i = 0;
    while (i < n.count) {
        strcpy(u, g_url_incoming);
        if (url_add(u, n.items[i], URLBUFN)) {
            int st = dav_delete(u);
            if (st != 200 && st != 204 && st != 404) {
                sprintf(line, "cleanup: DELETE failed (%d)", st);
                script_log(line);
            }
        }
        i = i + 1;
    }
    names_free(&n);
}

/* Upload the zip as a new version that never replaces another: PUT to
 * .incoming/<incomingName>, check that Koofr holds all of it, MKCOL the game
 * folder, then MOVE (Overwrite: F)
 * to "<versionBase>.zip", "<versionBase> (2).zip", ... until a name is free.
 * The MOVE is the completion point. outFinal gets the file name. 1 OK. */
int upload_version(char* zipPath, char* kindDir, char* gameFolder, char* versionBase, char* incomingName, char* outFinal)
{
    char inUrl[URLBUFN];
    char game[URLBUFN];
    char dst[URLBUFN];
    char name[NAMEN];
    char line[96];
    outFinal[0] = '\0';
    strcpy(inUrl, g_url_incoming);
    strcpy(game, g_url_root);
    if (!url_add(inUrl, incomingName, URLBUFN) || !url_add(game, kindDir, URLBUFN) || !url_add(game, gameFolder, URLBUFN)) {
        script_log("upload: the URL is too long");
        return 0;
    }
    int st = dav_put_file(zipPath, inUrl);
    if (st != 200 && st != 201 && st != 204) {
        sprintf(line, "upload: PUT failed (%d)", st);
        script_log(line);
        return 0;
    }
    /* Koofr can answer a PUT 201 and keep fewer bytes (3DSaveSync saw it
     * serve *.bin files back empty): only a file of the full size is moved
     * into place; any other is deleted and the save counts as failed. */
    int sent   = sd_size(zipPath);
    int stored = dav_size(inUrl);
    if (stored != sent || sent < 0) {
        sprintf(line, "upload: Koofr holds %d of %d bytes", stored, sent);
        script_log(line);
        dav_delete(inUrl);
        return 0;
    }
    if (!dav_mkcol(game)) {
        return 0;
    }
    int copy = 1;
    while (copy <= MAX_COPIES) {
        if (copy == 1) {
            snprintf(name, NAMEN, "%s.zip", versionBase);
        }
        else {
            snprintf(name, NAMEN, "%s (%d).zip", versionBase, copy);
        }
        strcpy(dst, game);
        if (!url_add(dst, name, URLBUFN)) {
            script_log("upload: the URL is too long");
            return 0;
        }
        st = dav_move(inUrl, dst);
        if (st == 201 || st == 204) {
            strcpy(outFinal, name);
            return 1;
        }
        if (st != 412) {
            sprintf(line, "upload: MOVE failed (%d)", st);
            script_log(line);
            return 0;
        }
        copy = copy + 1;
    }
    return 0;
}

/* ---- vault and Settings ------------------------------------------------- */

#define VAULTN 640 /* the vault's JSON: both fields escaped at worst, plus keys */

/* Append `raw`, then `value` JSON-escaped, to `dst`. 0 instead of truncating
 * when the result would not fit `limit`: a clipped password would be sealed
 * and only fail later at the server. */
int json_append(char* dst, char* raw, char* value, int limit)
{
    int j = strlen(dst);
    int i = 0;
    while (raw[i] != '\0') {
        if (j + 1 >= limit) {
            return 0;
        }
        dst[j] = raw[i];
        j      = j + 1;
        i      = i + 1;
    }
    i = 0;
    while (value[i] != '\0') {
        int c = value[i] & 0xFF;
        if (j + 7 >= limit) {
            return 0;
        }
        if (c == '\\' || c == '"') {
            dst[j]     = '\\';
            dst[j + 1] = value[i];
            j          = j + 2;
        }
        else if (c < 32) {
            sprintf(g_hbuf, "\\u%04x", c);
            dst[j] = '\0';
            strcat(dst, g_hbuf);
            j = j + 6;
        }
        else {
            dst[j] = value[i];
            j      = j + 1;
        }
        i = i + 1;
    }
    dst[j] = '\0';
    return 1;
}

/* The string `key` of the JSON object `o` into dest[size], "" if absent. */
void jget(struct JSON* o, char* key, char* dest, int size)
{
    dest[0] = '\0';
    if (!json_is_object(o) || !json_object_contains(o, key)) {
        return;
    }
    struct JSON* e = json_object_element(o, key);
    if (!json_is_string(e)) {
        return;
    }
    char* s = json_get_string(e);
    strncpy(dest, s, size - 1);
    dest[size - 1] = '\0';
    free(s);
}

void vault_path(char* dst)
{
    sprintf(dst, "%s/savesync.vault", g_cfg);
}

/* Seal g_user / g_pw (console key, no passphrase) into config/savesync.vault.
 * 1 on success. */
int vault_write(void)
{
    char json[VAULTN];
    char path[PATHN];
    strcpy(json, "{");
    int ok = json_append(json, "\"user\":\"", g_user, VAULTN);
    ok     = ok && json_append(json, "\",\"password\":\"", g_pw, VAULTN);
    ok     = ok && json_append(json, "\"}", "", VAULTN);
    if (!ok) {
        gui_message("That email or app password is too long to store.");
        return 0;
    }
    char* blob = NULL;
    int bn     = 0;
    int rc     = device_seal(json, strlen(json), "", &blob, &bn);
    memset(json, 0, VAULTN);
    if (rc != 0) {
        sprintf(path, "Could not seal the Koofr login (%d).", rc);
        gui_message(path);
        return 0;
    }
    vault_path(path);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        free(blob);
        gui_message("Could not write config/savesync.vault.");
        return 0;
    }
    int wrote = fwrite(blob, 1, bn, f);
    if (fclose(f) != 0) {
        wrote = -1; /* the final flush failed: the vault may be short */
    }
    free(blob);
    if (wrote != bn) {
        remove(path);
        gui_message("config/savesync.vault was written short.\nThe SD card may be full.");
        return 0;
    }
    script_log("Koofr login sealed");
    return 1;
}

/* Open config/savesync.vault into g_user / g_pw and build the auth header.
 * 1 = loaded, 0 = no vault yet, -1 = damaged (the user has been told). */
int vault_read(void)
{
    char path[PATHN];
    vault_path(path);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    int n = ftell(f);
    rewind(f);
    char* raw = NULL;
    int got   = 0;
    if (n > 0) {
        raw = (char*)malloc(n);
        got = fread(raw, 1, n, f);
    }
    fclose(f);

    char* json = NULL;
    int jn     = 0;
    int rc     = -1;
    if (raw != NULL && got == n) {
        rc = device_unseal(raw, n, "", &json, &jn);
    }
    if (raw != NULL) {
        free(raw);
    }
    g_user[0] = '\0';
    g_pw[0]   = '\0';
    if (rc == 0) {
        struct JSON* v = json_new();
        json_parse(v, json);
        memset(json, 0, jn);
        free(json);
        if (json_is_valid(v)) {
            jget(v, "user", g_user, USERN);
            jget(v, "password", g_pw, PWN);
        }
        json_delete(v);
    }
    else if (json != NULL) {
        free(json);
    }
    if (g_user[0] == '\0' || g_pw[0] == '\0') {
        script_log("config/savesync.vault could not be opened");
        gui_message("The saved Koofr login is damaged.\nEnter it again in Settings.");
        return -1;
    }
    build_auth();
    return 1;
}

void show_status(void)
{
    char line[USERN + 16];
    sprintf(line, "Koofr: %s", g_user);
    gui_status(line);
}

/* PROPFIND the base URL and say in words what happened. 1 on 207. A test is
 * a diagnosis, not a run: it leaves g_fatal as it found it. */
int dav_test(void)
{
    char msg[256];
    int fatal = g_fatal;
    int st    = dav_call("PROPFIND", g_base, "Depth: 0\nContent-Type: application/xml; charset=utf-8", PROPFIND_BODY, NULL, NULL);
    g_fatal   = fatal;
    if (st == 207) {
        sprintf(msg, "Connected to Koofr as %s.", g_user);
    }
    else if (st == 401) {
        strcpy(msg, "Koofr rejected the email or app password (401).\nUse an app password from Koofr's settings,\nnot the account password.");
    }
    else if (st == 404 || st == 405) {
        sprintf(msg, "Koofr has no WebDAV folder at that address (%d).", st);
    }
    else if (st < 0) {
        sprintf(msg, "No connection to Koofr (transfer error %d).\nCheck Wi-Fi.", st);
    }
    else {
        sprintf(msg, "Koofr answered HTTP %d.", st);
    }
    gui_message(msg);
    return st == 207;
}

/* Email, then app password, then seal and test. 0 when either keyboard was
 * cancelled: the old vault (if any) is left as it was. */
int settings_change(void)
{
    char user[USERN];
    char pw[PWN];
    gui_keyboard(user, "Koofr email", USERN);
    if (user[0] == '\0') {
        return 0;
    }
    gui_keyboard(pw, "Koofr app password", PWN);
    if (pw[0] == '\0') {
        return 0;
    }
    strcpy(g_user, user);
    strcpy(g_pw, pw);
    memset(pw, 0, PWN);
    build_auth();
    if (!vault_write()) {
        return 0;
    }
    show_status();
    dav_test();
    return 1;
}

void settings_menu(void)
{
    char* items[2];
    items[0] = "Change email and app password";
    items[1] = "Test connection";
    int pick = gui_pick_one("Settings", items, 2);
    if (pick == 0) {
        settings_change();
    }
    else if (pick == 1) {
        dav_test();
    }
}

/* ---- first dialog ------------------------------------------------------- */

#define ACT_QUIT     0
#define ACT_BACKUP   1
#define ACT_RESTORE  2
#define ACT_SETTINGS 3
#define ACT_UPDATE   4

#define PROFN    32 /* the profile keyboard: room past the 24-character limit, so a long name is refused, not clipped */
#define PROF_MAX 24

void profiles_path(char* dst)
{
    sprintf(dst, "%s/savesync-profiles.txt", g_cfg);
}

/* Strip a trailing "\n" / "\r\n" from a line read with fgets. */
void chomp(char* s)
{
    int n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n        = n - 1;
    }
}

/* Append the saved profile names, in creation order, to `out`. Returns how many. */
int profiles_load(struct names* out)
{
    char path[PATHN];
    char line[LINEN];
    profiles_path(path);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    int count = 0;
    while (fgets(line, LINEN, f) != NULL) {
        chomp(line);
        if (line[0] != '\0') {
            names_add(out, line);
            count = count + 1;
        }
    }
    fclose(f);
    return count;
}

/* Rewrite the profiles file from `in`. 1 on success, 0 when the file could
 * not be opened or its final flush failed. */
int profiles_save(struct names* in)
{
    char path[PATHN];
    profiles_path(path);
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return 0;
    }
    int i = 0;
    while (i < in->count) {
        fputs(in->items[i], f);
        fputs("\n", f);
        i = i + 1;
    }
    return fclose(f) == 0;
}

int lower_ascii(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 'a';
    }
    return c;
}

int same_ignoring_case(char* a, char* b)
{
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return 0;
        }
        i = i + 1;
    }
    return a[i] == b[i];
}

/* 1 valid, 0 bad characters or length, -1 equal to an existing name ignoring case. */
int profile_name_check(char* name, struct names* existing)
{
    int n = strlen(name);
    if (n < 1 || n > PROF_MAX) {
        return 0;
    }
    int i = 0;
    while (i < n) {
        int c = name[i] & 0xFF;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) {
            return 0;
        }
        i = i + 1;
    }
    i = 0;
    while (i < existing->count) {
        if (same_ignoring_case(name, existing->items[i])) {
            return -1;
        }
        i = i + 1;
    }
    return 1;
}

/* Keyboard for a new profile name until it is valid; it is then saved after
 * the others and copied to `out`. 0 if the keyboard was cancelled (Checkpoint
 * answers an empty keyboard as cancelled, so "" never reaches the check). */
int ask_new_name(char* out, struct names* names)
{
    char buf[PROFN];
    while (1) {
        gui_keyboard(buf, "Profile name (A-Z, 0-9, space, - or _)", PROFN);
        if (buf[0] == '\0') {
            return 0;
        }
        int ok = profile_name_check(buf, names);
        if (ok == 1) {
            names_add(names, buf);
            if (!profiles_save(names)) {
                script_log("the profile names could not be saved");
            }
            strcpy(out, buf);
            return 1;
        }
        if (ok == -1) {
            gui_message("That name is already in the list.");
        }
        else {
            gui_message("Use 1-24 Latin letters, digits, spaces, - or _.");
        }
    }
    return 0;
}

/* The one dialog of a normal run: pick your name and the backup starts. */
int first_dialog(char* outProfile)
{
    struct names names;
    names.count = 0;
    names.cap   = 0;
    names.items = NULL;
    outProfile[0] = '\0';
    profiles_load(&names);

    int act = ACT_QUIT;
    if (names.count == 0) {
        if (ask_new_name(outProfile, &names)) {
            act = ACT_BACKUP;
        }
        names_free(&names);
        return act;
    }

    char prompt[64];
    sprintf(prompt, "SaveSync %s", SAVESYNC_VERSION);
    int done = 0;
    while (!done) {
        int count    = names.count;
        char** items = (char**)malloc((count + 4) * sizeof(char*));
        int i        = 0;
        while (i < count) {
            items[i] = names.items[i];
            i        = i + 1;
        }
        items[count]     = "New name...";
        items[count + 1] = "Restore...";
        items[count + 2] = "Settings...";
        items[count + 3] = "Update script...";
        int pick = gui_pick_one(prompt, items, count + 4);
        free(items);
        done = 1;
        if (pick >= 0 && pick < count) {
            strcpy(outProfile, names.items[pick]);
            act = ACT_BACKUP;
        }
        else if (pick == count) {
            if (ask_new_name(outProfile, &names)) {
                act = ACT_BACKUP;
            }
            else {
                done = 0; /* a cancelled name goes back to the list */
            }
        }
        else if (pick == count + 1) {
            act = ACT_RESTORE;
        }
        else if (pick == count + 2) {
            act = ACT_SETTINGS;
        }
        else if (pick == count + 3) {
            act = ACT_UPDATE;
        }
    }
    names_free(&names);
    return act;
}

/* ---- backup ------------------------------------------------------------- */

#define KIND_SKIP    0
#define KIND_ACCOUNT 1
#define KIND_DEVICE  2

/* Account or device save, from the folder Checkpoint keeps its backups in
 * (saves/ or device/); BCAT and system saves are skipped. */
int entry_kind(int idx)
{
    char* p = title_backup_path(idx, 0);
    char path[PATHN];
    char parent[NAMEN];
    strncpy(path, p, PATHN - 1);
    path[PATHN - 1] = '\0';
    free(p);
    rstrip_slash(path);
    int n = strlen(path);
    while (n > 0 && path[n - 1] != '/') {
        n = n - 1;
    }
    if (n == 0) {
        return KIND_SKIP;
    }
    path[n - 1] = '\0';
    basename_of(path, parent, NAMEN);
    if (strcmp(parent, "saves") == 0) {
        return KIND_ACCOUNT;
    }
    if (strcmp(parent, "device") == 0) {
        return KIND_DEVICE;
    }
    return KIND_SKIP;
}

/* The game's cloud folder, made safe as one path segment: the name of
 * Checkpoint's backup folder for it ("0x<TITLEID> <name>"). For a title with
 * any non-ASCII byte Checkpoint uses "0x<TITLEID> 0x<TITLEID>", which nobody
 * can recognise in Koofr, so the cloud folder takes the real title name. */
void game_folder(int idx, char* out, int size)
{
    char* p    = title_backup_path(idx, 0);
    char* name = title_name(idx);
    char path[PATHN];
    char base[NAMEN];
    strncpy(path, p, PATHN - 1);
    path[PATHN - 1] = '\0';
    free(p);
    rstrip_slash(path);
    basename_of(path, base, NAMEN);
    int ascii = 1;
    int i     = 0;
    while (name[i] != '\0') {
        if ((name[i] & 0xFF) >= 0x80) {
            ascii = 0;
        }
        i = i + 1;
    }
    if (!ascii) {
        char* tid = title_id(idx);
        snprintf(base, NAMEN, "0x%s %s", tid, name);
        free(tid);
    }
    free(name);
    sanitize_name(base, out, size);
}

int g_dump_files = 0; /* files written by the current dump_save */

/* Copy the archive folder `dir` ("/" or "/sub/") of save handle `h` under the
 * SD folder `dstDir`. 1 OK, 0 failed. */
int dump_dir(int h, char* dir, char* dstDir)
{
    struct directory* d = sav_list(h, dir);
    if (d == NULL) {
        return 0;
    }
    char local[PATHN];
    int ok = 1;
    int i  = 0;
    while (i < d->count && ok) {
        char* entry = d->files[i];
        int n       = strlen(entry);
        if (strlen(dstDir) + n + 1 >= PATHN) {
            ok = 0;
        }
        else if (n > 0 && entry[n - 1] == '/') {
            sprintf(local, "%s%s", dstDir, entry);
            sd_mkdirs(local);
            ok = dump_dir(h, entry, dstDir);
        }
        else {
            char* data = NULL;
            int size   = 0;
            if (sav_read(h, entry, &data, &size) != 0) {
                ok = 0;
            }
            else {
                sprintf(local, "%s%s", dstDir, entry);
                FILE* f = fopen(local, "wb");
                if (f == NULL) {
                    ok = 0;
                }
                else {
                    if (size > 0 && fwrite(data, 1, size, f) != size) {
                        ok = 0;
                    }
                    if (fclose(f) != 0) {
                        ok = 0;
                    }
                    g_dump_files = g_dump_files + 1;
                }
                free(data);
            }
        }
        i = i + 1;
    }
    delete_directory(d);
    return ok;
}

/* Copy the save of catalog entry `idx` into the SD folder `dstDir`, read-only.
 * 1 OK, 0 failed, -1 the save holds no files. */
int dump_save(int idx, char* dstDir)
{
    int h = sav_open(idx, 0);
    if (h < 0) {
        char line[64];
        sprintf(line, "  cannot open the save (%d)", h);
        script_log(line);
        return 0;
    }
    g_dump_files = 0;
    int ok       = dump_dir(h, "/", dstDir);
    sav_close(h);
    if (!ok) {
        script_log("  cannot copy the save");
        return 0;
    }
    if (g_dump_files == 0) {
        return -1;
    }
    return 1;
}

int g_uploaded  = 0;
int g_unchanged = 0;
int g_failed    = 0;

/* One catalog entry: dump, zip, fingerprint, and upload when it changed. */
void backup_entry(int idx, int kind, char* profile)
{
    char dump[PATHN];
    char zip[PATHN];
    char key[96];
    char fp[96];
    char folder[NAMEN];
    char base[64];
    char incoming[64];
    char fin[NAMEN];
    char line[NAMEN + 64];
    char* name = title_name(idx);
    char* tid  = title_id(idx);
    progress_label(0, name);
    sprintf(dump, "%s/dump", g_work);
    sprintf(zip, "%s/up.zip", g_work);
    game_folder(idx, folder, NAMEN);
    if (kind == KIND_ACCOUNT) {
        sprintf(key, "a:%s:%s", profile, tid);
        sprintf(base, "%s %s", g_stamp, profile);
        sprintf(incoming, "%s-a-%s.zip", tid, g_stamp);
    }
    else {
        sprintf(key, "d:%s", tid);
        strcpy(base, g_stamp);
        sprintf(incoming, "%s-d-%s.zip", tid, g_stamp);
    }
    snprintf(line, NAMEN + 64, "%s:", folder);
    script_log(line);

    rm_tree(dump);
    sd_mkdirs(dump);
    int dumped = dump_save(idx, dump);
    int zipped = 0;
    if (dumped == 1) {
        zipped = zip_dir(dump, zip) == 0 && zip_fingerprint(zip, fp, 96);
    }
    rm_tree(dump);
    rmdir(dump);

    if (dumped == -1) {
        script_log("  empty save, skipped");
        g_unchanged = g_unchanged + 1;
    }
    else if (!zipped) {
        if (dumped == 1) {
            script_log("  cannot zip the save");
        }
        g_failed = g_failed + 1;
    }
    else if (state_get(key) != NULL && strcmp(state_get(key), fp) == 0) {
        script_log("  unchanged");
        g_unchanged = g_unchanged + 1;
    }
    else if (upload_version(zip, kind == KIND_ACCOUNT ? "saves" : "device", folder, base, incoming, fin)) {
        state_set(key, fp);
        snprintf(line, NAMEN + 64, "  uploaded %s", fin);
        script_log(line);
        g_uploaded = g_uploaded + 1;
    }
    else {
        g_failed = g_failed + 1;
    }
    remove(zip);
    free(name);
    free(tid);
}

/* The message a run ends with when Koofr cannot go on: g_fatal's reason, or
 * the last HTTP status when the folders could not be made for another one. */
void show_stop(void)
{
    char line[96];
    if (g_fatal == FATAL_LOGIN) {
        strcpy(line, "Koofr rejected the login. Check email and app password in Settings.");
    }
    else if (g_fatal == FATAL_FULL) {
        strcpy(line, "Koofr storage is full.");
    }
    else if (g_fatal == FATAL_NET) {
        strcpy(line, "No connection to Koofr. Check Wi-Fi.");
    }
    else {
        sprintf(line, "Koofr answered HTTP %d.", g_status);
    }
    script_log(line);
    gui_message(line);
}

/* After a transfer error or a 401/403 in the middle of a run, one PROPFIND
 * Depth 0 of SaveSyncNx/: when Koofr answers it, the failure was that save's
 * alone (curl gives up on a transfer slower than 300 B/s for 10 s, and Koofr
 * may refuse one file), so g_fatal is cleared and the run goes on. Otherwise
 * g_fatal keeps a reason and the run stops. A full storage is never rechecked. */
void recheck_koofr(void)
{
    int was = g_fatal;
    g_fatal = 0;
    int st  = dav_call("PROPFIND", g_url_root, "Depth: 0\nContent-Type: application/xml; charset=utf-8", PROPFIND_BODY, NULL, NULL);
    if (st == 207) {
        script_log("  Koofr still answers: only this save failed");
        return;
    }
    if (g_fatal == 0) {
        g_fatal = was;
    }
}

/* The backup of a run: every account and device save that changed since its
 * last upload becomes a new version in Koofr. No network, a rejected login or
 * a full storage stop the run at once, without the summary (a transfer error
 * or a 401/403 on one save only after recheck_koofr); anything else fails only
 * the save it happened to. */
int backup_run(char* profile)
{
    char line[128];
    g_uploaded  = 0;
    g_unchanged = 0;
    g_failed    = 0;
    g_fatal     = 0;
    rm_tree(g_work);
    snprintf(line, 128, "backup as %s", profile);
    script_log(line);
    gui_status("Connecting to Koofr...");
    if (!cloud_prepare()) {
        show_stop();
        return 0;
    }
    cloud_cleanup_incoming();
    if (g_fatal != 0) {
        show_stop();
        return 0;
    }
    state_load();
    show_status();

    int count = titles_count();
    progress_begin(0, "Saves", count);
    int idx = 0;
    while (idx < count && g_fatal == 0) {
        int kind = entry_kind(idx);
        if (kind != KIND_SKIP) {
            backup_entry(idx, kind, profile);
            if (g_fatal == FATAL_NET || g_fatal == FATAL_LOGIN) {
                recheck_koofr();
            }
        }
        idx = idx + 1;
        progress_set(0, idx);
    }
    progress_end(0);
    progress_clear();
    if (g_fatal != 0) {
        show_stop();
        return 0;
    }
    sprintf(line, "Backup done: %d uploaded, %d unchanged, %d failed.", g_uploaded, g_unchanged, g_failed);
    script_log(line);
    gui_message(line);
    return 1;
}

/* ---- restore ------------------------------------------------------------ */

/* 1 when the folder name starts with a title id ("0x" + 16 hex digits, then a
 * space or the end); `out` gets the digits in uppercase. */
int folder_title_id(char* folder, char* out)
{
    if (folder[0] != '0' || (folder[1] != 'x' && folder[1] != 'X')) {
        return 0;
    }
    int i = 0;
    while (i < 16) {
        int c = folder[2 + i];
        if (hexval(c) < 0) {
            return 0;
        }
        if (c >= 'a' && c <= 'f') {
            c = c - 'a' + 'A';
        }
        out[i] = c;
        i      = i + 1;
    }
    out[16] = '\0';
    return folder[18] == ' ' || folder[18] == '\0';
}

/* 1 when one of the cloud folders `folders` belongs to title `tid`. */
int folders_have_title(struct names* folders, char* tid)
{
    char id[20];
    int i = 0;
    while (i < folders->count) {
        if (folder_title_id(folders->items[i], id) && strcmp(id, tid) == 0) {
            return 1;
        }
        i = i + 1;
    }
    return 0;
}

int names_has(struct names* n, char* s)
{
    int i = 0;
    while (i < n->count) {
        if (strcmp(n->items[i], s) == 0) {
            return 1;
        }
        i = i + 1;
    }
    return 0;
}

/* The game folders under `url`. A folder Koofr does not have yet (404: no
 * backup of that kind so far) is an empty list. 1 OK. */
int restore_list(char* url, struct names* out)
{
    if (dav_list(url, out)) {
        return 1;
    }
    return g_fatal == 0 && g_status == 404;
}

/* How many games of one kind Koofr has that the console has no save of:
 * the distinct title ids of `folders` missing from `onConsole`. */
int count_missing(struct names* folders, struct names* onConsole)
{
    char id[20];
    char prev[20];
    int k   = 0;
    prev[0] = '\0';
    sort_names(folders);
    int i = 0;
    while (i < folders->count) {
        if (folder_title_id(folders->items[i], id) && strcmp(id, prev) != 0) {
            strcpy(prev, id);
            if (!names_has(onConsole, id)) {
                k = k + 1;
            }
        }
        i = i + 1;
    }
    return k;
}

/* Download one version and unzip it into a new folder in Checkpoint's backup
 * folder for catalog entry `idx`: "<version>", or "<version> (2)", ... when
 * that is taken, so no existing folder is ever written into. The zip is
 * unpacked into savesync/restore first and renamed into place only once it is
 * whole: hold-B stops the script at the next statement, so a cleanup after
 * unzip would never run, and Checkpoint must never list a half-written folder.
 * What a failure or hold-B leaves in savesync/ goes at the next run's start. */
void restore_version(int idx, char* kindUrl, char* folder, char* version)
{
    char url[URLBUFN];
    char down[PATHN];
    char stage[PATHN];
    char base[PATHN];
    char dest[PATHN];
    char zipName[NAMEN];
    char line[PATHN + 64];
    sprintf(down, "%s/down.zip", g_work);
    sprintf(stage, "%s/restore", g_work);
    snprintf(zipName, NAMEN, "%s.zip", version);
    strcpy(url, kindUrl);
    int ok = url_add(url, folder, URLBUFN) && url_add(url, zipName, URLBUFN);

    char* p = title_backup_path(idx, 0);
    strncpy(base, p, PATHN - 1);
    base[PATHN - 1] = '\0';
    free(p);
    rstrip_slash(base);
    dest[0] = '\0';
    int copy = 1;
    while (ok && dest[0] == '\0' && copy <= MAX_COPIES) {
        if (copy == 1) {
            snprintf(line, PATHN + 64, "%s/%s", base, version);
        }
        else {
            snprintf(line, PATHN + 64, "%s/%s (%d)", base, version, copy);
        }
        if (strlen(line) >= PATHN) {
            ok = 0;
        }
        else if (!sd_exists(line)) {
            strcpy(dest, line);
        }
        copy = copy + 1;
    }
    if (dest[0] == '\0') {
        ok = 0;
    }

    int made = 0;
    if (ok) {
        gui_status("Downloading from Koofr...");
        ok = dav_get_file(url, down);
        show_status();
    }
    if (ok) {
        made = 1;
        ok   = sd_mkdirs(stage) == 0 && unzip(down, stage) == 0 && rename(stage, dest) == 0;
    }
    remove(down);
    if (ok) {
        snprintf(line, PATHN + 64, "restored %s into %s", zipName, dest);
        script_log(line);
        gui_message("Downloaded. Pick it in Checkpoint's backup\nlist for this game and press Restore.\nFor an account save, select the right\nprofile first.");
        return;
    }
    if (made) {
        rm_tree(stage);
        rmdir(stage);
    }
    snprintf(line, PATHN + 64, "restore of %s failed", zipName);
    script_log(line);
    gui_message("The download failed and was removed.\nNothing was changed.");
}

/* The versions of one game, from every cloud folder of its title id (a game
 * renamed in Checkpoint has two), newest first. 1 when the restore is over,
 * 0 to go back to the game list. */
int restore_game(int idx, char* tid, char* label, char* kindUrl, struct names* folders)
{
    struct names all;
    struct names shown;
    struct names from;
    char id[20];
    char url[URLBUFN];
    char entry[NAMEN * 2 + 2];
    char text[NAMEN * 2 + 256];
    names_init(&all);
    names_init(&shown);
    names_init(&from);

    int i = 0;
    while (i < folders->count) {
        char* folder = folders->items[i];
        if (folder_title_id(folder, id) && strcmp(id, tid) == 0) {
            struct names files;
            names_init(&files);
            strcpy(url, kindUrl);
            if (!url_add(url, folder, URLBUFN) || !dav_list(url, &files)) {
                names_free(&files);
                names_free(&all);
                show_stop();
                return 1;
            }
            int j = 0;
            while (j < files.count) {
                char* f = files.items[j];
                int n   = strlen(f);
                int zip = n > 4 && f[n - 4] == '.' && f[n - 3] == 'z' && f[n - 2] == 'i' && f[n - 1] == 'p';
                if (zip && n - 4 < NAMEN && strlen(folder) < NAMEN) {
                    strncpy(entry, f, n - 4);
                    entry[n - 4] = '\0';
                    strcat(entry, "\t");
                    strcat(entry, folder);
                    names_add(&all, entry);
                }
                j = j + 1;
            }
            names_free(&files);
        }
        i = i + 1;
    }
    if (all.count == 0) {
        names_free(&all);
        snprintf(text, NAMEN * 2 + 256, "There are no versions of %s in Koofr.", label);
        gui_message(text);
        return 0;
    }

    sort_names(&all);
    i = all.count - 1;
    while (i >= 0) {
        char* e = all.items[i];
        int t   = 0;
        while (e[t] != '\t') {
            t = t + 1;
        }
        strncpy(entry, e, t);
        entry[t] = '\0';
        names_add(&shown, entry);
        int k = 0;
        while (e[t + 1 + k] != '\0') {
            entry[k] = e[t + 1 + k];
            k        = k + 1;
        }
        entry[k] = '\0';
        names_add(&from, entry);
        i = i - 1;
    }
    names_free(&all);

    char prompt[NAMEN + 32];
    snprintf(prompt, NAMEN + 32, "Pick a version of %s", label);
    int result = -1;
    while (result < 0) {
        int v = gui_pick_one(prompt, shown.items, shown.count);
        if (v < 0) {
            result = 0;
        }
        else {
            snprintf(text, NAMEN * 2 + 256,
                     "Download\n%s\ninto Checkpoint's backup folder for\n%s?\n\nNothing is written to the game until\nyou restore it in Checkpoint.",
                     shown.items[v], label);
            if (gui_confirm(text)) {
                restore_version(idx, kindUrl, from.items[v], shown.items[v]);
                result = 1;
            }
        }
    }
    names_free(&shown);
    names_free(&from);
    return result;
}

/* Restore: pick a game the console has a save of, then one of its versions in
 * Koofr; it lands in Checkpoint's backup folder, never in the save. B goes
 * back one step (confirm → versions → games → end). */
int restore_flow(void)
{
    struct names sf;
    struct names df;
    struct names onA;
    struct names onD;
    struct names labels;
    struct names keys;
    struct names tids;
    names_init(&sf);
    names_init(&df);
    names_init(&onA);
    names_init(&onD);
    names_init(&labels);
    names_init(&keys);
    names_init(&tids);
    g_fatal = 0;
    rm_tree(g_work);
    gui_status("Connecting to Koofr...");
    cloud_urls();
    int ok = restore_list(g_url_saves, &sf) && restore_list(g_url_device, &df);
    show_status();
    if (!ok) {
        names_free(&sf);
        names_free(&df);
        show_stop();
        return 0;
    }

    /* the games on the console that have backups of the same kind */
    int count  = titles_count();
    int* gidx  = (int*)malloc((count + 1) * sizeof(int));
    int* gkind = (int*)malloc((count + 1) * sizeof(int));
    char key[24];
    char label[NAMEN + 16];
    int games = 0;
    int idx   = 0;
    while (idx < count) {
        int kind = entry_kind(idx);
        if (kind != KIND_SKIP) {
            char* tid = title_id(idx);
            sprintf(key, "%c%s", kind == KIND_ACCOUNT ? 'a' : 'd', tid);
            names_add(kind == KIND_ACCOUNT ? &onA : &onD, tid);
            if (folders_have_title(kind == KIND_ACCOUNT ? &sf : &df, tid) && !names_has(&keys, key)) {
                char* name = title_name(idx);
                if (kind == KIND_ACCOUNT) {
                    snprintf(label, NAMEN + 16, "%s", name);
                }
                else {
                    snprintf(label, NAMEN + 16, "%s (device)", name);
                }
                free(name);
                names_add(&labels, label);
                names_add(&keys, key);
                names_add(&tids, tid);
                gidx[games]  = idx;
                gkind[games] = kind;
                games        = games + 1;
            }
            free(tid);
        }
        idx = idx + 1;
    }
    int missing = count_missing(&sf, &onA) + count_missing(&df, &onD);

    if (games == 0) {
        gui_message("There are no backups in Koofr for the games on this console.");
    }
    else {
        char prompt[160];
        if (missing > 0) {
            sprintf(prompt, "Pick a game\n(%d more in Koofr have no save on this console;\nstart a game once to create its save)", missing);
        }
        else {
            strcpy(prompt, "Pick a game");
        }
        int done = 0;
        while (!done) {
            int g = gui_pick_one(prompt, labels.items, labels.count);
            if (g < 0) {
                done = 1;
            }
            else if (gkind[g] == KIND_ACCOUNT) {
                done = restore_game(gidx[g], tids.items[g], labels.items[g], g_url_saves, &sf);
            }
            else {
                done = restore_game(gidx[g], tids.items[g], labels.items[g], g_url_device, &df);
            }
        }
    }
    free(gidx);
    free(gkind);
    names_free(&sf);
    names_free(&df);
    names_free(&onA);
    names_free(&onD);
    names_free(&labels);
    names_free(&keys);
    names_free(&tids);
    return 1;
}

/* ---- update ------------------------------------------------------------- */

#define TAGN 32

/* 1 when `tag` is a plausible release tag: 1-31 of A-Z a-z 0-9 . - _ (it goes
 * into a URL path and into the dialogs). */
int tag_ok(char* tag)
{
    int n = strlen(tag);
    if (n < 1 || n >= TAGN) {
        return 0;
    }
    int i = 0;
    while (i < n) {
        int c = tag[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return 0;
        }
        i = i + 1;
    }
    return 1;
}

void update_failed(char* reason)
{
    char line[192];
    snprintf(line, 192, "Could not update: %s.\nNothing was changed.", reason);
    script_log(line);
    gui_message(line);
}

/* GET from GitHub: no login, but GitHub wants a User-Agent. Returns the
 * status; on 200 *out is the NUL-terminated body for the caller to free,
 * otherwise NULL. */
int github_get(char* url, char** out, int* outSize)
{
    *out     = NULL;
    *outSize = 0;
    int st   = web_request("GET", url, "User-Agent: savesync", "", 0, out, outSize, NULL);
    if (st != 200 && *out != NULL) {
        free(*out);
        *out = NULL;
    }
    return st;
}

/* Put the checked download `from` in place of the installed script `to`.
 * FAT's rename does not replace a file, so the old script is moved aside
 * first and put back if the new one cannot take its place. 1 OK. */
int install_script(char* from, char* to)
{
    char aside[PATHN];
    if (rename(from, to) == 0) {
        return 1;
    }
    sprintf(aside, "%s/installed.old", g_work);
    remove(aside);
    if (rename(to, aside) != 0) {
        return 0;
    }
    if (rename(from, to) == 0) {
        remove(aside);
        return 1;
    }
    rename(aside, to);
    return 0;
}

/* "Update script...": compare the latest GitHub release with this version,
 * and on confirm replace <root>/scripts/universal/savesync.c with that
 * release's script, but only once it is checked to be SaveSync of that tag.
 * Any failure leaves the installed script as it was. */
int update_flow(void)
{
    char tag[TAGN];
    char url[URLBUFN];
    char reason[128];
    char text[256];
    char want[64];
    char dir[PATHN];
    char target[PATHN];
    char tmp[PATHN];
    char* body = NULL;
    int n      = 0;

    gui_status("Checking GitHub for a new version...");
    int st = github_get(GITHUB_API, &body, &n);
    show_status();
    if (body == NULL) {
        sprintf(reason, "GitHub could not be reached (%d)", st);
        update_failed(reason);
        return 0;
    }
    struct JSON* j = json_new();
    json_parse(j, body);
    free(body);
    tag[0] = '\0';
    if (json_is_valid(j)) {
        jget(j, "tag_name", tag, TAGN);
    }
    json_delete(j);
    if (!tag_ok(tag)) {
        update_failed("GitHub's answer names no release");
        return 0;
    }
    if (strcmp(tag, SAVESYNC_VERSION) == 0) {
        sprintf(text, "SaveSync %s is the latest version.", SAVESYNC_VERSION);
        gui_message(text);
        return 1;
    }
    sprintf(text, "Update SaveSync from %s to %s?\n\nThe new version runs from the next start.", SAVESYNC_VERSION, tag);
    if (!gui_confirm(text)) {
        return 0;
    }

    snprintf(url, URLBUFN, "%s/%s/scripts/savesync.c", GITHUB_RAW, tag);
    st = github_get(url, &body, &n);
    if (body == NULL) {
        sprintf(reason, "GitHub could not be reached (%d)", st);
        update_failed(reason);
        return 0;
    }
    sprintf(want, "#define SAVESYNC_VERSION \"%s\"", tag);
    if (strstr(body, want) == NULL) {
        free(body);
        sprintf(reason, "the download is not SaveSync %s", tag);
        update_failed(reason);
        return 0;
    }
    sprintf(tmp, "%s/update.c", g_work);
    sprintf(dir, "%s/scripts/universal", g_root);
    sprintf(target, "%s/savesync.c", dir);
    sd_mkdirs(g_work);
    FILE* f   = fopen(tmp, "wb");
    int wrote = -1;
    if (f != NULL) {
        wrote = fwrite(body, 1, n, f);
        if (fclose(f) != 0) {
            wrote = -1;
        }
    }
    free(body);
    if (wrote != n || sd_mkdirs(dir) != 0 || !install_script(tmp, target)) {
        remove(tmp);
        update_failed("the script could not be written to the SD card");
        return 0;
    }
    sprintf(text, "Updated to %s. Run savesync again to use it.", tag);
    script_log(text);
    gui_message(text);
    return 1;
}

#ifndef SAVESYNC_NO_MAIN
int main(int argc, char** argv)
{
    char profile[PROFN];
    init_paths();
    if (vault_read() != 1) {
        if (settings_change() == 0) {
            return 0;
        }
    }
    show_status();
    /* Settings and Update come back to the first dialog; Backup and Restore end the run. */
    int act = ACT_SETTINGS;
    while (act == ACT_SETTINGS || act == ACT_UPDATE) {
        act = first_dialog(profile);
        if (act == ACT_BACKUP) {
            backup_run(profile);
        }
        else if (act == ACT_RESTORE) {
            restore_flow();
        }
        else if (act == ACT_SETTINGS) {
            settings_menu();
        }
        else if (act == ACT_UPDATE) {
            update_flow();
        }
    }
    return 0;
}
#endif
