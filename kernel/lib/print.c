// 标准输出和报错机制
#include <stdarg.h>
#include "lib/print.h"
#include "lib/lock.h"
#include "dev/uart.h"

volatile int panicking = 0; // printing a panic message
volatile int panicked = 0;

static spinlock_t print_lk;

static char digits[] = "0123456789abcdef";

/**
 * @param x: 要打印的整数
 * @param base: 进制（10进制(有符号：d 无符号：u)，16进制(x)等）
 * @param sign: 是否有符号(1: 有符号 0: 无符号)
 */
static void
printint(long long xx, int base, int sign)
{
  // 一个64位无符号整数 (unsigned long long) 的最大值
  // 是 18,446,744,073,709,551,615，这有 20 位数字。
  char buf[20];
  int i;
  unsigned long long x;

  // 无符号数、有符号数但是非负数，原值赋给 x
  // 有符号数且为负数，取相反数赋给 x
  // 注意x为无符号类型，-xx会被转换为对应的无符号值
  // 避免了由于有符号数不对称而导致的溢出问题
  if(sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  // 使用 do-while 确保即使 x 为 0，循环体也至少执行一次，
  // 从而能正确打印出 "0"。注意 ++ 为后置递增运算符
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  // 处理符号
  if(sign)
    buf[i++] = '-';

  // 前置--
  while(--i >= 0)
    uart_putc_sync(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  // 16进制
  uart_putc_sync('0');
  uart_putc_sync('x');
  // 打印64位，一个字节2个16进制字符，共16个字符
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    // 取最高的4位进行打印
    uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

void print_init(void)
{
    spinlock_init(&print_lk, "print");
}

// Print to the console. only understands %d, %x, %p, %s.
void printf(const char *fmt, ...)
{
    // variable argument list: use va_list to access ... arguments
    va_list ap; // argument pointer
    int i, cx, c0;
    char *s;

    if (panicking == 0) {
        spinlock_acquire(&print_lk);
    }

    // initialize ap to point to first unnamed argument
    // fmt is the last named argument
    // ap is va_list type variable
    va_start(ap, fmt); 
    // cx : current character
    // & 0xff : 防御性编程，防止char被某些系统视为signed char
    // 而导致符号扩展（cx 为 int 类型）
    for(i = 0; (cx = fmt[i] & 0xff) != 0; i++){
        if(cx != '%'){ // 普通字符，直接输出
            uart_putc_sync(cx);
            continue;
        }
        // 格式化占位符，读取下一个字符
        i++;
        c0 = fmt[i+0] & 0xff;

        if(c0 == 'd'){ // 十进制有符号整数
            // va_arg: retrieve the next argument value of given type(next arg type)
            // and increment ap to point to next argument
            printint(va_arg(ap, int), 10, 1);
        } else if(c0 == 'x'){ // 十六进制无符号整数
            printint(va_arg(ap, uint32), 16, 0);
        } else if(c0 == 'p'){ // 指针(按64位十六进制打印)
            printptr(va_arg(ap, uint64));
        } else if(c0 == 's'){ // 字符串
            if((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for(; *s; s++)
                uart_putc_sync(*s);
        } else if(c0 == '%'){ // 输出百分号本身(%%)
            uart_putc_sync('%');
        } else if(c0 == 0){ // %在格式化字符串结尾，格式错误，退出循环
            break;
        } else {
            // Print unknown % sequence to draw attention.
            uart_putc_sync('%');
            uart_putc_sync(c0);
        }
    }

    // end up accessing variable argument list
    // release any resources associated with ap and set ap to NULL
    va_end(ap);

    // release the lock after printing, if not panicking
    if(panicking == 0)
        spinlock_release(&print_lk);
}

void panic(const char *s)
{
    panicking = 1; // set panicking flag
    printf("panic: ");
    // \n 的识别转换成对应的ascii是编译器的工作，与printf的逻辑无关
    printf("%s\n", s);
    panicked = 1; // freeze uart output from other CPUs
    // 设置后，无法再向控制台输出任何信息
    for(;;)
        ;
}

void assert(bool condition, const char* warning)
{
    if(!condition){
        panic(warning);
    }
}
