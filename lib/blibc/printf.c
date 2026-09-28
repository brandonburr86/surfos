/*
SurfOS - printf()
Infamous printf!!!
(C)2004 Brandon Burr
*/

#include <blibc_common.h>
void printf (const char *format, ...)
{
  char **arg = (char **) &format;
  int c;
  char buf[20];

  arg++;

  while ((c = *format++) != 0)
    {
      if (c != '%')
        putch(c);
      else
        {
          char *p;

          c = *format++;
          switch (c)
            {
        case 'i':
            case 'd':
            case 'u':
            case 'x':
              itoa (buf, c, *((int *) arg++));
              p = buf;
              goto string;
              break;

            case 's':
              p = *arg++;
              if (! p)
                p = "(null)";

            string:
              while (*p)
                putch(*p++);
              break;

            default:
              putch(*((int *) arg++));
              break;
            }
        }
    }
}
