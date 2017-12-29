/*
 * Copyright (C) 2009 Renê de Souza Pinto
 * Tempos - Tempos is an Educational and multi purpose Operating System
 *
 * File: task.c
 * Desc: Implements functions to create/handle tasks.
 *
 * This file is part of TempOS.
 *
 * TempOS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * TempOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <tempos/sched.h>
#include <tempos/kernel.h>
#include <x86/x86.h>
#include <x86/io.h>

#define load_esp(x) __asm__ __volatile__("movl %%esp, %0" : "=r"(x))

#define load_eflags(x) __asm__ __volatile__("pushfl ; popl %0" : "=r"(x) :: "eax")

extern tss_t task_tss;
extern pagedir_t *kerneldir;

/**
 * This is the low level routine to make context switch
 * \see arch/x86/task.S
 */
extern void task_switch_to(arch_tss_t *task);
extern void initial_task(task_t *task);

arch_tss_t *arch_tss_cur_task;

/**
 * Configure and start the first kernel thread.
 * \param start_routine Pointer to the function which will be executed.
 */
void arch_init_scheduler(void (*start_routine)(void*))
{
	uint32_t eflags;
	task_t *newth = NULL;
	char *kstack;

	/* Alloc memory for task structure */
	newth = (task_t*)kmalloc(sizeof(task_t), GFP_NORMAL_Z);
	if (newth == NULL) {
		return;
	}

	/* Alloc memory for kernel TSS stack */
	kstack = (char*)kmalloc(PROCESS_STACK_SIZE, GFP_NORMAL_Z | PAGE_USER);
	if (kstack == NULL) {
		kfree(newth);
		return;
	} else {
		task_tss.esp0 = (uint32_t)kstack + PROCESS_STACK_SIZE;
	}

	load_eflags(eflags);
	newth->state       = TASK_RUNNING;
	newth->priority    = DEFAULT_PRIORITY;
	newth->pid         = KERNEL_PID;
	newth->return_code = 0;
	newth->wait_queue  = 0;

	newth->arch_tss.regs.eip = (uint32_t)start_routine;
	newth->arch_tss.regs.ds  = KERNEL_DS;
	newth->arch_tss.regs.fs  = KERNEL_DS;
	newth->arch_tss.regs.gs  = KERNEL_DS;
	newth->arch_tss.regs.ss  = KERNEL_DS;
	newth->arch_tss.regs.es  = KERNEL_DS;
	newth->arch_tss.regs.cs  = KERNEL_CS;
	newth->arch_tss.cr3 = (uint32_t)kerneldir->dir_phy_addr; /* physical address */
	newth->arch_tss.regs.eflags = (eflags | EFLAGS_IF); /* enable interrupts */

	cli();
	c_llist_add(&tasks, newth);
	cur_task = tasks;
	arch_tss_cur_task = &newth->arch_tss;
	sti();

	/* Jump to main thread */
	load_esp(newth->arch_tss.regs.esp);
	newth->kstack = (char*)(newth->arch_tss.regs.esp);
	initial_task(newth);
}

/**
 * Configure and prepare the stack to initialize the thread.
 */
void setup_task(task_t *task, void (*start_routine)(void*))
{
	uint32_t ss, cs;

	if (task == NULL) {
		return;
	}

	task->arch_tss.regs.eip = (uint32_t)start_routine;
	task->arch_tss.regs.ds  = KERNEL_DS;
	task->arch_tss.regs.fs  = KERNEL_DS;
	task->arch_tss.regs.gs  = KERNEL_DS;
	task->arch_tss.regs.ss  = KERNEL_DS;
	task->arch_tss.regs.es  = KERNEL_DS;
	task->arch_tss.regs.cs  = KERNEL_CS;
	task->arch_tss.cr3 = (uint32_t)kerneldir->dir_phy_addr; /* physical address */

	task->arch_tss.regs.eflags = EFLAGS_IF;
	
	/* Setup thread context into stack */
	task->arch_tss.regs.esp = (uint32_t)task->kstack - (14 * sizeof(task->arch_tss.regs.eax)) - sizeof(task->arch_tss.regs.ds);
	
	/* Configure thread's stack */
	cs = task->arch_tss.regs.cs;
	ss = task->arch_tss.regs.ss;
	push_into_stack(task->kstack, ss);
	push_into_stack(task->kstack, task->arch_tss.regs.esp);
	push_into_stack(task->kstack, task->arch_tss.regs.eflags);
	push_into_stack(task->kstack, cs);
	push_into_stack(task->kstack, task->arch_tss.regs.eip);
	push_into_stack(task->kstack, task->arch_tss.regs.eax);
	push_into_stack(task->kstack, task->arch_tss.regs.ecx);
	push_into_stack(task->kstack, task->arch_tss.regs.edx);
	push_into_stack(task->kstack, task->arch_tss.regs.ebx);
	push_into_stack(task->kstack, task->arch_tss.regs.esp);
	push_into_stack(task->kstack, task->arch_tss.regs.ebp);
	push_into_stack(task->kstack, task->arch_tss.regs.esi);
	push_into_stack(task->kstack, task->arch_tss.regs.edi);
	push_into_stack(task->kstack, task->arch_tss.regs.ds);
	push_into_stack(task->kstack, task->arch_tss.cr3);

	return;
}

/**
 * Switch task
 */

uint32_t current_esp; /*esta variável global pega o valor do stack seja ela do kernelspace ou do userspace */


void switch_to(c_llist *tsk)
{
	task_t *task = GET_TASK(tsk);
	task_t *current_task = GET_TASK(cur_task);
   

	if (current_task == NULL || task == NULL) {
		return;
	}
	
	/* Change context to the new task */
	if (current_task->state == TASK_RUNNING) {
		current_task->state = TASK_READY_TO_RUN;
	}
	arch_tss_cur_task =&current_task->arch_tss;
    current_esp = current_task->arch_tss.regs.esp;
	cur_task = tsk;
	task->state = TASK_RUNNING;
	task_switch_to(&task->arch_tss);
}

