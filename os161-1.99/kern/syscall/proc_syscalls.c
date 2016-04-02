#include <opt-A2.h>

#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include <synch.h>
#include <limits.h>
#include <vfs.h>
#include <kern/fcntl.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;


  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

#ifdef OPT_A2
	p->p_exit_code = _MKWAIT_EXIT(exitcode);
	p->p_is_alive = false;
	lock_acquire(p->p_wait_lock);
	cv_broadcast(p->p_wait_cv, p->p_wait_lock);
	lock_release(p->p_wait_lock);
/*
	need to prevent the situation where we have destroyed this process and the parent is still
	waiting for a destroyed lock, doing any work would be fine. 
*/
#endif
	
  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
#ifdef OPT_A2
  *retval = curproc->p_pid;
#else
  *retval = 1;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

#ifdef OPT_A2
	lock_acquire(process_table_lock); // is this synchronization even necessary?
	// make sure the process exist
	struct proc *child = procarray_get(process_table, pid);
	if (child == NULL)
	{
		lock_release(process_table_lock);
		return ESRCH;
	}
	lock_release(process_table_lock);

	// make sure the process is a child process
	if (child->p_parent != curproc)
	{
		return ECHILD;
	}	

	if (((options & WNOHANG) > 0) && child->p_is_alive)
	{
		return ECHILD;
	}

	lock_acquire(child->p_wait_lock);
	while (child->p_is_alive)
	{
		cv_wait(child->p_wait_cv, child->p_wait_lock);
	}
	lock_release(child->p_wait_lock);

	exitstatus = child->p_exit_code;
#else

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }


  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif // OPT_A2 

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

#ifdef OPT_A2
int
sys_fork(struct trapframe *tf, pid_t *retval) {
/*
 * errno:
 *  https://www.student.cs.uwaterloo.ca/~cs350/common/os161-man/syscall/errno.html
 */
	int errno;
	if (no_pid_left()) return ENPROC;
	// Create process structure for child process
	struct proc *forked_proc = proc_create_runprogram(curproc->p_name);
	if ( !forked_proc )
	{
		return ENOMEM;
	}	

	forked_proc->p_parent = curproc;
	procarray_add(&curproc->p_children, forked_proc, NULL);

	errno = as_copy(curproc_getas(), &forked_proc->p_addrspace); 
	if (errno)
	{
		proc_destroy(forked_proc);
		return errno;
	}

	struct trapframe *forked_tf = kmalloc(sizeof(struct trapframe));
	if ( !forked_tf ) 
	{
		as_destroy(forked_proc->p_addrspace);
		proc_destroy(forked_proc);
		return ENOMEM;
	}
	memcpy(forked_tf, tf, sizeof(struct trapframe));

	errno = thread_fork("forked_thread", forked_proc, (void*)enter_forked_process, forked_tf, 0);
	if (errno) 
	{
		kfree(forked_tf);
		as_destroy(forked_proc->p_addrspace);
		proc_destroy(forked_proc);
		return errno;
	}
	
	*retval = forked_proc->p_pid;
	return 0;
}

/* backport of strnlen from clib */
static 
size_t
strnlen(const char *str, size_t max_len)
{
	size_t len;

	for (len = 0; len < max_len; len ++) {
		if (!(str[len]))
		{
			break;
		}
	}
	return len;
}

/*
 *	comes from ARG_MAX. 
 */
#define MAX_ARGC 64 // prof said a sufficient large one would work
#define MAX_ARG 1024
/* len is the number of successful allocation, not theorectial number of elements */
static 
void
free2d(char **arr, int len)
{
	for (int i = 0; i < len; i ++)
	{
		kfree(arr[i]);
	}
	kfree(arr);
}

/*
 * https://www.student.cs.uwaterloo.ca/~cs350/common/os161-man/syscall/execv.html
 */
// signature directly from slides
int
sys_execv(userptr_t progname, userptr_t args)
{
/*
After finishing the assignment, in retrospect, the order of implementation given 
in the supplementary slide is subideal: copying arguments into kernel should not 
be the 1st thing to do, as error handling in the middle part would require free them.
*/
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	int len; // temp variable for length
	struct addrspace *old_as;

	if (progname == NULL)
	{
		return EFAULT;
	}
	// Count the number of arguments and copy them into the kernel
	int argc = 0;
	while (((char**)args)[argc] != NULL) 
	{
		if (argc == MAX_ARGC)
		{
			return E2BIG;
		}
		argc ++;
	}
	char **kargv = kmalloc((argc + 1) * sizeof(char*)); // +1 for NULL terminator
	if (kargv == NULL) return ENOMEM;
	
	for (int i = 0; i < argc; i ++) {
		len = strnlen(((char**)args)[i], MAX_ARG + 1);
		if (len > MAX_ARG + 1)
		{
			free2d(kargv, i);
			return E2BIG;
		}	
		char *arg = kmalloc((len + 1)* sizeof(char));
		if (arg == NULL)
		{
			free2d(kargv, i);
			return ENOMEM;
		}
		kargv[i] = arg;	
		result = copyinstr((const_userptr_t)(((char**)args)[i]), arg, len + 1, NULL);
		if (result)
		{
			free2d(kargv, i + 1);
			return result;
		}
	}
	kargv[argc] = NULL;

	// Copy the program path into the kernel
	// size of PATH_MAX is aligned, so it is assumed that it includes null terminator. 
	len = strnlen((char *)progname, PATH_MAX);
	if (len == PATH_MAX)
	{
		free2d(kargv, argc);
		return E2BIG;
	}
	char* path = kmalloc((len + 1) * sizeof(char));
	if (path == NULL)
	{
		free2d(kargv, argc);
		return ENOMEM;
	}

	result = copyinstr(progname, path, PATH_MAX, NULL);	
	if (result)
	{
		kfree(path);
		return result;
	}

	/* Open the file. */
	result = vfs_open(path, O_RDONLY, 0, &v);
	if (result) {
		kfree(path);
		free2d(kargv, argc);
		return result;
	}

	/* We should be a new process. */
//	KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		kfree(path);
		free2d(kargv, argc);
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	old_as =  curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		kfree(path);
		free2d(kargv, argc);
		as_deactivate();
		curproc_setas(old_as);
		as_activate();
		as_destroy(as);
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		kfree(path);
		free2d(kargv, argc);
		as_deactivate();
		curproc_setas(old_as);
		as_activate();
		as_destroy(as);
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

/*
	Need to copy the arguments into the new address space. Consider copying the
	arguments (both the array and the strings) onto the user stack as part of
	as_define_stack.
*/	

	// sanity check
	while (stackptr % 8) stackptr --;
	char **user_argv;
	for (int i = 0; i < argc; i ++) 
	{
		len = strlen(kargv[i]);
		stackptr -= ROUNDUP((len + 1) * sizeof(char), 8);
		result = copyout(kargv[i], (userptr_t)stackptr, (len + 1) * sizeof(char));
		if (result) 
		{
			kfree(path);
			free2d(kargv, argc);
			as_deactivate();
			curproc_setas(old_as);
			as_activate();
			as_destroy(as);
			return result;
		}
		kfree(kargv[i]);
		kargv[i] = (char*)stackptr;
	}
	while (stackptr % 8) stackptr --;
	stackptr -= ROUNDUP((argc + 1) * sizeof(char*), 8);
	user_argv = (char**)stackptr;
	result = copyout(kargv, (userptr_t)stackptr, (argc + 1) * sizeof(char*));
	kfree(kargv);
	if(result) {
		as_deactivate();
		curproc_setas(old_as);
		as_activate();
		as_destroy(as);
		return result;
	}

	// required by slides
	as_destroy(old_as);

	/* Warp to user mode. */
	enter_new_process(argc /*argc*/, (userptr_t)user_argv /*userspace addr of argv*/,
			  stackptr, entrypoint);
		
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;

}

#endif // OPT_A2
