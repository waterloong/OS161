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



#endif // OPT_A2
