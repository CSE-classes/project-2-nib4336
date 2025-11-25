#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
extern int page_allocator_type;
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);



void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(proc->killed)
      exit();
    proc->tf = tf;
    syscall();
    if(proc->killed)
      exit();
    return;
  }
 // CS 3320 project 2
 // You might need to change the folloiwng default page fault handling
 // for your project 2

 //Lazy page allocation handler for trap()
if(tf->trapno == T_PGFLT){
    uint faulting_va = rcr2();

    // If no current process or we're in kernel mode, we treat it as unhandled
    if(proc == 0 || (tf->cs & 3) == 0){
      cprintf("Unhandled page fault!\n");
    } else {
      // If lazy allocator is enabled and the faulting VA is inside the heap region
      if(page_allocator_type == 1 && faulting_va < proc->sz + 1){
        void *va_page = (void*)PGROUNDDOWN(faulting_va);

        // allocate a physical page
        char *mem = kalloc();
        if(mem == 0){
          // cannot get memory; behave like failed sbrk
          cprintf("Allocating pages failed!\n");
          cprintf("Unhandled page fault!\n");
        } else {
          memset(mem, 0, PGSIZE);
          if(mappages(proc->pgdir, va_page, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0){
            // mapping failed; clean up and treat as unhandled
            kfree(mem);
            cprintf("Allocating pages failed!\n");
            cprintf("Unhandled page fault!\n");
          } else {
            return;
          }
        }
      } else {
        // Not in heap or not using lazy allocator
        cprintf("Unhandled page fault!\n");
      }
    }
  }



  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpu->id == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpu->id, tf->cs, tf->eip);
    lapiceoi();
    break;
   
  //PAGEBREAK: 13
  default:
    if(proc == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpu->id, tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            proc->pid, proc->name, tf->trapno, tf->err, cpu->id, tf->eip, 
            rcr2());
    proc->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(proc && proc->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();
}
