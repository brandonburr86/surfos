/*
SurfOS Type Definitions
----------------------
File: types.h   Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_TYPES_H
#define _SURFOS_TYPES_H


/* Type definitions */
typedef unsigned int size_t;
typedef unsigned int pid_t;
typedef unsigned char uint8;
typedef unsigned int bool;

#define false 0
#define true !false


typedef unsigned short u_short;
typedef unsigned char u_char;
typedef unsigned long u_long;
typedef unsigned int u_int;

#define offsetof(type, member) __builtin_offsetof(type, member)
/********************/

/* Console Definitions */
typedef enum STextColor { BLACK_TXT, BLUE_TXT, GREEN_TXT, TEAL_TXT, RED_TXT, MAROON_TXT,
ORANGE_TXT, WHITE_TXT, GREY_TXT, LBLUE_TXT, LGREEN_TXT,LWHITE_TXT, LRED_TXT, PURPLE_TXT
,YELLOW_TXT, BWHITE_TXT } STextColor;

typedef u_char TEXTCOLOR;
/********************/



#endif
