#include <linux/kernel.h>

asmlinkage long sys_mycall(void){
	printk("Systme Call Example!\n");

	return 0;
}
