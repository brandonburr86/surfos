/*
SurfOS user library: buffered input and output
----------------------
File: stdio.h   Date: 10/5/26
----------------------
FILE streams over file descriptors (user/libsurf/stdio.c). stdout is line
buffered on the console and fully buffered when it goes to a file, stderr is
unbuffered, files opened with fopen() are fully buffered (BUFSIZ). exit() and
a return from main() flush every stream. The formatter knows the flags - 0 + space #,
width and precision (numbers or *), the lengths hh h l ll z j t and the
conversions d i u x X o c s p %; there is no floating point.
*/

#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF      (-1)
#define BUFSIZ   4096
#define _IOFBF   0              /* full buffering */
#define _IOLBF   1              /* line buffering */
#define _IONBF   2              /* no buffering */
#define FOPEN_MAX    16         /* the kernel's descriptors per process */
#define FILENAME_MAX 256

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct _FILE FILE;

extern FILE *stdin, *stdout, *stderr;

#define __printf(f, a) __attribute__((format(printf, f, a)))
#define __scanf(f, a)  __attribute__((format(scanf, f, a)))

FILE *fopen(const char *path, const char *mode);    /* r w a r+ w+ a+ (b ignored, x: must not exist) */
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);                                /* NULL: every stream */
int fpurge(FILE *f);                                /* 10/6/26: what is buffered is dropped, the error cleared */
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);

size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);                         /* one character of push-back */
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);

int printf(const char *fmt, ...) __printf(1, 2);
int fprintf(FILE *f, const char *fmt, ...) __printf(2, 3);
int dprintf(int fd, const char *fmt, ...) __printf(2, 3);
int sprintf(char *s, const char *fmt, ...) __printf(2, 3);
int snprintf(char *s, size_t n, const char *fmt, ...) __printf(3, 4);
int vprintf(const char *fmt, va_list ap) __printf(1, 0);
int vfprintf(FILE *f, const char *fmt, va_list ap) __printf(2, 0);
int vdprintf(int fd, const char *fmt, va_list ap) __printf(2, 0);
int vsprintf(char *s, const char *fmt, va_list ap) __printf(2, 0);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) __printf(3, 0);
int sscanf(const char *s, const char *fmt, ...) __scanf(2, 3);    /* %d %i %u %x %o %s %c %n %%, widths, * */
int vsscanf(const char *s, const char *fmt, va_list ap) __scanf(2, 0);

int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);

int remove(const char *path);                       /* a file or an empty directory */
int rename(const char *from, const char *to);       /* ENOSYS where the file system cannot */
void perror(const char *s);

#endif
