// 来自test.c
// 测试目的: 测试 stdin 和 stdout 输入一行然后回车换行
#include "userlib.h"

int main(int argc, char* argv[])
{
    char tmp[128];
    while(1) {
        memset(tmp, 0, 128);
        stdin(tmp, 128);
        stdout(tmp, strlen(tmp));
    }
}