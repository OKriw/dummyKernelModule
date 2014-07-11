/*  
 *
 *    module.c - The simple kernel module that can:
 *  - be compiled, loaded, unloaded
 *  - create proc entry
 *  - create second proc entry
 *  - show different output depending for each proc entry used 
 *  - support writing to proc entries a single integer, store it and show with read callbacks
 *  - store input values in a list, print them in read callback, clean list at unload
 *  - add only unique values to list
 *  - protect list with spinlock so it can be used from multiple threads
 *  - use single file instead os seq file
 *  - write callback for second proc file deletes entry from list if it is present
 *  - list is protected with RCU for concurrent reading
 *
 */

// KNOWN BUGS
//  FIXME: echo "123456789012" > /proc/_first_j_entry  hangs the system for unknown reason

#include <linux/module.h>	/* necessary for any module*/
#include <linux/kernel.h>	/* for KERN_XXX macros */
#include <linux/init.h>	/* for __init and __exit macros (but it also seems to works without it O_o) */
#include <linux/proc_fs.h>	/* for working with /proc filesystem */
#include <linux/seq_file.h>	/* for seq_XXX operations with proc file */
#include <linux/uaccess.h>	/* for copy_from_user */
#include <linux/spinlock.h>	/* for spinlock */
#include <linux/version.h>

#define FIRST_PROC_ENTRY_NAME "_first_j_entry"
#define SECOND_PROC_ENTRY_NAME "_second_j_entry"

#define BUF_SIZE 12	/* size of kernel buffer to copy user string: length of minimum integer + 1 for '\0' */

static long int data;
static char * first_entry_name = FIRST_PROC_ENTRY_NAME;
static char * second_entry_name = SECOND_PROC_ENTRY_NAME;

static LIST_HEAD(j_list);

static DEFINE_SPINLOCK(list_lock);

struct j_list_entry {
	struct list_head j_list_head;
	long int val;
	struct rcu_head rcu_head;
};

static void *j_seq_start(struct seq_file *f, loff_t *pos)
{
	rcu_read_lock();
	if (*pos == 0)
		return SEQ_START_TOKEN;
	else
		return seq_list_start(&j_list, *pos);

};

static void *j_seq_next(struct seq_file *f, void *v, loff_t *pos)
{
	if (v ==SEQ_START_TOKEN)
		return seq_list_start(&j_list, 0);

	return seq_list_next(v, &j_list, pos);
}

static void j_seq_stop(struct seq_file *f, void *v)
{
	rcu_read_unlock();
}

static int j_seq_show(struct seq_file *f, void *v)
{
	struct j_list_entry *entry;

	if (v != SEQ_START_TOKEN) {
		entry = container_of(v, struct j_list_entry, j_list_head);
		seq_printf(f, "val is %ld\n", entry->val);
	} else {
		seq_printf(f, "This is start of list, (entry name is %s)\n", (char *)f->private);
	}

	return 0;
}

static struct seq_operations j_seq_operations = {
	.start = j_seq_start,
	.stop = j_seq_stop,
	.next = j_seq_next,
	.show = j_seq_show
};

static int first_j_open(struct inode *j_inode, struct file *j_file)
{
	int ret;

	if((ret = seq_open(j_file, &j_seq_operations)))
		return ret;

	((struct seq_file *)j_file->private_data)->private = first_entry_name;
	return ret;
}

static int second_j_open(struct inode *j_inode, struct file *j_file)
{
	int ret;

	if((ret = seq_open(j_file, &j_seq_operations))) 
		return ret;

	((struct seq_file *)j_file->private_data)->private = second_entry_name;
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0)
static void free_list_entry(struct rcu_head* head)
{
	struct j_list_entry *entry = container_of(head, struct j_list_entry, rcu_head);
	kfree(entry);
}
#endif

static ssize_t first_j_write(struct file* j_file, const char __user *user_input, size_t size, loff_t *offset)
{
	char kbuf[BUF_SIZE], *clean_kbuf;
	unsigned long written, left;
	int ret;
	struct j_list_entry *new_entry;
	int new = 1;
	struct j_list_entry *list_cursor;

	printk("%s\n", __func__);

	if ((left = copy_from_user(&kbuf, user_input, min_t(unsigned long, size, BUF_SIZE-1)))) {	// copy_from_user returns amount of UNCOPIED data
		printk(KERN_ERR "copy_from_user failed (left %ld) \n", left);
		return -EFAULT;
	} else {
		written = min_t(unsigned long, size, BUF_SIZE);
		printk(KERN_INFO "copied %ld bytes from user\n", written);
	}

	printk("force string to be null terminated\n");

	kbuf[written] = '\0';
	printk("data is: %s\n", kbuf);

	clean_kbuf = strstrip(kbuf);	// warning: ignoring return value of `strstrip`
	printk("clean data is: %s\n", clean_kbuf);

	if ((ret = strict_strtol(clean_kbuf, 10, &data))) {
		printk(KERN_ERR "invalid input: %s\n", clean_kbuf);
		return ret; //-EINVAL or -ERANGE
	}


	printk("conversion done\n");

	new_entry = kmalloc(sizeof(struct j_list_entry), GFP_KERNEL); // XXX: is it OK to do kmalloc here or is it better to alloc not holding spinlock and free in not needed
	new_entry->val = data;

	spin_lock(&list_lock);
	list_for_each_entry_rcu(list_cursor, &j_list, j_list_head) { //XXX: no need for _rcu becuase already holding spinlock
		if (list_cursor->val == data) {
			new = 0;
			break;
		}
	}

	if (new)
		list_add_tail_rcu(&new_entry->j_list_head, &j_list);

	spin_unlock(&list_lock);

	if (!new)
		kfree(new_entry);

	return size;
}

static ssize_t second_j_write(struct file* j_file, const char __user *user_input, size_t size, loff_t *offset)
{
	char kbuf[BUF_SIZE], *clean_kbuf;
	unsigned long written, left;
	int ret;
	struct j_list_entry *list_cursor;

	if ((left = copy_from_user(&kbuf, user_input, min_t(unsigned long, size, BUF_SIZE-1)))) {	// copy_from_user returns amount of UNCOPIED data
		printk(KERN_ERR "copy_from_user failed (left %ld) \n", left);
		return -EFAULT;
	} else {
		written = min_t(unsigned long, size, BUF_SIZE) - left;
		printk(KERN_INFO "copied %ld bytes from user\n", written);
	}

	kbuf[written] = '\0';
	printk("data is: %s\n", kbuf);

	clean_kbuf = strstrip(kbuf);	// warning: ignoring return value of `strstrip`
	printk("clean data is: %s\n", clean_kbuf);

	if ((ret = strict_strtol(clean_kbuf, 10, &data))) {
		printk(KERN_ERR "invalid input: %s\n", clean_kbuf);
		return ret; //-EINVAL or -ERANGE
	}

	spin_lock(&list_lock);

	list_for_each_entry_rcu(list_cursor, &j_list, j_list_head) { //XXX: no need for _rcu here because already holding spinlock
		if (list_cursor->val == data) {
			list_del_rcu(&list_cursor->j_list_head);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
			kfree_rcu(list_cursor, rcu_head);
#else
			call_rcu(&list_cursor->rcu_head, free_list_entry);
#endif
			break;
		}
	}
	spin_unlock(&list_lock);

	return size;
}

static struct file_operations first_j_file_operations = {
	.owner = THIS_MODULE,
	.open = first_j_open,
	.read = seq_read,
	.write = first_j_write,
	.llseek = seq_lseek,
	.release = seq_release
};

static struct file_operations second_j_file_operations = {
	.owner = THIS_MODULE,
	.open = second_j_open,
	.read = seq_read,
	.write = second_j_write,
	.llseek = seq_lseek,
	.release = seq_release
};

static int create_proc_entry_verbose(char* name, struct file_operations *file_ops)
{
	struct proc_dir_entry *proc_file_entry = NULL;

	if ((proc_file_entry = proc_create(name, S_IROTH | S_IWOTH , NULL, file_ops)) != NULL)
		printk(KERN_INFO "Proc entry %s created :)\n", name);
	else {
		printk(KERN_ERR "Prock entry %s not created :(\n", name);
		return -ENOMEM;	//XXX: why ENOMEM ???
	}

	return 0;
}

static int __init init_j_module(void)
{
	int status = 0;

	printk(KERN_INFO "blue_pig is in the kernel\n");

	if ((status = create_proc_entry_verbose(FIRST_PROC_ENTRY_NAME, &first_j_file_operations)))
		return status;

	if ((status = create_proc_entry_verbose(SECOND_PROC_ENTRY_NAME, &second_j_file_operations))){
		remove_proc_entry(FIRST_PROC_ENTRY_NAME, NULL);	// cleanup
		return status;
	}

	return 0;
}

static void __exit exit_j_module(void)
{
	struct j_list_entry *to_del;

	remove_proc_entry(FIRST_PROC_ENTRY_NAME, NULL);
	remove_proc_entry(SECOND_PROC_ENTRY_NAME, NULL);

	while (!(list_empty(&j_list))) {
		to_del = list_first_entry(&j_list, struct j_list_entry, j_list_head);
		list_del(&to_del->j_list_head);
		printk(KERN_INFO "deleting %ld\n", to_del->val);
		kfree(to_del);
	}

	printk(KERN_INFO "blue_pig left the kernel alone\n");
}

module_init(init_j_module);
module_exit(exit_j_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("blue_pig");
MODULE_DESCRIPTION("Dummy kernel module");
