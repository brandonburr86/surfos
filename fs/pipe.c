/*
SurfOS pipes
--------------------
File: pipe.c    Date: 10/24/26
--------------------
SYS_pipe (surfos/pipe.h). A pipe is a ring of PIPE_SIZE bytes from the kernel heap with two
ends, each a vnode of pipefs, a file system of its own that is never mounted (as the
sockets' and the pseudo-terminals' are): fds[0] is an open file of the reading end's vnode,
fds[1] one of the writing end's. A file's references are its descriptors (dup2, spawnfd),
so an end's vnode is released when the last descriptor of it is closed: the end of the
input for the readers, a broken pipe for the writers. The pipe is freed with its second
end.

The ring is looked at and changed with interrupts off, and the bytes are copied to and from
the caller's memory with them on: a reader takes bytes where the ring's data begins, a
writer puts them after its end, so the two never touch the same bytes. While a copy is
under way its side is busy (rbusy, wbusy): a second reader or writer waits for it, so two
readers never get the same bytes, and a write of up to PIPE_BUF bytes, which waits until
all of it fits, goes in whole. Every wait is one that a kill, Ctrl+C or a hangup cuts short
(wait_prepare_intr()); whatever changes what a pipe is ready for calls poll_wake(). At most
PIPE_MAX pipes exist at once.
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/poll.h>
#include <surfos/pipe.h>
#include <surfos/sysimpl.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <blibc_common.h>

struct pipe {
    char *buf;                  /* PIPE_SIZE bytes: a ring */
    u_int head, len;            /* where the bytes to read begin, how many there are */
    u_char readers, writers;    /* the reading end, the writing end is open */
    u_char rbusy, wbusy;        /* a reader, a writer is copying (interrupts on) */
    u_char ends;                /* ends not released yet: the pipe is freed at 0 */
    wait_queue_t readq;         /* readers waiting for bytes, for the end, or for another reader */
    wait_queue_t writeq;        /* writers waiting for room, or for another writer */
};

static u_int npipes;            /* pipes that exist, PIPE_MAX at most */
static u_long serial;           /* the pipes made so far: a pipe's two inode numbers */

/* the pipes' file system: no paths, nothing to sync, never mounted */
static const struct fs_type pipefs_type = { "pipefs", NULL, NULL, NULL };
static struct superblock pipefs = { &pipefs_type, NULL, NULL, NULL, NULL, false, MUTEX_INIT("pipefs"), 0 };

static const struct vnode_ops read_ops, write_ops;

/* one end is closed for good (writing: the writing end): who waits for it is told; the second
   one frees the pipe. All of it with interrupts off: once the count is down, the other end's
   release may free the pipe at any moment. */
static void end_closed(struct pipe *p, int writing) {
    u_long flags = irq_save();
    bool last;
    if(writing) p->writers = 0;
    else p->readers = 0;
    wake_up_all(writing ? &p->readq : &p->writeq);  /* the end of the input; or nobody will read */
    poll_wake();
    last = --p->ends == 0;
    if(last) npipes--;
    irq_restore(flags);
    if(last) {
        kfree(p->buf);
        kfree(p);
    }
}

static void read_release(struct vnode *v) {
    end_closed((struct pipe *)v->priv, 0);
}

static void write_release(struct vnode *v) {
    end_closed((struct pipe *)v->priv, 1);
}

/* what there is, at least a byte, at most len; 0 once it is empty and the writing end is
   closed; -EINTR when the wait is cut short */
static int pipe_read(struct vnode *v, u32 off, void *buf, u32 len) {
    struct pipe *p = (struct pipe *)v->priv;
    u32 n, at, k;
    u_long flags = irq_save();
    (void)off;
    for(;;) {
        if(p->len && !p->rbusy) break;
        if(!p->len && !p->writers) {                    /* a writer copying has its end open */
            irq_restore(flags);
            return 0;
        }
        if(task_interrupted()) {
            irq_restore(flags);
            return -EINTR;
        }
        wait_prepare_intr(&p->readq);
        irq_restore(flags);
        yield();
        flags = irq_save();
    }
    n = len < p->len ? len : p->len;
    at = p->head;
    p->rbusy = 1;
    irq_restore(flags);
    k = PIPE_SIZE - at < n ? PIPE_SIZE - at : n;        /* up to the end of the ring, then from its start */
    memcpy(buf, p->buf + at, k);
    memcpy((char *)buf + k, p->buf, n - k);
    flags = irq_save();
    p->head = (p->head + n) % PIPE_SIZE;
    p->len -= n;
    p->rbusy = 0;
    irq_restore(flags);
    wake_up_all(&p->writeq);                            /* room */
    wake_up_all(&p->readq);                             /* a reader that waited for this one */
    poll_wake();
    return (int)n;
}

/* all len bytes, waiting for room; up to PIPE_BUF of them in one piece. -EPIPE when nobody
   reads, -EINTR when the wait is cut short: the bytes written by then when there are any */
static int pipe_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    struct pipe *p = (struct pipe *)v->priv;
    const char *s = (const char *)buf;
    u32 done = 0, n, at, k, room;
    u_long flags;
    (void)off;
    while(done < len) {
        flags = irq_save();
        for(;;) {
            if(!p->readers) {
                irq_restore(flags);
                return done ? (int)done : -EPIPE;
            }
            if(task_interrupted()) {
                irq_restore(flags);
                return done ? (int)done : -EINTR;
            }
            room = PIPE_SIZE - p->len;
            if(!p->wbusy && (len > PIPE_BUF ? room > 0 : room >= len)) break;
            wait_prepare_intr(&p->writeq);
            irq_restore(flags);
            yield();
            flags = irq_save();
        }
        n = len - done < room ? len - done : room;
        at = (p->head + p->len) % PIPE_SIZE;
        p->wbusy = 1;
        irq_restore(flags);
        k = PIPE_SIZE - at < n ? PIPE_SIZE - at : n;
        memcpy(p->buf + at, s + done, k);
        memcpy(p->buf, s + done + k, n - k);
        flags = irq_save();
        p->len += n;
        p->wbusy = 0;
        irq_restore(flags);
        done += n;
        wake_up_all(&p->readq);                         /* bytes */
        wake_up_all(&p->writeq);                        /* a writer that waited for this one */
        poll_wake();
    }
    return (int)done;
}

/* surfos/pipe.h: POLLIN with something to read or at the end, POLLHUP at the end */
static int read_poll(struct vnode *v, struct file *f) {
    struct pipe *p = (struct pipe *)v->priv;
    int r = 0;
    u_long flags = irq_save();
    if(p->len || !p->writers) r |= POLLIN;
    if(!p->writers) r |= POLLHUP;
    irq_restore(flags);
    (void)f;
    return r;
}

/* POLLOUT when PIPE_BUF bytes fit; POLLOUT and POLLERR when nobody reads (a write ends at once) */
static int write_poll(struct vnode *v, struct file *f) {
    struct pipe *p = (struct pipe *)v->priv;
    int r = 0;
    u_long flags = irq_save();
    if(!p->readers) r |= POLLOUT | POLLERR;
    else if(PIPE_SIZE - p->len >= PIPE_BUF) r |= POLLOUT;
    irq_restore(flags);
    (void)f;
    return r;
}

static const struct vnode_ops read_ops = {
    NULL, NULL, pipe_read, NULL, NULL, NULL, NULL, read_release, NULL, read_poll
};

static const struct vnode_ops write_ops = {
    NULL, NULL, NULL, pipe_write, NULL, NULL, NULL, write_release, NULL, write_poll
};

/* fs/vfs.h: is f an end of a pipe? (lseek answers -ESPIPE) */
bool pipe_file(const struct file *f) {
    return f && f->v && (f->v->ops == &read_ops || f->v->ops == &write_ops);
}

/* 10/25/26 (the review of B1): a pipe for the kernel, its reading end in f[0] and its writing end in f[1], a reference
   each (vfs_close() lets go of one): 0, -ENFILE or -ENOMEM. SYS_pipe's, and the self test's for its children's input */
int pipe_files(struct file *f[2]) {
    struct pipe *p;
    struct vnode *v[2] = { NULL, NULL };
    int i;
    u_long flags;
    u32 ino;

    f[0] = f[1] = NULL;
    flags = irq_save();
    if(npipes == PIPE_MAX) {
        irq_restore(flags);
        return -ENFILE;
    }
    npipes++;
    ino = (u32)(++serial & 0xFFFFFFF) * 2;
    irq_restore(flags);
    p = (struct pipe *)kcalloc(1, sizeof(*p));
    if(p) p->buf = (char *)kalloc(PIPE_SIZE);
    if(!p || !p->buf) {
        if(p) kfree(p);
        flags = irq_save();
        npipes--;
        irq_restore(flags);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;
    p->ends = 2;
    for(i = 0; i < 2; i++) {
        v[i] = vnode_alloc(&pipefs, i ? &write_ops : &read_ops, VN_FILE);
        if(!v[i]) continue;
        v[i]->priv = p;
        v[i]->ino = ino + (u32)i;
    }
    for(i = 0; i < 2 && v[i]; i++) {
        if(vfs_open_vnode(v[i], i ? O_WRONLY : O_RDONLY, &f[i])) break;
        v[i] = NULL;                                    /* the file has the vnode's reference now */
    }
    if(i < 2) {                                         /* no memory: what there is goes again */
        for(i = 0; i < 2; i++) {
            if(f[i]) vfs_close(f[i]);                   /* releases its end */
            else if(v[i]) vput(v[i]);                   /* the same */
            else end_closed(p, i);                      /* an end that never had a vnode */
            f[i] = NULL;
        }
        return -ENOMEM;
    }
    return 0;
}

/* surfos/pipe.h: the reading end as fds[0], the writing end as fds[1] */
int sys_pipe(int *fds) {
    struct file *f[2];
    int k[2], i, spare = 0, r;

    if(!user_buf(fds, sizeof(k), true)) return -EFAULT;
    for(i = 0; i < NR_OPEN; i++) if(!curTask->files[i]) spare++;
    if(spare < 2) return -EMFILE;
    r = pipe_files(f);
    if(r) return r;
    k[0] = fd_install(f[0]);                            /* two descriptors are free: this task alone */
    k[1] = fd_install(f[1]);                            /* changes its table */
    memcpy(fds, k, sizeof(k));
    return 0;
}
