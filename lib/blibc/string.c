/*
SurfOS String Manipulation Functions - blibc
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <blibc_common.h>

u_int strlen(const char str[]) {
    u_int cnt=0;
    while(*str++!=0) cnt++;
    return cnt;
}

int strcmp(const char str1[], const char str2[]) { //probably not standard
    int s1,s2,i=0;
    s1=strlen(str1);
    s2=strlen(str2);
    if(s1<s2) return -1;
    if(s1>s2) return 1;
    for(i=0;i<s1;i++) if(str1[i]!=str2[i]) return -1;
    return 0;
}

char *strncpy(char *str1, const char *str2, int size) {
    int i;
    for(i=0;i<size;i++) {
        str1[i]=str2[i];
    }
    str1[i]=0;
    return str1;
}

char *strcpy(char *str1, const char *str2) {
    return strncpy(str1,str2,strlen(str2));
}

char *strchr(const char *str, int ch) {
    int i,j=strlen(str);
    for(i=0;i<j;i++) {
        if(str[i]==ch) return (char*)(str+i);
    }
    return NULL;
}
