#include "sut.h"
#include <stdio.h>
#include <string.h>

void hello1()
{
    int i;
    char *str;
    char sbuf[128];
    sut_open("127.0.0.1", 5000);
    for (i = 0; i < 10; i++)
    {
        sprintf(sbuf, "Dummy server message\n");
        sut_write(sbuf, strlen(sbuf));
        str = sut_read();
        if (strlen(str) != 0)
            printf("I am SUT-One, message from server: %s\n", str);
        else
            printf("ERROR!, empty message received \n");
        sut_yield();
    }
    sut_close();
    sut_exit();
}

void hello2()
{
    int i;
    for (i = 0; i < 20; i++)
    {
        printf("Hello world!, this is SUT-Two \n");
        sut_yield();
    }
    sut_exit();
}

int main()
{
    sut_init();
    sut_create(hello1);
    sut_create(hello2);
    sut_shutdown();
}
