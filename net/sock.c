/*
SurfOS Network Stack: connections for programs
--------------------
File: sock.c    Date: 10/6/26
--------------------
SYS_listen, SYS_accept, SYS_connect and SYS_netinfo (surfos/sock.h). A listening
socket or a connection is a file: a vnode of "sockfs", a file system that is never
mounted, whose vnodes each wrap a TCP socket (net/tcp.c). read() and write() go to
tcp_read() and tcp_write(), poll() to tcp_poll(); when the last descriptor goes
(close(), or the descriptors closed as the task ends, also when it was killed) the
vnode is released and the socket closed: a connection sends its FIN and finishes on
its own, a listening socket stops listening. dup2() and spawnfd() share the file, as
for any other. The calls answer -errno.
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/sysimpl.h>
#include <surfos/sock.h>
#include <surfos/poll.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/tcp.h>
#include <blibc_common.h>

#define CONNECT_MS 10000                /* SYS_connect gives up after this */

static const struct vnode_ops sock_vops;
static const struct fs_type sockfs_type = { "sockfs", NULL, NULL, NULL };
static struct superblock sockfs = { &sockfs_type, NULL, NULL, NULL, NULL, false, MUTEX_INIT("sockfs"), 0 };

static int sock_read(struct vnode *v, u32 off, void *buf, u32 len) {
    (void)off;
    return tcp_read((struct tcp_socket *)v->priv, buf, len);
}

static int sock_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    (void)off;
    return tcp_write((struct tcp_socket *)v->priv, buf, len);
}

/* the last reference: the last descriptor of the socket is closed */
static void sock_release(struct vnode *v) {
    if(v->priv) tcp_release((struct tcp_socket *)v->priv);
    v->priv = NULL;
}

static int sock_poll(struct vnode *v, struct file *f) {
    (void)f;
    return v->priv ? tcp_poll((struct tcp_socket *)v->priv) : POLLHUP;
}

static const struct vnode_ops sock_vops = {
    NULL, NULL, sock_read, sock_write, NULL, NULL, NULL, sock_release, NULL, sock_poll
};

/* the socket behind a descriptor, NULL when it is no socket */
static struct tcp_socket *fd_socket(struct file *f) {
    return f && f->v->ops == &sock_vops ? (struct tcp_socket *)f->v->priv : NULL;
}

/* is there a free descriptor? (asked before a socket is made, so none is lost for want of one) */
static bool fd_free(void) {
    int i;
    for(i = 0; i < NR_OPEN; i++) if(!curTask->files[i]) return true;
    return false;
}

/* a descriptor for s; when there is none, s is closed and the error returned */
static int sock_fd(struct tcp_socket *s) {
    struct vnode *v = vnode_alloc(&sockfs, &sock_vops, VN_FILE);
    struct file *f;
    int fd;
    if(!v) { tcp_release(s); return -ENOMEM; }
    v->priv = s;
    if(vfs_open_vnode(v, O_RDWR, &f)) { vput(v); return -ENOMEM; }     /* vput releases s */
    fd = fd_install(f);
    if(fd < 0) vfs_close(f);
    return fd;
}

int sys_listen(u32 port) {
    struct tcp_socket *s;
    int r;
    if(!port || port > 65535) return -EINVAL;
    if(!fd_free()) return -EMFILE;
    r = tcp_listen_on((u16)port, &s);
    return r ? r : sock_fd(s);
}

/* -EBADF for no descriptor, -EINVAL for one that is no listening socket; the peer's address is
   checked before anything waits, so no connection is lost to a bad pointer */
int sys_accept(int fd, struct netaddr *peer) {
    struct file *f = fd_get(fd);
    struct tcp_socket *l = fd_socket(f), *c;
    struct netaddr a;
    int r;
    if(!f) return -EBADF;
    if(!l || !tcp_listening(l)) return -EINVAL;
    if(peer && !user_buf(peer, sizeof(*peer), true)) return -EFAULT;
    if(!fd_free()) return -EMFILE;
    r = tcp_accept_next(l, TCP_NO_LIMIT, &c);
    if(r) return r;
    memset(&a, 0, sizeof(a));
    a.ip = tcp_peer(c, &a.port);
    r = sock_fd(c);
    if(r >= 0 && peer) memcpy(peer, &a, sizeof(a));
    return r;
}

int sys_connect(u32 ip, u32 port) {
    struct tcp_socket *s;
    int r;
    if(!ip || !port || port > 65535) return -EINVAL;
    if(!fd_free()) return -EMFILE;
    r = tcp_open(ip, (u16)port, CONNECT_MS, &s);
    return r ? r : sock_fd(s);
}

int sys_netinfo(struct netinfo *ni) {
    struct netdev *dev;
    struct netinfo k;
    if(!user_buf(ni, sizeof(*ni), true)) return -EFAULT;
    memset(&k, 0, sizeof(k));
    mutex_lock(&net_lock);                      /* dhcp and ifconfig change it under the lock */
    dev = netdev_default();
    if(dev) {
        strlcpy(k.name, dev->name, sizeof(k.name));
        memcpy(k.mac, dev->mac, sizeof(k.mac));
        k.link = dev->link_up ? 1 : 0;
        k.configured = dev->ip ? 1 : 0;
        k.ip = dev->ip;
        k.netmask = dev->netmask;
        k.gateway = dev->gateway;
        k.dns = dev->dns;
    }
    mutex_unlock(&net_lock);
    if(!dev) return -ENODEV;
    memcpy(ni, &k, sizeof(k));
    return 0;
}
