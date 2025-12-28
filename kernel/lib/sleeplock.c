#include "lib/lock.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "proc/proc.h"

void sleeplock_init(sleeplock_t* lk, char* name)
{
    spinlock_init(&lk->lk, "sleep lock");
    lk->name = name;
    lk->locked = 0;
    lk->pid = 0;
}

void sleeplock_acquire(sleeplock_t* lk)
{
    spinlock_acquire(&lk->lk);
    while (lk->locked) {
        proc_sleep(lk, &lk->lk);
    }
    lk->locked = 1;
    lk->pid = myproc()->pid;
    spinlock_release(&lk->lk);
}

void sleeplock_release(sleeplock_t* lk)
{
    spinlock_acquire(&lk->lk);
    lk->locked = 0;
    lk->pid = 0;
    proc_wakeup(lk);
    spinlock_release(&lk->lk);
}

bool sleeplock_holding(sleeplock_t* lk)
{
    int r;
    
    spinlock_acquire(&lk->lk);
    r = lk->locked && (lk->pid == myproc()->pid);
    spinlock_release(&lk->lk);
    return r;
}