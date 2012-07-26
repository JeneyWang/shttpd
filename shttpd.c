/* shttpd - a fork of darkhttpd
 *
 * copyright (c) 2003-2008 Emil Mikulic.
 * copyright (c) 2010 Calvin Morrison.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

static const char
    pkgname[]   = "shttpd/0.1",
    copyright[] = "copyright (c) 2003-2008 Emil Mikulic, 2010 Calvin Morrison",
    rcsid[]     = "$Id: darkhttpd.c 188 2008-11-04 08:53:22Z emil calvin $";

#ifndef DEBUG
#define NDEBUG
static const int debug = 0;
#else
static const int debug = 1;
#endif

#ifdef __linux
#define _GNU_SOURCE /* for strsignal() and vasprintf() */
#include <sys/sendfile.h>
#endif

#ifdef __sun__
#include <sys/sendfile.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef min
#define min(a,b) ( ((a)<(b)) ? (a) : (b) )
#endif

#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif

#if defined(O_EXCL) && !defined(O_EXLOCK)
#define O_EXLOCK O_EXCL
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__linux)
#include <err.h>
#else
/* err - prints "error: format: strerror(errno)" to stderr and exit()s with
 * the given code.
 */
static void
err(const int code, const char *format, ...)
{
   va_list va;

   va_start(va, format);
   fprintf(stderr, "error: ");
   vfprintf(stderr, format, va);
   fprintf(stderr, ": %s\n", strerror(errno));
   va_end(va);
   exit(code);
}

/* errx - err without the strerror */
static void
errx(const int code, const char *format, ...)
{
   va_list va;

   va_start(va, format);
   fprintf(stderr, "error: ");
   vfprintf(stderr, format, va);
   fprintf(stderr, "\n");
   va_end(va);
   exit(code);
}

/* warn - err without the exit */
static void
warn(const char *format, ...)
{
   va_list va;

   va_start(va, format);
   fprintf(stderr, "warning: ");
   vfprintf(stderr, format, va);
   fprintf(stderr, ": %s\n", strerror(errno));
   va_end(va);
}
#endif

/* ---------------------------------------------------------------------------
 * LIST_* macros taken from FreeBSD's src/sys/sys/queue.h,v 1.56
 * Copyright (c) 1991, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Under a BSD license.
 */
#define LIST_HEAD(name, type)                                           \
struct name {                                                           \
        struct type *lh_first;  /* first element */                     \
}

#define LIST_HEAD_INITIALIZER(head)                                     \
        { NULL }

#define LIST_ENTRY(type)                                                \
struct {                                                                \
        struct type *le_next;   /* next element */                      \
        struct type **le_prev;  /* address of previous next element */  \
}

#define LIST_FIRST(head)        ((head)->lh_first)

#define LIST_FOREACH(var, head, field)                                  \
        for ((var) = LIST_FIRST((head));                                \
            (var);                                                      \
            (var) = LIST_NEXT((var), field))

#define LIST_FOREACH_SAFE(var, head, field, tvar)                       \
    for ((var) = LIST_FIRST((head));                                    \
        (var) && ((tvar) = LIST_NEXT((var), field), 1);                 \
        (var) = (tvar))

#define LIST_INIT(head) do {                                            \
        LIST_FIRST((head)) = NULL;                                      \
} while (0)

#define LIST_INSERT_HEAD(head, elm, field) do {                         \
        if ((LIST_NEXT((elm), field) = LIST_FIRST((head))) != NULL)     \
                LIST_FIRST((head))->field.le_prev = &LIST_NEXT((elm), field);\
        LIST_FIRST((head)) = (elm);                                     \
        (elm)->field.le_prev = &LIST_FIRST((head));                     \
} while (0)

#define LIST_NEXT(elm, field)   ((elm)->field.le_next)

#define LIST_REMOVE(elm, field) do {                                    \
        if (LIST_NEXT((elm), field) != NULL)                            \
                LIST_NEXT((elm), field)->field.le_prev =                \
                    (elm)->field.le_prev;                               \
        *(elm)->field.le_prev = LIST_NEXT((elm), field);                \
} while (0)
/* ------------------------------------------------------------------------ */



LIST_HEAD(conn_list_head, connection) connlist =
    LIST_HEAD_INITIALIZER(conn_list_head);

struct connection
{
    LIST_ENTRY(connection) entries;

    int socket;
    in_addr_t client;
    time_t last_active;
    enum {
        RECV_REQUEST,   /* receiving request */
        SEND_HEADER,    /* sending generated header */
        SEND_REPLY,     /* sending reply */
        DONE            /* connection closed, need to remove from queue */
        } state;

    /* char request[request_length+1] is null-terminated */
    char *request;
    size_t request_length;

    /* request fields */
    char *method, *uri, *referer, *user_agent;
    size_t range_begin, range_end;
    int range_begin_given, range_end_given;

    char *header;
    size_t header_length, header_sent;
    int header_dont_free, header_only, http_code, conn_close;

    enum { REPLY_GENERATED, REPLY_FROMFILE } reply_type;
    char *reply;
    int reply_dont_free;
    int reply_fd;
    size_t reply_start, reply_length, reply_sent;

    unsigned int total_sent; /* header + body = total, for logging */
};

struct mime_mapping
{
    char *extension, *mimetype;
};

struct mime_mapping *mime_map = NULL;
size_t mime_map_size = 0;
size_t longest_ext = 0;


/* Time is cached in the event loop to avoid making an excessive number of
 * gettimeofday() calls.
 */
static time_t now;

/* To prevent a malformed request from eating up too much memory, die once the
 * request exceeds this many bytes:
 */
#define MAX_REQUEST_LENGTH 4000


/* Defaults can be overridden on the command-line */
static int idletime = 60; /*idle time before timeout*/
static char *keep_alive_field = NULL;
static in_addr_t bindaddr = INADDR_ANY;
static unsigned short bindport = 80;
static int max_connections = -1;        /* kern.ipc.somaxconn */
static const char *index_name = "index.html";

static int sockin = -1;             /* socket to accept connections from */
static char *wwwroot = NULL;        /* a path name */
static char *logfile_name = NULL;   /* NULL = no logging */
static FILE *logfile = NULL;
static char *pidfile_name = NULL;   /* NULL = no pidfile */
static int want_chroot = 0, want_daemon = 0, want_accf = 0;
static uint32_t num_requests = 0;
static uint64_t total_in = 0, total_out = 0;

static int running = 1; /* signal handler sets this to false */

#define INVALID_UID ((uid_t) -1)
#define INVALID_GID ((gid_t) -1)

static uid_t drop_uid = INVALID_UID;
static gid_t drop_gid = INVALID_GID;

/* Default mimetype mappings - make sure this array is NULL terminated. */
static const char *default_extension_map[] = {
    "application/ogg"      " ogg",
    "application/pdf"      " pdf",
    "application/xml"      " xsl xml",
    "application/xml-dtd"  " dtd",
    "application/xslt+xml" " xslt",
    "application/zip"      " zip",
    "application/x-tar"    " tar",
    "application/x-bzip2"  " bz2 boz bz"
    "audio/mpeg"           " mp2 mp3 mpga",
    "audio/midi"           " midi mid",
    "image/gif"            " gif",
    "image/jpeg"           " jpeg jpe jpg",
    "image/png"            " png",
    "text/css"             " css",
    "text/html"            " html htm",
    "text/javascript"      " js",
    "text/plain"           " txt asc h c",
    "video/mpeg"           " mpeg mpe mpg",
    "video/quicktime"      " qt mov",
    "video/x-msvideo"      " avi",
    NULL
};

static const char default_mimetype[] = "application/octet-stream";

/* Connection or Keep-Alive field, depending on conn_close. */
#define keep_alive(conn) ((conn)->conn_close ? \
    "Connection: close\r\n" : keep_alive_field)

/* Prototypes. */
static void poll_recv_request(struct connection *conn);
static void poll_send_header(struct connection *conn);
static void poll_send_reply(struct connection *conn);


/* ---------------------------------------------------------------------------
 * close that dies on error.
 */
static void xclose(const int fd) {
    if (close(fd) == -1) err(1, "close()");
}


/* ---------------------------------------------------------------------------
 * malloc that errx()s if it can't allocate.
 */
static void *xmalloc(const size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) errx(1, "can't allocate %lu bytes", size);
    return ptr;
}


/* ---------------------------------------------------------------------------
 * realloc() that errx()s if it can't allocate.
 */
static void *xrealloc(void *original, const size_t size) {
    void *ptr = realloc(original, size);
    if (ptr == NULL) errx(1, "can't reallocate %lu bytes", size);
    return ptr;
}


/* ---------------------------------------------------------------------------
 * strdup() that errx()s if it can't allocate.  Do this by hand since strdup()
 * isn't C89.
 */
static char *xstrdup(const char *src)
{
    size_t len = strlen(src) + 1;
    char *dest = xmalloc(len);
    memcpy(dest, src, len);
    return dest;
}


#ifdef __sun /* unimpressed by Solaris */
static int vasprintf(char **strp, const char *fmt, va_list ap)
{
    char tmp;
    int result = vsnprintf(&tmp, 1, fmt, ap);
    *strp = xmalloc(result+1);
    result = vsnprintf(*strp, result+1, fmt, ap);
    return result;
}
#endif


// vasprintf() that errx()s if it fails.
static unsigned int xvasprintf(char **ret, const char *format, va_list ap)
{
    int len = vasprintf(ret, format, ap);
    if (ret == NULL || len == -1) errx(1, "out of memory in vasprintf()");
    return (unsigned int)len;
}


// asprintf() that errx()s if it fails.
static unsigned int xasprintf(char **ret, const char *format, ...)
{
    va_list va;
    unsigned int len;

    va_start(va, format);
    len = xvasprintf(ret, format, va);
    va_end(va);
    return len;
}


/* ---------------------------------------------------------------------------
 * Append buffer code.  A somewhat efficient string buffer with pool-based
 * reallocation.
 */
#define APBUF_INIT 4096
#define APBUF_GROW APBUF_INIT
struct apbuf
{
    size_t length, pool;
    char *str;
};


static struct apbuf *make_apbuf(void)
{
    struct apbuf *buf = xmalloc(sizeof(struct apbuf));
    buf->length = 0;
    buf->pool = APBUF_INIT;
    buf->str = xmalloc(buf->pool);
    return buf;
}


static void appendl(struct apbuf *buf, const char *s, const size_t len)
{
    if (buf->pool < buf->length + len)
    {
        /* pool has dried up */
        while (buf->pool < buf->length + len) buf->pool += APBUF_GROW;
        buf->str = xrealloc(buf->str, buf->pool);
    }

    memcpy(buf->str + buf->length, s, len);
    buf->length += len;
}


#ifdef __GNUC__
#define append(buf, s) appendl(buf, s, \
    (__builtin_constant_p(s) ? sizeof(s)-1 : strlen(s)) )
#else
static void append(struct apbuf *buf, const char *s)
{
    appendl(buf, s, strlen(s));
}
#endif


static void appendf(struct apbuf *buf, const char *format, ...)
{
    char *tmp;
    va_list va;
    size_t len;

    va_start(va, format);
    len = xvasprintf(&tmp, format, va);
    va_end(va);

    appendl(buf, tmp, len);
    free(tmp);
}


// Make the specified socket non-blocking.
static void
nonblock_socket(const int sock)
{
    int flags = fcntl(sock, F_GETFL, NULL);

    if (flags == -1)
        err(1, "fcntl(F_GETFL)");
    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) == -1)
        err(1, "fcntl() to set O_NONBLOCK");
}



//Split string out of src with range [left:right-1]
static char *split_string(const char *src,
    const size_t left, const size_t right)
{
    char *dest;
    assert(left <= right);
    assert(left < strlen(src));   /* [left means must be smaller */
    assert(right <= strlen(src)); /* right) means can be equal or smaller */

    dest = xmalloc(right - left + 1);
    memcpy(dest, src+left, right-left);
    dest[right-left] = '\0';
    return dest;
}



// Consolidate slashes in-place by shifting parts of the string over repeated slashes.
static void consolidate_slashes(char *s)
{
    size_t left = 0, right = 0;
    int saw_slash = 0;

    assert(s != NULL);

    while (s[right] != '\0')
    {
        if (saw_slash)
        {
            if (s[right] == '/') right++;
            else
            {
                saw_slash = 0;
                s[left++] = s[right++];
            }
        }
        else
        {
            if (s[right] == '/') saw_slash++;
            s[left++] = s[right++];
        }
    }
    s[left] = '\0';
}


// Resolve /./ and /../ in a URI, in-place.  Returns NULL if the URI is
//invalid/unsafe, or the original buffer if successful.
static char *make_safe_uri(char *uri)
{
    struct {
        char *start;
        size_t len;
    } *chunks;
    unsigned int num_slashes, num_chunks;
    size_t urilen, i, j, pos;
    int ends_in_slash;

    assert(uri != NULL);
    if (uri[0] != '/') return NULL;
    consolidate_slashes(uri);
    urilen = strlen(uri);
    if (urilen > 0)
        ends_in_slash = (uri[urilen-1] == '/');
    else
        ends_in_slash = 1;

    /* count the slashes */
    for (i=0, num_slashes=0; i<urilen; i++)
        if (uri[i] == '/') num_slashes++;

    /* make an array for the URI elements */
    chunks = xmalloc(sizeof(*chunks) * num_slashes);

    /* split by slashes and build chunks array */
    num_chunks = 0;
    for (i=1; i<urilen;) {
        /* look for the next slash */
        for (j=i; j<urilen && uri[j] != '/'; j++)
            ;

        /* process uri[i,j) */
        if ((j == i+1) && (uri[i] == '.'))
            /* "." */;
        else if ((j == i+2) && (uri[i] == '.') && (uri[i+1] == '.')) {
            /* ".." */
            if (num_chunks == 0) {
                /* unsafe string so free chunks */
                free(chunks);
                return (NULL);
            } else
                num_chunks--;
        } else {
            chunks[num_chunks].start = uri+i;
            chunks[num_chunks].len = j-i;
            num_chunks++;
        }

        i = j + 1; /* uri[j] is a slash - move along one */
    }

    /* reassemble in-place */
    pos = 0;
    for (i=0; i<num_chunks; i++) {
        assert(pos <= urilen);
        uri[pos++] = '/';

        assert(pos + chunks[i].len <= urilen);
        assert(uri + pos <= chunks[i].start);

        if (uri+pos < chunks[i].start)
            memmove(uri+pos, chunks[i].start, chunks[i].len);
        pos += chunks[i].len;
    }
    free(chunks);

    if ((num_chunks == 0) || ends_in_slash) uri[pos++] = '/';
    assert(pos <= urilen);
    uri[pos] = '\0';
    return uri;
}


// Associates an extension with a mimetype in the mime_map.  Entries are in
// unsorted order.  Makes copies of extension and mimetype strings.
static void add_mime_mapping(const char *extension, const char *mimetype)
{
    size_t i;
    assert(strlen(extension) > 0);
    assert(strlen(mimetype) > 0);

    /* update longest_ext */
    i = strlen(extension);
    if (i > longest_ext) longest_ext = i;

    /* look through list and replace an existing entry if possible */
    for (i=0; i<mime_map_size; i++)
        if (strcmp(mime_map[i].extension, extension) == 0)
        {
            free(mime_map[i].mimetype);
            mime_map[i].mimetype = xstrdup(mimetype);
            return;
        }

    /* no replacement - add a new entry */
    mime_map_size++;
    mime_map = xrealloc(mime_map,
        sizeof(struct mime_mapping) * mime_map_size);
    mime_map[mime_map_size-1].extension = xstrdup(extension);
    mime_map[mime_map_size-1].mimetype = xstrdup(mimetype);
}


//qsort() the mime_map.  The map must be sorted before it can be searched through.
static int mime_mapping_cmp(const void *a, const void *b)
{
    return strcmp( ((const struct mime_mapping *)a)->extension,
                   ((const struct mime_mapping *)b)->extension );
}

static void sort_mime_map(void)
{
    qsort(mime_map, mime_map_size, sizeof(struct mime_mapping),
        mime_mapping_cmp);
}



//Parses a mime.types line and adds the parsed data to the mime_map.
static void parse_mimetype_line(const char *line)
{
    unsigned int pad, bound1, lbound, rbound;

    /* parse mimetype */
    for (pad=0; line[pad] == ' ' || line[pad] == '\t'; pad++);
    if (line[pad] == '\0' || /* empty line */
        line[pad] == '#')    /* comment */
        return;

    for (bound1=pad+1;
        line[bound1] != ' ' &&
        line[bound1] != '\t';
        bound1++)
    {
        if (line[bound1] == '\0') return; /* malformed line */
    }

    lbound = bound1;
    for (;;)
    {
        char *mimetype, *extension;

        /* find beginning of extension */
        for (; line[lbound] == ' ' || line[lbound] == '\t'; lbound++);
        if (line[lbound] == '\0') return; /* end of line */

        /* find end of extension */
        for (rbound = lbound;
            line[rbound] != ' ' &&
            line[rbound] != '\t' &&
            line[rbound] != '\0';
            rbound++);

        mimetype = split_string(line, pad, bound1);
        extension = split_string(line, lbound, rbound);
        add_mime_mapping(extension, mimetype);
        free(mimetype);
        free(extension);

        if (line[rbound] == '\0') return; /* end of line */
        else lbound = rbound + 1;
    }
}



//Adds contents of default_extension_map[] to mime_map list.  The array must be NULL terminated
static void parse_default_extension_map(void) {
    int i;

    for (i=0; default_extension_map[i] != NULL; i++)
        parse_mimetype_line(default_extension_map[i]);
}


/* ---------------------------------------------------------------------------
 * read_line - read a line from [fp], return its contents in a
 * dynamically allocated buffer, not including the line ending.
 *
 * Handles CR, CRLF and LF line endings, as well as NOEOL correctly.  If
 * already at EOF, returns NULL.  Will err() or errx() in case of
 * unexpected file error or running out of memory.
 */
static char *read_line(FILE *fp) {
   char *buf;
   long startpos, endpos;
   size_t linelen, numread;
   int c;

   startpos = ftell(fp);
   if (startpos == -1) err(1, "ftell()");

   /* find end of line (or file) */
   linelen = 0;
   for (;;)
   {
      c = fgetc(fp);
      if (c == EOF || c == (int)'\n' || c == (int)'\r') break;
      linelen++;
   }

   /* return NULL on EOF (and empty line) */
   if (linelen == 0 && c == EOF) return NULL;

   endpos = ftell(fp);
   if (endpos == -1) err(1, "ftell()");

   /* skip CRLF */
   if (c == (int)'\r' && fgetc(fp) == (int)'\n') endpos++;

   buf = (char*)xmalloc(linelen + 1);

   /* rewind file to where the line stared and load the line */
   if (fseek(fp, startpos, SEEK_SET) == -1) err(1, "fseek()");
   numread = fread(buf, 1, linelen, fp);
   if (numread != linelen)
      errx(1, "fread() %lu bytes, expecting %lu bytes", numread, linelen);

   /* terminate buffer */
   buf[linelen] = 0;

   /* advance file pointer over the endline */
   if (fseek(fp, endpos, SEEK_SET) == -1) err(1, "fseek()");

   return buf;
}


//Removes the ending newline in a string, if there is one.
static void chomp(char *str)
{
   size_t len = strlen(str);
   if (len == 0) return;
   if (str[len-1] == '\n') str[len-1] = '\0';
}


//Adds contents of specified file to mime_map list.
static void parse_extension_map_file(const char *filename) {
    char *buf;
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) err(1, "fopen(\"%s\")", filename);

    while ( (buf = read_line(fp)) != NULL )
    {
        chomp(buf);
        parse_mimetype_line(buf);
        free(buf);
    }

    fclose(fp);
}



//Uses the mime_map to determine a Content-Type: for a requested URI.  This
//bsearch()es mime_map, so make sure it's sorted first.
static int mime_mapping_cmp_str(const void *a, const void *b) {
    return strcmp(
        (const char *)a,
        ((const struct mime_mapping *)b)->extension
    );
}

static const char *uri_content_type(const char *uri) {
    size_t period, urilen = strlen(uri);

    for (period=urilen-1;
        period > 0 &&
        uri[period] != '.' &&
        (urilen-period-1) <= longest_ext;
        period--)
            ;

    if (uri[period] == '.')
    {
        struct mime_mapping *result =
            bsearch((uri+period+1), mime_map, mime_map_size,
            sizeof(struct mime_mapping), mime_mapping_cmp_str);

        if (result != NULL)
        {
            assert(strcmp(uri+period+1, result->extension) == 0);
            return result->mimetype;
        }
    }
    /* else no period found in the string */
    return default_mimetype;
}


//Initialize the sockin global.  This is the socket that we accept connections from.
static void init_sockin(void)
{
    struct sockaddr_in addrin;
    int sockopt;

    /* create incoming socket */
    sockin = socket(PF_INET, SOCK_STREAM, 0);
    if (sockin == -1) err(1, "socket()");

    /* reuse address */
    sockopt = 1;
    if (setsockopt(sockin, SOL_SOCKET, SO_REUSEADDR,
            &sockopt, sizeof(sockopt)) == -1)
        err(1, "setsockopt(SO_REUSEADDR)");

#if 0
    /* disable Nagle since we buffer everything ourselves */
    sockopt = 1;
    if (setsockopt(sockin, IPPROTO_TCP, TCP_NODELAY,
            &sockopt, sizeof(sockopt)) == -1)
        err(1, "setsockopt(TCP_NODELAY)");
#endif

#ifdef TORTURE
    /* torture: cripple the kernel-side send buffer so we can only squeeze out
     * one byte at a time (this is for debugging)
     */
    sockopt = 1;
    if (setsockopt(sockin, SOL_SOCKET, SO_SNDBUF,
            &sockopt, sizeof(sockopt)) == -1)
        err(1, "setsockopt(SO_SNDBUF)");
#endif

    /* bind socket */
    addrin.sin_family = (u_char)PF_INET;
    addrin.sin_port = htons(bindport);
    addrin.sin_addr.s_addr = bindaddr;
    memset(&(addrin.sin_zero), 0, 8);
    if (bind(sockin, (struct sockaddr *)&addrin,
            sizeof(struct sockaddr)) == -1)
        err(1, "bind(port %u)", bindport);

    printf("listening on %s:%u\n", inet_ntoa(addrin.sin_addr), bindport);

    /* listen on socket */
    if (listen(sockin, max_connections) == -1)
        err(1, "listen()");

    /* enable acceptfilter (this is only available on FreeBSD) */
    if (want_accf)
    {
#if defined(__FreeBSD__)
        struct accept_filter_arg filt = {"httpready", ""};
        if (setsockopt(sockin, SOL_SOCKET, SO_ACCEPTFILTER,
            &filt, sizeof(filt)) == -1)
            fprintf(stderr, "cannot enable acceptfilter: %s\n",
                strerror(errno));
        else
            printf("enabled acceptfilter\n");
#else
        printf("this platform doesn't support acceptfilter\n");
#endif
    }
}


 //Prints  usage statement.
static void usage(void)
{
    printf("\n"
    "usage: shttpd /path/to/wwwroot [options]\n\n"
    "options:\n\n");
    printf(
    "\t--port number (default: %u)\n" /* bindport */
    "\t\tSpecifies which port to listen on for connections.\n"
    "\n", bindport);
    printf(
    "\t--addr ip (default: all)\n"
    "\t\tIf multiple interfaces are present, specifies\n"
    "\t\twhich one to bind the listening port to.\n"
    "\n");
    printf(
    "\t--maxconn number (default: system maximum)\n"
    "\t\tSpecifies how many concurrent connections to accept.\n"
    "\n");
    printf(
    "\t--log filename (default: no logging)\n"
    "\t\tSpecifies which file to append the request log to.\n"
    "\n");
    printf(
    "\t--chroot (default: don't chroot)\n"
    "\t\tLocks server into wwwroot directory for added security.\n"
    "\n");
    printf(
    "\t--daemon (default: don't daemonize)\n"
    "\t\tDetach from the controlling terminal and run in the background.\n"
    "\n");
    printf(
    "\t--index filename (default: %s)\n" /* index_name */
    "\t\tDefault file to serve when a directory is requested.\n"
    "\n", index_name);
    printf(
    "\t--mimetypes filename (optional)\n"
    "\t\tParses specified file for extension-MIME associations.\n"
    "\n");
    printf(
    "\t--uid uid/uname, --gid gid/gname (default: don't privdrop)\n"
    "\t\tDrops privileges to given uid:gid after initialization.\n"
    "\n");
    printf(
    "\t--pidfile filename (default: no pidfile)\n"
    "\t\tWrite PID to the specified file.  Note that if you are\n"
    "\t\tusing --chroot, then the pidfile must be relative to,\n"
    "\t\tand inside the wwwroot."
    "\n");
    printf(
    "\t--help \n"
    "\t\tprints this dialogue.\n"
    "\n");
#ifdef __FreeBSD__
    printf(
    "\t--accf (default: don't use acceptfilter)\n"
    "\t\tUse acceptfilter.  Needs the accf_http module loaded.\n"
    "\n");
#endif
}


//Returns 1 if string is a number, 0 otherwise.  Set num to NULL if disinterested in value.
static int str_to_num(const char *str, int *num) {
    char *endptr;
    long l = strtol(str, &endptr, 10);
    if (*endptr != '\0') return 0;

    if (num != NULL) *num = (int)l;
    return 1;
}


//Parses commandline options.
static void parse_commandline(const int argc, char *argv[]) {
    int i;

    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
            usage();
            exit(EXIT_FAILURE);
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { 
            printf("%s \n", rcsid);
            exit(EXIT_FAILURE);
    }
    
    if (argc < 2) {
    wwwroot = getcwd(NULL, PATH_MAX);
    } 
    else {
    wwwroot = xstrdup(argv[1]); 
    } 
    
    puts(wwwroot);
    
    /* Strip ending slash. */
    if (wwwroot[strlen(wwwroot)-1] == '/') wwwroot[strlen(wwwroot)-1] = '\0';

    /* walk through the remainder of the arguments (if any) */
    for (i=2; i<argc; i++)
    {
        if (strcmp(argv[i], "--port") == 0)
        {
            if (++i >= argc) errx(1, "missing number after --port");
            bindport = (unsigned short)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--addr") == 0)
        {
            if (++i >= argc) errx(1, "missing ip after --addr");
            bindaddr = inet_addr(argv[i]);
            if (bindaddr == (in_addr_t)INADDR_NONE)
                errx(1, "malformed --addr argument");
        }
        else if (strcmp(argv[i], "--maxconn") == 0)
        {
            if (++i >= argc) errx(1, "missing number after --maxconn");
            max_connections = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--log") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --log");
            logfile_name = argv[i];
        }
        else if (strcmp(argv[i], "--chroot") == 0)
        {
            want_chroot = 1;
        }
        else if (strcmp(argv[i], "--daemon") == 0)
        {
            want_daemon = 1;
        }
        else if (strcmp(argv[i], "--index") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --index");
            index_name = argv[i];
        }
        else if (strcmp(argv[i], "--mimetypes") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --mimetypes");
            parse_extension_map_file(argv[i]);
        }
        else if (strcmp(argv[i], "--uid") == 0)
        {
            struct passwd *p;
            int num;
            if (++i >= argc) errx(1, "missing uid after --uid");
            p = getpwnam(argv[i]);
            if ((p == NULL) && (str_to_num(argv[i], &num)))
                p = getpwuid( (uid_t)num );

            if (p == NULL) errx(1, "no such uid: `%s'", argv[i]);
            drop_uid = p->pw_uid;
        }
        else if (strcmp(argv[i], "--gid") == 0)
        {
            struct group *g;
            int num;
            if (++i >= argc) errx(1, "missing gid after --gid");
            g = getgrnam(argv[i]);
            if ((g == NULL) && (str_to_num(argv[i], &num)))
                g = getgrgid( (gid_t)num );

            if (g == NULL) errx(1, "no such gid: `%s'", argv[i]);
            drop_gid = g->gr_gid;
        }
        else if (strcmp(argv[i], "--pidfile") == 0)
        {
            if (++i >= argc)
                errx(1, "missing filename after --pidfile");
            pidfile_name = argv[i];
        }
        else if (strcmp(argv[i], "--accf") == 0)
        {
            want_accf = 1;
        }
        else
            errx(1, "unknown argument `%s'", argv[i]);
    }
}



/* ---------------------------------------------------------------------------
 * Allocate and initialize an empty connection.
 */
static struct connection *new_connection(void)
{
    struct connection *conn = xmalloc(sizeof(struct connection));

    conn->socket = -1;
    conn->client = INADDR_ANY;
    conn->last_active = now;
    conn->request = NULL;
    conn->request_length = 0;
    conn->method = NULL;
    conn->uri = NULL;
    conn->referer = NULL;
    conn->user_agent = NULL;
    conn->range_begin = 0;
    conn->range_end = 0;
    conn->range_begin_given = 0;
    conn->range_end_given = 0;
    conn->header = NULL;
    conn->header_length = 0;
    conn->header_sent = 0;
    conn->header_dont_free = 0;
    conn->header_only = 0;
    conn->http_code = 0;
    conn->conn_close = 1;
    conn->reply = NULL;
    conn->reply_dont_free = 0;
    conn->reply_fd = -1;
    conn->reply_start = 0;
    conn->reply_length = 0;
    conn->reply_sent = 0;
    conn->total_sent = 0;

    /* Make it harmless so it gets garbage-collected if it should, for some
     * reason, fail to be correctly filled out.
     */
    conn->state = DONE;

    return conn;
}


//Accept a connection from sockin and add it to the connection queue.
static void accept_connection(void)
{
    struct sockaddr_in addrin;
    socklen_t sin_size;
    struct connection *conn;

    /* allocate and initialise struct connection */
    conn = new_connection();

    sin_size = sizeof(addrin);
    memset(&addrin, 0, sin_size);
    conn->socket = accept(sockin, (struct sockaddr *)&addrin,
            &sin_size);
    if (conn->socket == -1) err(1, "accept()");

    nonblock_socket(conn->socket);

    conn->state = RECV_REQUEST;
    conn->client = addrin.sin_addr.s_addr;
    LIST_INSERT_HEAD(&connlist, conn, entries);

    if (debug) printf("accepted connection from %s:%u\n",
        inet_ntoa(addrin.sin_addr),
        ntohs(addrin.sin_port) );

    /* try to read straight away rather than going through another iteration
     * of the select() loop.
     */
    poll_recv_request(conn);
}


static void log_connection(const struct connection *conn);


// Log a connection, then cleanly deallocate its internals.
static void free_connection(struct connection *conn) {
    if (debug) printf("free_connection(%d)\n", conn->socket);
    log_connection(conn);
    if (conn->socket != -1) xclose(conn->socket);
    if (conn->request != NULL) free(conn->request);
    if (conn->method != NULL) free(conn->method);
    if (conn->uri != NULL) free(conn->uri);
    if (conn->referer != NULL) free(conn->referer);
    if (conn->user_agent != NULL) free(conn->user_agent);
    if (conn->header != NULL && !conn->header_dont_free)
        free(conn->header);
    if (conn->reply != NULL && !conn->reply_dont_free) free(conn->reply);
    if (conn->reply_fd != -1) xclose(conn->reply_fd);
}


// Recycle a finished connection for HTTP/1.1 Keep-Alive.
static void recycle_connection(struct connection *conn) {
    int socket_tmp = conn->socket;
    if (debug) printf("recycle_connection(%d)\n", socket_tmp);
    conn->socket = -1; /* so free_connection() doesn't close it */
    free_connection(conn);
    conn->socket = socket_tmp;

    /* don't reset conn->client */
    conn->request = NULL;
    conn->request_length = 0;
    conn->method = NULL;
    conn->uri = NULL;
    conn->referer = NULL;
    conn->user_agent = NULL;
    conn->range_begin = 0;
    conn->range_end = 0;
    conn->range_begin_given = 0;
    conn->range_end_given = 0;
    conn->header = NULL;
    conn->header_length = 0;
    conn->header_sent = 0;
    conn->header_dont_free = 0;
    conn->header_only = 0;
    conn->http_code = 0;
    conn->conn_close = 1;
    conn->reply = NULL;
    conn->reply_dont_free = 0;
    conn->reply_fd = -1;
    conn->reply_start = 0;
    conn->reply_length = 0;
    conn->reply_sent = 0;
    conn->total_sent = 0;
    
    conn->state = RECV_REQUEST; /* ready for another */
}



// Uppercasify all characters in a string of given length.
static void strntoupper(char *str, const size_t length)
{
    size_t i;
    for (i=0; i<length; i++)
        str[i] = toupper(str[i]);
}



/* ---------------------------------------------------------------------------
 * If a connection has been idle for more than idletime seconds, it will be
 * marked as DONE and killed off in httpd_poll()
 */
static void poll_check_timeout(struct connection *conn)
{
    if (idletime > 0) /* optimised away by compiler */
    {
        if (now - conn->last_active >= idletime)
        {
            if (debug) printf("poll_check_timeout(%d) caused closure\n",
                conn->socket);
            conn->conn_close = 1;
            conn->state = DONE;
        }
    }
}



/* ---------------------------------------------------------------------------
 * Format [when] as an RFC1123 date, stored in the specified buffer.  The same
 * buffer is returned for convenience.
 */
#define DATE_LEN 30 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT")+1 */
static char *rfc1123_date(char *dest, const time_t when)
{
    time_t when_copy = when;
    if (strftime(dest, DATE_LEN,
        "%a, %d %b %Y %H:%M:%S GMT", gmtime(&when_copy) ) == 0)
            errx(1, "strftime() failed [%s]", dest);
    return dest;
}



/* ---------------------------------------------------------------------------
 * Decode URL by converting %XX (where XX are hexadecimal digits) to the
 * character it represents.  Don't forget to free the return value.
 */
static char *urldecode(const char *url)
{
    size_t i, len = strlen(url);
    char *out = xmalloc(len+1);
    int pos;

    for (i=0, pos=0; i<len; i++)
    {
        if (url[i] == '%' && i+2 < len &&
            isxdigit(url[i+1]) && isxdigit(url[i+2]))
        {
            /* decode %XX */
            #define HEX_TO_DIGIT(hex) ( \
                ((hex) >= 'A' && (hex) <= 'F') ? ((hex)-'A'+10): \
                ((hex) >= 'a' && (hex) <= 'f') ? ((hex)-'a'+10): \
                ((hex)-'0') )

            out[pos++] = HEX_TO_DIGIT(url[i+1]) * 16 +
                         HEX_TO_DIGIT(url[i+2]);
            i += 2;

            #undef HEX_TO_DIGIT
        }
        else
        {
            /* straight copy */
            out[pos++] = url[i];
        }
    }
    out[pos] = '\0';
    return (out);
}



/* ---------------------------------------------------------------------------
 * A default reply for any (erroneous) occasion.
 */
static void default_reply(struct connection *conn,
    const int errcode, const char *errname, const char *format, ...)
{
    char *reason, date[DATE_LEN];
    va_list va;

    va_start(va, format);
    xvasprintf(&reason, format, va);
    va_end(va);

    /* Only really need to calculate the date once. */
    rfc1123_date(date, now);

    conn->reply_length = xasprintf(&(conn->reply),
     "<html><head><title>%d %s</title></head><body>\n"
     "<h1>%s</h1>\n" /* errname */
     "%s\n" /* reason */
     "<hr>\n"
     "Generated by %s on %s\n"
     "</body></html>\n",
     errcode, errname, errname, reason, pkgname, date);
    free(reason);

    conn->header_length = xasprintf(&(conn->header),
     "HTTP/1.1 %d %s\r\n"
     "Date: %s\r\n"
     "Server: %s\r\n"
     "%s" /* keep-alive */
     "Content-Length: %d\r\n"
     "Content-Type: text/html\r\n"
     "\r\n",
     errcode, errname, date, pkgname, keep_alive(conn),
     conn->reply_length);

    conn->reply_type = REPLY_GENERATED;
    conn->http_code = errcode;
}



/* ---------------------------------------------------------------------------
 * Redirection.
 */
static void redirect(struct connection *conn, const char *format, ...)
{
    char *where, date[DATE_LEN];
    va_list va;

    va_start(va, format);
    xvasprintf(&where, format, va);
    va_end(va);

    /* Only really need to calculate the date once. */
    rfc1123_date(date, now);

    conn->reply_length = xasprintf(&(conn->reply),
     "<html><head><title>301 Moved Permanently</title></head><body>\n"
     "<h1>Moved Permanently</h1>\n"
     "Moved to: <a href=\"%s\">%s</a>\n" /* where x 2 */
     "<hr>\n"
     "Generated by %s on %s\n"
     "</body></html>\n",
     where, where, pkgname, date);

    conn->header_length = xasprintf(&(conn->header),
     "HTTP/1.1 301 Moved Permanently\r\n"
     "Date: %s\r\n"
     "Server: %s\r\n"
     "Location: %s\r\n"
     "%s" /* keep-alive */
     "Content-Length: %d\r\n"
     "Content-Type: text/html\r\n"
     "\r\n",
     date, pkgname, where, keep_alive(conn), conn->reply_length);

    free(where);
    conn->reply_type = REPLY_GENERATED;
    conn->http_code = 301;
}



/* ---------------------------------------------------------------------------
 * Parses a single HTTP request field.  Returns string from end of [field] to
 * first \r, \n or end of request string.  Returns NULL if [field] can't be
 * matched.
 *
 * You need to remember to deallocate the result.
 * example: parse_field(conn, "Referer: ");
 */
static char *parse_field(const struct connection *conn, const char *field)
{
    size_t bound1, bound2;
    char *pos;

    /* find start */
    pos = strstr(conn->request, field);
    if (pos == NULL) return NULL;
    bound1 = pos - conn->request + strlen(field);

    /* find end */
    for (bound2 = bound1;
        conn->request[bound2] != '\r' &&
        bound2 < conn->request_length; bound2++)
            ;

    /* copy to buffer */
    return split_string(conn->request, bound1, bound2);
}



/* ---------------------------------------------------------------------------
 * Parse a Range: field into range_begin and range_end.  Only handles the
 * first range if a list is given.  Sets range_{begin,end}_given to 1 if
 * either part of the range is given.
 */
static void parse_range_field(struct connection *conn)
{
    size_t bound1, bound2, len;
    char *range;

    range = parse_field(conn, "Range: bytes=");
    if (range == NULL) return;
    len = strlen(range);

    do /* break handling */
    {
        /* parse number up to hyphen */
        bound1 = 0;
        for (bound2=0;
            isdigit( (int)range[bound2] ) && bound2 < len;
            bound2++)
                ;

        if (bound2 == len || range[bound2] != '-')
            break; /* there must be a hyphen here */

        if (bound1 != bound2)
        {
            conn->range_begin_given = 1;
            conn->range_begin = (size_t)strtol(range+bound1, NULL, 10);

        }

        /* parse number after hyphen */
        bound2++;
        for (bound1=bound2;
            isdigit( (int)range[bound2] ) && bound2 < len;
            bound2++)
                ;

        if (bound2 != len && range[bound2] != ',')
            break; /* must be end of string or a list to be valid */

        if (bound1 != bound2)
        {
            conn->range_end_given = 1;
            conn->range_end = (size_t)strtol(range+bound1, NULL, 10);
        }
    }
    while(0); /* break handling */
    free(range);

    /* sanity check: begin <= end */
    if (conn->range_begin_given && conn->range_end_given &&
        (conn->range_begin > conn->range_end))
    {
        conn->range_begin_given = conn->range_end_given = 0;
    }
}



/* ---------------------------------------------------------------------------
 * Parse an HTTP request like "GET / HTTP/1.1" to get the method (GET), the
 * url (/), the referer (if given) and the user-agent (if given).  Remember to
 * deallocate all these buffers.  The method will be returned in uppercase.
 */
static int parse_request(struct connection *conn)
{
    size_t bound1, bound2;
    char *tmp;
    assert(conn->request_length == strlen(conn->request));

    /* parse method */
    for (bound1 = 0; bound1 < conn->request_length &&
        conn->request[bound1] != ' '; bound1++)
            ;

    conn->method = split_string(conn->request, 0, bound1);
    strntoupper(conn->method, bound1);

    /* parse uri */
    for (; bound1 < conn->request_length &&
        conn->request[bound1] == ' '; bound1++)
            ;

    if (bound1 == conn->request_length) return 0; /* fail */

    for (bound2=bound1+1; bound2 < conn->request_length &&
        conn->request[bound2] != ' ' &&
        conn->request[bound2] != '\r'; bound2++)
            ;

    conn->uri = split_string(conn->request, bound1, bound2);

    /* parse protocol to determine conn_close */
    if (conn->request[bound2] == ' ')
    {
        char *proto;
        for (bound1 = bound2; bound1 < conn->request_length &&
            conn->request[bound1] == ' '; bound1++)
                ;

        for (bound2=bound1+1; bound2 < conn->request_length &&
            conn->request[bound2] != ' ' &&
            conn->request[bound2] != '\r'; bound2++)
                ;

        proto = split_string(conn->request, bound1, bound2);
        if (strcasecmp(proto, "HTTP/1.1") == 0) conn->conn_close = 0;
        free(proto);
    }

    /* parse connection field */
    tmp = parse_field(conn, "Connection: ");
    if (tmp != NULL)
    {
        if (strcasecmp(tmp, "close") == 0) conn->conn_close = 1;
        else if (strcasecmp(tmp, "keep-alive") == 0) conn->conn_close = 0;
        free(tmp);
    }

    /* parse important fields */
    conn->referer = parse_field(conn, "Referer: ");
    conn->user_agent = parse_field(conn, "User-Agent: ");
    parse_range_field(conn);
    return 1;
}



/* ---------------------------------------------------------------------------
 * Check if a file exists.
 */
static int file_exists(const char *path)
{
    struct stat filestat;
    if ((stat(path, &filestat) == -1) && (errno = ENOENT))
        return 0;
    else
        return 1;
}



/* ---------------------------------------------------------------------------
 * Make sorted list of files in a directory.  Returns number of entries, or -1
 * if error occurs.
 */
struct dlent
{
    char *name;
    int is_dir;
    off_t size;
};

static int dlent_cmp(const void *a, const void *b)
{
    return strcmp( (*((const struct dlent * const *)a))->name,
                   (*((const struct dlent * const *)b))->name );
}

static ssize_t make_sorted_dirlist(const char *path, struct dlent ***output)
{
    DIR *dir;
    struct dirent *ent;
    size_t entries = 0, pool = 0;
    #define POOL_INCR 100
    char *currname;
    struct dlent **list = NULL;

    dir = opendir(path);
    if (dir == NULL) return -1;

    currname = xmalloc(strlen(path) + MAXNAMLEN + 1);

    /* construct list */
    while ((ent = readdir(dir)) != NULL)
    {
        struct stat s;

        if (ent->d_name[0] == '.' && ent->d_name[1] == '\0')
            continue; /* skip "." */
        assert(strlen(ent->d_name) <= MAXNAMLEN);
        sprintf(currname, "%s%s", path, ent->d_name);
        if (stat(currname, &s) == -1)
            continue; /* skip un-stat-able files */

        if (entries == pool)
        {
            pool += POOL_INCR;
            list = xrealloc(list, sizeof(struct dlent*) * pool);
        }

        list[entries] = xmalloc(sizeof(struct dlent));
        list[entries]->name = xstrdup(ent->d_name);
        list[entries]->is_dir = S_ISDIR(s.st_mode);
        list[entries]->size = s.st_size;
        entries++;
    }

    (void)closedir(dir); /* can't error out if opendir() succeeded */

    free(currname);
    qsort(list, entries, sizeof(struct dlent*), dlent_cmp);
    *output = xrealloc(list, sizeof(struct dlent*) * entries);
    return entries;
    #undef POOL_INCR
}



/* ---------------------------------------------------------------------------
 * Cleanly deallocate a sorted list of directory files.
 */
static void cleanup_sorted_dirlist(struct dlent **list, const ssize_t size)
{
    ssize_t i;
    for (i=0; i<size; i++)
    {
        free(list[i]->name);
        free(list[i]);
    }
}

/* ---------------------------------------------------------------------------
 * Should this character be urlencoded (according to rfc1738)
 */
static int needs_urlencoding(unsigned char c)
{
    int i;
    static const char bad[] = "<>\"%{}|^~[]`\\;:/?@#=&";

    for (i=0; i<sizeof(bad)-1; i++)
        if (c == bad[i])
            return 1;

    /* Non-US-ASCII characters */
    if ((c <= 0x1F) || (c >= 0x80) || (c == 0x7F))
        return 1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Encode filename to be an rfc1738-compliant URL part
 */
static void urlencode_filename( char *name, char *safe_url)
{
    static const char hex[] = "0123456789ABCDEF";
    int i, j;

    for (i = j = 0; name[i] != '\0'; i++)
    {
        if (needs_urlencoding(name[i]))
        {
            safe_url[j++] = '%';
            safe_url[j++] = hex[(name[i] >> 4) & 0xF];
            safe_url[j++] = hex[ name[i]       & 0xF];
        }
        else
            safe_url[j++] = name[i];
    }

    safe_url[j] = '\0';
}

/* ---------------------------------------------------------------------------
 * Generate directory listing.
 */
static void generate_dir_listing(struct connection *conn, const char *path)
{
    char date[DATE_LEN], *spaces;
    struct dlent **list;
    ssize_t listsize;
    size_t maxlen = 0;
    int i;
    struct apbuf *listing = make_apbuf();

    listsize = make_sorted_dirlist(path, &list);
    if (listsize == -1)
    {
        default_reply(conn, 500, "Internal Server Error",
            "Couldn't list directory: %s", strerror(errno));
        return;
    }

    for (i=0; i<listsize; i++)
    {
        size_t tmp = strlen(list[i]->name);
        if (maxlen < tmp) maxlen = tmp;
    }

    append(listing, "<html>\n<head>\n <title>");
    append(listing, conn->uri);
    append(listing, "</title>\n</head>\n<body>\n<h1>");
    append(listing, conn->uri);
    append(listing, "</h1>\n<tt><pre>\n");

    spaces = xmalloc(maxlen);
    memset(spaces, ' ', maxlen);

    for (i=0; i<listsize; i++)
    {
        /* If a filename is made up of entirely unsafe chars,
         * the url would be three times its original length.
         */
        char safe_url[MAXNAMLEN*3 + 1];

        urlencode_filename(list[i]->name, safe_url);

        append(listing, "<a href=\"");
        append(listing, safe_url);
        append(listing, "\">");
        append(listing, list[i]->name);
        append(listing, "</a>");

        if (list[i]->is_dir)
            append(listing, "/\n");
        else
        {
            appendl(listing, spaces, maxlen-strlen(list[i]->name));
            appendf(listing, "%10d\n", list[i]->size);
        }
    }

    cleanup_sorted_dirlist(list, listsize);
    free(list);
    free(spaces);

    rfc1123_date(date, now);
    append(listing,
     "</pre></tt>\n"
     "<hr>\n"
     "Generated by ");
    append(listing, pkgname);
    append(listing, " on ");
    append(listing, date);
    append(listing, "\n</body>\n</html>\n");

    conn->reply = listing->str;
    conn->reply_length = listing->length;
    free(listing); /* don't free inside of listing */

    conn->header_length = xasprintf(&(conn->header),
     "HTTP/1.1 200 OK\r\n"
     "Date: %s\r\n"
     "Server: %s\r\n"
     "%s" /* keep-alive */
     "Content-Length: %u\r\n"
     "Content-Type: text/html\r\n"
     "\r\n",
     date, pkgname, keep_alive(conn), conn->reply_length);

    conn->reply_type = REPLY_GENERATED;
    conn->http_code = 200;
}



/* ---------------------------------------------------------------------------
 * Process a GET/HEAD request
 */
static void process_get(struct connection *conn)
{
    char *decoded_url, *target, *if_mod_since;
    char date[DATE_LEN], lastmod[DATE_LEN];
    const char *mimetype = NULL;
    struct stat filestat;

    /* work out path of file being requested */
    decoded_url = urldecode(conn->uri);

    /* make sure it's safe */
    if (make_safe_uri(decoded_url) == NULL) {
        default_reply(conn, 400, "Bad Request",
            "You requested an invalid URI: %s", conn->uri);
        free(decoded_url);
        return;
    }

    /* does it end in a slash? serve up url/index_name */
    if (decoded_url[strlen(decoded_url)-1] == '/')
    {
        xasprintf(&target, "%s%s%s", wwwroot, decoded_url, index_name);
        if (!file_exists(target))
        {
            free(target);
            xasprintf(&target, "%s%s", wwwroot, decoded_url);
            generate_dir_listing(conn, target);
            free(target);
            free(decoded_url);
            return;
        }
        mimetype = uri_content_type(index_name);
    }
    else /* points to a file */
    {
        xasprintf(&target, "%s%s", wwwroot, decoded_url);
        mimetype = uri_content_type(decoded_url);
    }
    free(decoded_url);
    if (debug) printf("uri=%s, target=%s, content-type=%s\n",
        conn->uri, target, mimetype);

    /* open file */
    conn->reply_fd = open(target, O_RDONLY | O_NONBLOCK);
    free(target);

    if (conn->reply_fd == -1)
    {
        /* open() failed */
        if (errno == EACCES)
            default_reply(conn, 403, "Forbidden",
                "You don't have permission to access (%s).", conn->uri);
        else if (errno == ENOENT)
            default_reply(conn, 404, "Not Found",
                "The URI you requested (%s) was not found.", conn->uri);
        else
            default_reply(conn, 500, "Internal Server Error",
                "The URI you requested (%s) cannot be returned: %s.",
                conn->uri, strerror(errno));

        return;
    }

    /* stat the file */
    if (fstat(conn->reply_fd, &filestat) == -1)
    {
        default_reply(conn, 500, "Internal Server Error",
            "fstat() failed: %s.", strerror(errno));
        return;
    }

    /* make sure it's a regular file */
    if (S_ISDIR(filestat.st_mode))
    {
        redirect(conn, "%s/", conn->uri);
        return;
    }
    else if (!S_ISREG(filestat.st_mode))
    {
        default_reply(conn, 403, "Forbidden", "Not a regular file.");
        return;
    }

    conn->reply_type = REPLY_FROMFILE;
    (void) rfc1123_date(lastmod, filestat.st_mtime);

    /* check for If-Modified-Since, may not have to send */
    if_mod_since = parse_field(conn, "If-Modified-Since: ");
    if (if_mod_since != NULL &&
        strcmp(if_mod_since, lastmod) == 0)
    {
        if (debug) printf("not modified since %s\n", if_mod_since);
        default_reply(conn, 304, "Not Modified", "");
        conn->header_only = 1;
        free(if_mod_since);
        return;
    }
    free(if_mod_since);

    if (conn->range_begin_given || conn->range_end_given)
    {
        size_t from, to;

        if (conn->range_begin_given && conn->range_end_given)
        {
            /* 100-200 */
            from = conn->range_begin;
            to = conn->range_end;

            /* clamp [to] to filestat.st_size-1 */
            if (to > (size_t)(filestat.st_size-1))
                to = filestat.st_size-1;
        }
        else if (conn->range_begin_given && !conn->range_end_given)
        {
            /* 100- :: yields 100 to end */
            from = conn->range_begin;
            to = filestat.st_size-1;
        }
        else if (!conn->range_begin_given && conn->range_end_given)
        {
            /* -200 :: yields last 200 */
            to = filestat.st_size-1;
            from = to - conn->range_end + 1;

            /* check for wrapping */
            if (from > to) from = 0;
        }
        else errx(1, "internal error - from/to mismatch");

        conn->reply_start = from;
        conn->reply_length = to - from + 1;

        conn->header_length = xasprintf(&(conn->header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Date: %s\r\n"
            "Server: %s\r\n"
            "%s" /* keep-alive */
            "Content-Length: %d\r\n"
            "Content-Range: bytes %d-%d/%d\r\n"
            "Content-Type: %s\r\n"
            "Last-Modified: %s\r\n"
            "\r\n"
            ,
            rfc1123_date(date, now), pkgname, keep_alive(conn),
            conn->reply_length, from, to, filestat.st_size,
            mimetype, lastmod
        );
        conn->http_code = 206;
        if (debug) printf("sending %u-%u/%u\n",
            (unsigned int)from, (unsigned int)to,
            (unsigned int)filestat.st_size);
    }
    else /* no range stuff */
    {
        conn->reply_length = filestat.st_size;

        conn->header_length = xasprintf(&(conn->header),
            "HTTP/1.1 200 OK\r\n"
            "Date: %s\r\n"
            "Server: %s\r\n"
            "%s" /* keep-alive */
            "Content-Length: %d\r\n"
            "Content-Type: %s\r\n"
            "Last-Modified: %s\r\n"
            "\r\n"
            ,
            rfc1123_date(date, now), pkgname, keep_alive(conn),
            conn->reply_length, mimetype, lastmod
        );
        conn->http_code = 200;
    }
}



/* ---------------------------------------------------------------------------
 * Process a request: build the header and reply, advance state.
 */
static void process_request(struct connection *conn)
{
    num_requests++;
    if (!parse_request(conn))
    {
        default_reply(conn, 400, "Bad Request",
            "You sent a request that the server couldn't understand.");
    }
    else if (strcmp(conn->method, "GET") == 0)
    {
        process_get(conn);
    }
    else if (strcmp(conn->method, "HEAD") == 0)
    {
        process_get(conn);
        conn->header_only = 1;
    }
    else if (strcmp(conn->method, "OPTIONS") == 0 ||
             strcmp(conn->method, "POST") == 0 ||
             strcmp(conn->method, "PUT") == 0 ||
             strcmp(conn->method, "DELETE") == 0 ||
             strcmp(conn->method, "TRACE") == 0 ||
             strcmp(conn->method, "CONNECT") == 0)
    {
        default_reply(conn, 501, "Not Implemented",
            "The method you specified (%s) is not implemented.",
            conn->method);
    }
    else
    {
        default_reply(conn, 400, "Bad Request",
            "%s is not a valid HTTP/1.1 method.", conn->method);
    }

    /* advance state */
    conn->state = SEND_HEADER;

    /* request not needed anymore */
    free(conn->request);
    conn->request = NULL; /* important: don't free it again later */
}



/* ---------------------------------------------------------------------------
 * Receiving request.
 */
static void poll_recv_request(struct connection *conn)
{
    #define BUFSIZE 65536
    char buf[BUFSIZE];
    ssize_t recvd;

    assert(conn->state == RECV_REQUEST);
    recvd = recv(conn->socket, buf, BUFSIZE, 0);
    if (debug) printf("poll_recv_request(%d) got %d bytes\n",
        conn->socket, (int)recvd);
    if (recvd <= 0)
    {
        if (recvd == -1) {
            if (errno == EAGAIN) {
                if (debug) printf("poll_recv_request would have blocked\n");
                return;
            }
            if (debug) printf("recv(%d) error: %s\n",
                conn->socket, strerror(errno));
        }
        conn->conn_close = 1;
        conn->state = DONE;
        return;
    }
    conn->last_active = now;
    #undef BUFSIZE

    /* append to conn->request */
    conn->request = xrealloc(conn->request, conn->request_length+recvd+1);
    memcpy(conn->request+conn->request_length, buf, (size_t)recvd);
    conn->request_length += recvd;
    conn->request[conn->request_length] = 0;
    total_in += recvd;

    /* process request if we have all of it */
    if ((conn->request_length > 2) &&
        (memcmp(conn->request+conn->request_length-2, "\n\n", 2) == 0))
            process_request(conn);
    else if ((conn->request_length > 4) &&
        (memcmp(conn->request+conn->request_length-4, "\r\n\r\n", 4) == 0))
            process_request(conn);

    /* die if it's too long */
    if (conn->request_length > MAX_REQUEST_LENGTH)
    {
        default_reply(conn, 413, "Request Entity Too Large",
            "Your request was dropped because it was too long.");
        conn->state = SEND_HEADER;
    }

    /* if we've moved on to the next state, try to send right away, instead of
     * going through another iteration of the select() loop.
     */
    if (conn->state == SEND_HEADER)
        poll_send_header(conn);
}



/* ---------------------------------------------------------------------------
 * Sending header.  Assumes conn->header is not NULL.
 */
static void poll_send_header(struct connection *conn)
{
    ssize_t sent;

    assert(conn->state == SEND_HEADER);
    assert(conn->header_length == strlen(conn->header));

    sent = send(conn->socket, conn->header + conn->header_sent,
        conn->header_length - conn->header_sent, 0);
    conn->last_active = now;
    if (debug) printf("poll_send_header(%d) sent %d bytes\n",
        conn->socket, (int)sent);

    /* handle any errors (-1) or closure (0) in send() */
    if (sent < 1)
    {
        if ((sent == -1) && (errno == EAGAIN)) {
            if (debug) printf("poll_send_header would have blocked\n");
            return;
        }
        if (debug && (sent == -1))
            printf("send(%d) error: %s\n", conn->socket, strerror(errno));
        conn->conn_close = 1;
        conn->state = DONE;
        return;
    }
    conn->header_sent += sent;
    conn->total_sent += sent;
    total_out += sent;

    /* check if we're done sending header */
    if (conn->header_sent == conn->header_length)
    {
        if (conn->header_only)
            conn->state = DONE;
        else {
            conn->state = SEND_REPLY;
            /* go straight on to body, don't go through another iteration of
             * the select() loop.
             */
            poll_send_reply(conn);
        }
    }
}



/* ---------------------------------------------------------------------------
 * Send chunk on socket <s> from FILE *fp, starting at <ofs> and of size
 * <size>.  Use sendfile() if possible since it's zero-copy on some platforms.
 * Returns the number of bytes sent, 0 on closure, -1 if send() failed, -2 if
 * read error.
 */
static ssize_t send_from_file(const int s, const int fd,
    off_t ofs, const size_t size)
{
#ifdef __FreeBSD__
    off_t sent;
    int ret = sendfile(fd, s, ofs, size, NULL, &sent, 0);

    /* It is possible for sendfile to send zero bytes due to a blocking
     * condition.  Handle this correctly.
     */
    if (ret == -1)
        if (errno == EAGAIN)
            if (sent == 0)
                return -1;
            else
                return sent;
        else
            return -1;
    else
        return size;
#else
#if defined(__linux) || defined(__sun__)
    return sendfile(s, fd, &ofs, size);
#else
    #define BUFSIZE 20000
    char buf[BUFSIZE];
    size_t amount = min((size_t)BUFSIZE, size);
    ssize_t numread;
    #undef BUFSIZE

    if (lseek(fd, ofs, SEEK_SET) == -1) err(1, "fseek(%d)", (int)ofs);
    numread = read(fd, buf, amount);
    if (numread == 0)
    {
        fprintf(stderr, "premature eof on fd %d\n", fd);
        return -1;
    }
    else if (numread == -1)
    {
        fprintf(stderr, "error reading on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    else if ((size_t)numread != amount)
    {
        fprintf(stderr, "read %d bytes, expecting %u bytes on fd %d\n",
            numread, amount, fd);
        return -1;
    }
    else
        return send(s, buf, amount, 0);
#endif
#endif
}



/* ---------------------------------------------------------------------------
 * Sending reply.
 */
static void poll_send_reply(struct connection *conn)
{
    ssize_t sent;

    assert(conn->state == SEND_REPLY);
    assert(!conn->header_only);
    if (conn->reply_type == REPLY_GENERATED)
    {
        sent = send(conn->socket,
            conn->reply + conn->reply_start + conn->reply_sent,
            conn->reply_length - conn->reply_sent, 0);
    }
    else
    {
        sent = send_from_file(conn->socket, conn->reply_fd,
            (off_t)(conn->reply_start + conn->reply_sent),
            conn->reply_length - conn->reply_sent);
    }
    conn->last_active = now;
    if (debug) printf("poll_send_reply(%d) sent %d: %d+[%d-%d] of %d\n",
        conn->socket, (int)sent, (int)conn->reply_start,
        (int)conn->reply_sent,
        (int)(conn->reply_sent + sent - 1),
        (int)conn->reply_length);

    /* handle any errors (-1) or closure (0) in send() */
    if (sent < 1)
    {
        if (sent == -1)
        {
            if (errno == EAGAIN) {
                if (debug) printf("poll_send_reply would have blocked\n");
                return;
            }
            if (debug) printf("send(%d) error: %s\n",
                conn->socket, strerror(errno));
        }
        else if (sent == 0)
        {
            if (debug) printf("send(%d) closure\n", conn->socket);
        }
        conn->conn_close = 1;
        conn->state = DONE;
        return;
    }
    conn->reply_sent += (unsigned int)sent;
    conn->total_sent += (unsigned int)sent;
    total_out += sent;

    /* check if we're done sending */
    if (conn->reply_sent == conn->reply_length) conn->state = DONE;
}



/* ---------------------------------------------------------------------------
 * Add a connection's details to the logfile.
 */
static void log_connection(const struct connection *conn)
{
    struct in_addr inaddr;

    if (logfile == NULL)
        return;
    if (conn->http_code == 0)
        return; /* invalid - died in request */
    if (conn->method == NULL)
        return; /* invalid - didn't parse - maybe too long */

    /* Separated by tabs:
     * time client_ip method uri http_code bytes_sent "referer" "user-agent"
     */

    inaddr.s_addr = conn->client;

    fprintf(logfile, "%lu\t%s\t%s\t%s\t%d\t%u\t\"%s\"\t\"%s\"\n",
        (unsigned long int)now, inet_ntoa(inaddr),
        conn->method, conn->uri,
        conn->http_code, conn->total_sent,
        (conn->referer == NULL)?"":conn->referer,
        (conn->user_agent == NULL)?"":conn->user_agent
        );
    fflush(logfile);
}



/* ---------------------------------------------------------------------------
 * Main loop of the httpd - a select() and then delegation to accept
 * connections, handle receiving of requests, and sending of replies.
 */
static void httpd_poll(void)
{
    fd_set recv_set, send_set;
    int max_fd, select_ret;
    struct connection *conn, *next;
    int bother_with_timeout = 0;
    struct timeval timeout;

    timeout.tv_sec = idletime;
    timeout.tv_usec = 0;

    FD_ZERO(&recv_set);
    FD_ZERO(&send_set);
    max_fd = 0;

    /* set recv/send fd_sets */
    #define MAX_FD_SET(sock, fdset) { FD_SET(sock,fdset); \
                                    max_fd = (max_fd<sock) ? sock : max_fd; }

    MAX_FD_SET(sockin, &recv_set);

    LIST_FOREACH_SAFE(conn, &connlist, entries, next)
    {
        poll_check_timeout(conn);
        switch (conn->state)
        {
        case DONE:
            /* do nothing */
            break;

        case RECV_REQUEST:
            MAX_FD_SET(conn->socket, &recv_set);
            bother_with_timeout = 1;
            break;

        case SEND_HEADER:
        case SEND_REPLY:
            MAX_FD_SET(conn->socket, &send_set);
            bother_with_timeout = 1;
            break;

        default: errx(1, "invalid state");
        }
    }
    #undef MAX_FD_SET

    /* -select- */
    select_ret = select(max_fd + 1, &recv_set, &send_set, NULL,
        (bother_with_timeout) ? &timeout : NULL);
    if (select_ret == 0)
    {
        if (!bother_with_timeout)
            errx(1, "select() timed out");
        else
            return;
    }
    if (select_ret == -1) {
        if (errno == EINTR)
            return; /* interrupted by signal */
        else
            err(1, "select() failed");
    }

    /* update time */
    now = time(NULL);

    /* poll connections that select() says need attention */
    if (FD_ISSET(sockin, &recv_set)) accept_connection();

    LIST_FOREACH_SAFE(conn, &connlist, entries, next)
    {
        switch (conn->state)
        {
        case RECV_REQUEST:
            if (FD_ISSET(conn->socket, &recv_set)) poll_recv_request(conn);
            break;

        case SEND_HEADER:
            if (FD_ISSET(conn->socket, &send_set)) poll_send_header(conn);
            break;

        case SEND_REPLY:
            if (FD_ISSET(conn->socket, &send_set)) poll_send_reply(conn);
            break;

        case DONE:
            /* (handled later; ignore for now as it's a valid state) */
            break;

        default: errx(1, "invalid state");
        }

        if (conn->state == DONE) {
            /* clean out finished connection */
            if (conn->conn_close) {
                LIST_REMOVE(conn, entries);
                free_connection(conn);
                free(conn);
            } else {
                recycle_connection(conn);
                /* and go right back to recv_request without going through
                 * select() again.
                 */
                poll_recv_request(conn);
            }
        }
    }
}



/* ---------------------------------------------------------------------------
 * Daemonize helpers.
 */
#define PATH_DEVNULL "/dev/null"
static int lifeline[2] = { -1, -1 };
static int fd_null = -1;

static void
daemonize_start(void)
{
   pid_t f, w;

   if (pipe(lifeline) == -1)
      err(1, "pipe(lifeline)");

   fd_null = open(PATH_DEVNULL, O_RDWR, 0);
   if (fd_null == -1)
      err(1, "open(" PATH_DEVNULL ")");

   f = fork();
   if (f == -1)
      err(1, "fork");
   else if (f != 0) {
      /* parent: wait for child */
      char tmp[1];
      int status;

      if (close(lifeline[1]) == -1)
         warn("close lifeline in parent");
      read(lifeline[0], tmp, sizeof(tmp));
      w = waitpid(f, &status, WNOHANG);
      if (w == -1)
         err(1, "waitpid");
      else if (w == 0)
         /* child is running happily */
         exit(EXIT_SUCCESS);
      else
         /* child init failed, pass on its exit status */
         exit(WEXITSTATUS(status));
   }
   /* else we are the child: continue initializing */
}

static void
daemonize_finish(void)
{
   if (fd_null == -1)
      return; /* didn't daemonize_start() so we're not daemonizing */

   if (setsid() == -1)
      err(1, "setsid");
   if (close(lifeline[0]) == -1)
      warn("close read end of lifeline in child");
   if (close(lifeline[1]) == -1)
      warn("couldn't cut the lifeline");

   /* close all our std fds */
   if (dup2(fd_null, STDIN_FILENO) == -1)
      warn("dup2(stdin)");
   if (dup2(fd_null, STDOUT_FILENO) == -1)
      warn("dup2(stdout)");
   if (dup2(fd_null, STDERR_FILENO) == -1)
      warn("dup2(stderr)");
   if (fd_null > 2)
      close(fd_null);
}

/* ---------------------------------------------------------------------------
 * Pidfile helpers, based on FreeBSD src/lib/libutil/pidfile.c,v 1.3
 * Original was copyright (c) 2005 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 */
static int pidfile_fd = -1;
#define PIDFILE_MODE 0600

static void
pidfile_remove(void)
{
    if (unlink(pidfile_name) == -1)
        err(1, "unlink(pidfile) failed");
 /* if (flock(pidfile_fd, LOCK_UN) == -1)
        err(1, "unlock(pidfile) failed"); */
    xclose(pidfile_fd);
    pidfile_fd = -1;
}

static int
pidfile_read(void)
{
    char buf[16], *endptr;
    int fd, i, pid;

    fd = open(pidfile_name, O_RDONLY);
    if (fd == -1)
        err(1, " after create failed");

    i = read(fd, buf, sizeof(buf) - 1);
    if (i == -1)
        err(1, "read from pidfile failed");
    xclose(fd);
    buf[i] = '\0';

    pid = (int)strtoul(buf, &endptr, 10);
    if (endptr != &buf[i])
        err(1, "invalid pidfile contents: \"%s\"", buf);
    return (pid);
}

static void
pidfile_create(void)
{
    int error, fd;
    char pidstr[16];

    /* Open the PID file and obtain exclusive lock. */
    fd = open(pidfile_name,
        O_WRONLY | O_CREAT | O_EXLOCK | O_TRUNC | O_NONBLOCK, PIDFILE_MODE);
    if (fd == -1) {
        if ((errno == EWOULDBLOCK) || (errno == EEXIST))
            errx(1, "daemon already running with PID %d", pidfile_read());
        else
            err(1, "can't create pidfile %s", pidfile_name);
    }
    pidfile_fd = fd;

    if (ftruncate(fd, 0) == -1) {
        error = errno;
        pidfile_remove();
        errno = error;
        err(1, "ftruncate() failed");
    }

    snprintf(pidstr, sizeof(pidstr), "%u", getpid());
    if (pwrite(fd, pidstr, strlen(pidstr), 0) != (ssize_t)strlen(pidstr)) {
        error = errno;
        pidfile_remove();
        errno = error;
        err(1, "pwrite() failed");
    }
}

/* end of pidfile helpers.
 * ---------------------------------------------------------------------------
 * Close all sockets and FILEs and exit.
 */
static void
stop_running(int sig)
{
    running = 0;
    fprintf(stderr, "\ncaught %s, stopping\n", strsignal(sig));
}

/* ---------------------------------------------------------------------------
 * Execution starts here.
 */
int
main(int argc, char **argv)
{
    printf("%s, %s.\n", pkgname, copyright);
    parse_default_extension_map();
    parse_commandline(argc, argv);
    /* parse_commandline() might override parts of the extension map by
     * parsing a user-specified file.
     */
    sort_mime_map();
    xasprintf(&keep_alive_field, "Keep-Alive: timeout=%d\r\n", idletime);
    init_sockin();

    /* open logfile */
    if (logfile_name != NULL)
    {
        logfile = fopen(logfile_name, "ab");
        if (logfile == NULL)
            err(1, "opening logfile: fopen(\"%s\")", logfile_name);
    }

    if (want_daemon) daemonize_start();

    /* signals */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        err(1, "signal(ignore SIGPIPE)");
    if (signal(SIGINT, stop_running) == SIG_ERR)
        err(1, "signal(SIGINT)");
    if (signal(SIGQUIT, stop_running) == SIG_ERR)
        err(1, "signal(SIGQUIT)");
    if (signal(SIGTERM, stop_running) == SIG_ERR)
        err(1, "signal(SIGTERM)");

    /* security */
    if (want_chroot)
    {
        tzset(); /* read /etc/localtime before we chroot */
        if (chdir(wwwroot) == -1)
            err(1, "chdir(%s)", wwwroot);
        if (chroot(wwwroot) == -1)
            err(1, "chroot(%s)", wwwroot);
        printf("chrooted to `%s'\n", wwwroot);
        wwwroot[0] = '\0'; /* empty string */
    }
    if (drop_gid != INVALID_GID)
    {
        if (setgid(drop_gid) == -1) err(1, "setgid(%d)", drop_gid);
        printf("set gid to %d\n", drop_gid);
    }
    if (drop_uid != INVALID_UID)
    {
        if (setuid(drop_uid) == -1) err(1, "setuid(%d)", drop_uid);
        printf("set uid to %d\n", drop_uid);
    }

    /* create pidfile */
    if (pidfile_name) pidfile_create();

    if (want_daemon) daemonize_finish();

    /* main loop */
    while (running) httpd_poll();

    /* clean exit */
    xclose(sockin);
    if (logfile != NULL) fclose(logfile);
    if (pidfile_name) pidfile_remove();

    /* close and free connections */
    {
        struct connection *conn, *next;

        LIST_FOREACH_SAFE(conn, &connlist, entries, next)
        {
            LIST_REMOVE(conn, entries);
            free_connection(conn);
            free(conn);
        }
    }

    /* free the mallocs */
    {
        size_t i;
        for (i=0; i<mime_map_size; i++)
        {
            free(mime_map[i].extension);
            free(mime_map[i].mimetype);
        }
        free(mime_map);
        free(keep_alive_field);
        free(wwwroot);
    }

    /* usage stats */
    {
        struct rusage r;

        getrusage(RUSAGE_SELF, &r);
        printf("CPU time used: %u.%02u user, %u.%02u system\n",
            (unsigned int)r.ru_utime.tv_sec,
                (unsigned int)(r.ru_utime.tv_usec/10000),
            (unsigned int)r.ru_stime.tv_sec,
                (unsigned int)(r.ru_stime.tv_usec/10000)
        );
        printf("Requests: %u\n", num_requests);
        printf("%lu KB in, %lu KB out\n", total_in/1024, total_out/1024);
    }

    return (0);
}

/* vim:set tabstop=4 shiftwidth=4 expandtab tw=78: */
