#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int arc, char *argv[])
{
    char *buf = "Hello xv6!";
    int ret_val;
    ret_val = myfunction(buf);
    printf(1, "Retrun value : 0x%x\n", ret_val);
    exit();
}
