#include <stdio.h>
#include <sys/syscall.h>

int main(void){
	long int return_value=syscall(335);
	printf("system call returned : %ld\n",return_value);
	return 0;
}
