/* Force-included when blibc is compiled for the host: rename every symbol so the test
   program can call the SurfOS versions next to the host libc's. */
#define vsnprintf surf_vsnprintf
#define snprintf  surf_snprintf
#define sprintf   surf_sprintf
#define strlen    surf_strlen
#define strnlen   surf_strnlen
#define strcmp    surf_strcmp
#define strncmp   surf_strncmp
#define strcpy    surf_strcpy
#define strncpy   surf_strncpy
#define strlcpy   surf_strlcpy
#define strcat    surf_strcat
#define strlcat   surf_strlcat
#define strchr    surf_strchr
#define strrchr   surf_strrchr
#define strstr    surf_strstr
#define strtok_r  surf_strtok_r
#define memcpy    surf_memcpy
#define memmove   surf_memmove
#define memset    surf_memset
#define memcmp    surf_memcmp
#define memchr    surf_memchr
#define strtol    surf_strtol
#define strtoul   surf_strtoul
#define atoi      surf_atoi
#define abs       surf_abs
#define _ctype    surf__ctype
