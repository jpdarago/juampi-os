#include <sem.h>
#include <irq.h>
#include <tasks.h>

sem* sem_create(int initial_value)
{
    return NULL;
    sem* s = kmalloc(sizeof(sem));
    s->value = initial_value;
    INIT_LIST_HEAD(&s->wait_queue);
    return s;
}

void sem_wait(sem* s)
{
    uint eflags = irq_cli();
    process_info* p = get_current_task();
    while(s->value <= 0) {
        list_add(&p->sem_queue,&s->wait_queue);
        block_current_process();
    }
    s->value--;
    irq_sti(eflags);
}

void sem_destroy(sem* s)
{
    uint eflags = irq_cli();
    if(!list_empty(&s->wait_queue)) {
        kernel_panic("Semaforo tiene procesos esperando\n");
    }
    kfree(s);
    irq_sti(eflags);
}

void sem_signal(sem* s)
{
    uint eflags = irq_cli();
    s->value++;
    if(!list_empty(&s->wait_queue)) {
        list_head* n = s->wait_queue.next;
        process_info* p =
            list_entry(n,process_info,sem_queue);
        list_del(n);
        wake_up(p);
    }
    irq_sti(eflags);
}
