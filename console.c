// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <xv6/param.h>
#include <xv6/fs.h>
#include <termios.h>
#include "defs.h"
#include "traps.h"
#include "spinlock.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "gaia.h"

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
  struct termios termios;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  cprintf("cpu%d: panic: ", cpu->id);
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }
  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else {
    uartputc(c);
  }
}

void
consechoc(int c)
{
  if(c != C('D') && cons.termios.c_lflag & ECHO)
    consputc(c);
}

#define INPUT_BUF 128
struct {
  struct spinlock lock;
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} input;

void
consoleintr(int (*getc)(void))
{
  int c;

  acquire(&input.lock);
  while((c = getc()) >= 0){
    if(cons.termios.c_lflag & ICANON){
      switch(c){
      case C('P'):  // Process listing.
        procdump();
        continue;
      case C('U'):  // Kill line.
        while(input.e != input.w &&
              input.buf[(input.e-1) % INPUT_BUF] != '\n'){
          input.e--;
          consechoc(BACKSPACE);
        }
        continue;
      case C('H'): case '\x7f':  // Backspace
        if(input.e != input.w){
          input.e--;
          consechoc(BACKSPACE);
        }
        continue;
      }
    }
    if(c != 0 && input.e-input.r < INPUT_BUF){
      c = (c == '\r') ? '\n' : c;
      input.buf[input.e++ % INPUT_BUF] = c;
      consechoc(c);
      if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF || (cons.termios.c_lflag & ICANON) == 0){
        input.w = input.e;
        wakeup(&input.r);
      }
    }
  }
  release(&input.lock);
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&input.lock);
  while(n > 0){
    while(input.r == input.w){
      if(proc->killed){
        release(&input.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &input.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D') && cons.termios.c_lflag & ICANON){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n' && cons.termios.c_lflag & ICANON)
      break;
  }
  release(&input.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

int
consoleioctl(struct inode *ip, int req)
{
  struct termios *termios_p;
  if(req != TCGETA && req != TCSETA)
    return -1;
  if(argptr(2, (void*)&termios_p, sizeof(*termios_p)) < 0)
    return -1;
  if(req == TCGETA)
    *termios_p = cons.termios;
  else
    cons.termios = *termios_p;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");
  initlock(&input.lock, "input");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read  = consoleread;
  devsw[CONSOLE].ioctl = consoleioctl;

  cons.termios.c_lflag = ECHO | ICANON;
  cons.locking = 1;
}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt)
{
  int i, c, locking;
  uint *argp;
  char *s, *f;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }

    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }
  if(locking)
    release(&cons.lock);
}
