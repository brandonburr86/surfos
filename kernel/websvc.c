/*
SurfOS user web services
--------------------
File: websvc.c Date: 10/10/26 (Undertow)
--------------------
Copy bounded inputs before any wait; publish outputs only after success.
*/
#include <surfos/sysimpl.h>
#include <net/dhcp.h>
#include <net/dns_wire.h>
#include <fs/vfs.h>

int sys_resolve(const char *host, u32 *ip) {
    char name[DNS_NAME_MAX + 1];
    ipaddr_t address;
    u_int i;
    int r;
    if(!user_buf(ip, sizeof(*ip), true)) return -EFAULT;
    for(i = 0; i <= DNS_NAME_MAX; i++) {
        if(!user_buf(host + i, 1, false)) return -EFAULT;
        name[i] = host[i];
        if(!name[i]) break;
    }
    if(i > DNS_NAME_MAX || !dns_name_valid(name)) return -EINVAL;
    r = dns_lookup(name, &address, 5000);
    if(r == 0) *ip = address;
    return r;
}
