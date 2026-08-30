/*
 * The stream functions, all of which fail.
 *
 * See the comment in <stdio.h> for why this is the answer rather than a
 * placeholder: there is no global tree, so there is no path to open. The
 * three stream pointers are NULL and never dereferenced, because nothing
 * gets far enough to dereference them.
 */

#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

FILE *stdin  = NULL;
FILE *stdout = NULL;
FILE *stderr = NULL;

#define ENOENT  2

FILE *fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    errno = ENOENT;
    return NULL;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    (void)path;
    (void)mode;
    (void)f;
    errno = ENOENT;
    return NULL;
}

int fclose(FILE *f)        { (void)f; return EOF; }
size_t fread(void *b, size_t s, size_t c, FILE *f)
{
    (void)b; (void)s; (void)c; (void)f;
    return 0;
}
size_t fwrite(const void *b, size_t s, size_t c, FILE *f)
{
    (void)b; (void)s; (void)c; (void)f;
    return 0;
}
int  feof(FILE *f)         { (void)f; return 1; }
int  ferror(FILE *f)       { (void)f; return 1; }
void clearerr(FILE *f)     { (void)f; }
int  fflush(FILE *f)       { (void)f; return 0; }
int  getc(FILE *f)         { (void)f; return EOF; }
int  ungetc(int c, FILE *f) { (void)c; (void)f; return EOF; }

char *strerror(int errnum)
{
    switch (errnum) {
    case 0:      return "no error";
    case ENOENT: return "no such path in this namespace";
    case EDOM:   return "argument outside the domain";
    case ERANGE: return "result outside the representable range";
    default:     return "unknown error";
    }
}
