#include <stdio.h>
#include <sys/pointer.h>
static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)
int main(void) {
    struct pointer_event e;
    int i;
    pointer_limits(640, 400);
    pointer_snapshot(&e); CHECK(e.x == 320 && e.y == 200);
    pointer_device(0, true); pointer_device(1, true);
    CHECK(pointer_devices() == 2);
    pointer_report(0, -7, 9, 0, 1); CHECK(pointer_read(&e));
    CHECK(e.x == 313 && e.y == 209 && e.buttons == 1);
    pointer_report(1, 0, 0, 0, 2); CHECK(pointer_read(&e) && e.buttons == 3);
    pointer_device(0, false); CHECK(pointer_read(&e) && e.buttons == 2);
    pointer_device(1, false); CHECK(pointer_read(&e) && e.buttons == 0);
    pointer_report(0, 2147483647, -2147483647-1, 0, 0);
    CHECK(pointer_read(&e) && e.x == 639 && e.y == 0);
    for(i = 0; i < 100; i++) pointer_report(0, -1, 1, 0, i & 1);
    pointer_report(0, 0, 0, 0, 0);
    while(pointer_read(&e)) {}
    CHECK(e.buttons == 0 && e.x == 539 && e.y == 100);
    CHECK(pointer_dropped() > 0);
    pointer_limits(0, -1); pointer_snapshot(&e); CHECK(e.x == 0 && e.y == 0);
    pointer_report(100, 1, 1, 0, 7); CHECK(!pointer_read(&e));
    printf("pointer: %d checks, %d failures\n", checks, failed);
    return !!failed;
}
