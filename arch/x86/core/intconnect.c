/* intconnect.c - VxMicro interrupt management support for IA-32 arch */

/*
 * Copyright (c) 2010-2014 Wind River Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Wind River Systems nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
DESCRIPTION
This module provides routines to manage asynchronous interrupts in VxMicro
on the IA-32 architecture.

This module provides the public routine irq_connect(), the private
routine _IntVecSet(), and the BSP support routines _IntVecAlloc(),
_IntVecMarkAllocated() and _IntVecMarkFree().

INTERNAL
The _idt_base_address symbol is used to determine the base address of the IDT.
(It is generated by the linker script, and doesn't correspond to an actual
global variable.)

Interrupts are handled by an "interrupt stub" whose code is generated by the
system itself.  The stub performs various actions before and after invoking
the application (or operating system) specific interrupt handler; for example,
a thread context save is performed prior to invoking the interrupt handler.

The IA-32 code that makes up a "full" interrupt stub is shown below.  A full
interrupt stub is one that is associated with an interrupt vector that requires
a "beginning of interrupt" (BOI) callout and an "end of interrupt" (EOI) callout
(both of which require a parameter).

  0x00   call    _IntEnt         /@ inform kernel of interrupt @/
  Machine code:  0xe8, 0x00, 0x00, 0x00, 0x00

  0x05   pushl   $BoiParameter   /@ optional: push BOI handler parameter @/
  Machine code:  0x68, 0x00, 0x00, 0x00, 0x00

  0x0a   call    BoiRoutine      /@ optional: callout to BOI rtn @/
  Machine code:  0xe8, 0x00, 0x00, 0x00, 0x00

  0x0f   pushl   $IsrParameter   /@ push ISR parameter @/
  Machine code:  0x68, 0x00, 0x00, 0x00, 0x00

  0x14   call    IsrRoutine      /@ invoke ISR @/
  Machine code: 0xe8, 0x00, 0x00, 0x00, 0x00

  0x19   pushl   $EoiParameter   /@ optional: push EOI handler parameter @/
  Machine code: 0x68, 0x00, 0x00, 0x00, 0x00

  0x1e   call    EoiRoutine      /@ optional: callout to EOI rtn @/
  Machine code: 0xe8, 0x00, 0x00, 0x00, 0x00

  0x23   addl    $(4 * numParams), %esp    /@ pop parameters @/
  Machine code: 0x83, 0xc4, (4 * numParams)

  0x26  jmp      _IntExit        /@ restore context or reschedule @/
  Machine code: 0xe9, 0x00, 0x00, 0x00, 0x00

NOTE: Be sure to update the arch specific definition of the _INT_STUB_SIZE macro
to reflect the maximum potential size of the interrupt stub (as shown above).
The _INT_STUB_SIZE macro is defined in kernel/arch/Intel/include/nanok.h
and include/Intel/nanokernel.h

*/

#ifndef CONFIG_NO_ISRS

#include <nanokernel.h>
#include <nanokernel/cpu.h>
#include <nanok.h>

/* the _idt_base_address symbol is generated via a linker script */

extern unsigned char _idt_base_address[];

extern void _SpuriousIntHandler(void *);
extern void _SpuriousIntNoErrCodeHandler(void *);

/*
 * These 'dummy' variables are used in nanoArchInit() to force the inclusion of
 * the spurious interrupt handlers. They *must* be declared in a module other
 * than the one they are used in to get around garbage collection issues and
 * warnings issued some compilers that they aren't used. Therefore care must
 * be taken if they are to be moved. See nanok.h for more information.
 */
void *__DummySpur;
void *__DummyExcEnt;

/*
 * Place the addresses of the spurious interrupt handlers into the intList
 * section. The genIdt tool can then populate any unused vectors with
 * these routines.
 */
void *__attribute__((section(".spurIsr"))) MK_ISR_NAME(_SpuriousIntHandler) =
	&_SpuriousIntHandler;
void *__attribute__((section(".spurNoErrIsr")))
	MK_ISR_NAME(_SpuriousIntNoErrCodeHandler) =
		&_SpuriousIntNoErrCodeHandler;

/*
 * Bitfield used to track which interrupt vectors are available for allocation.
 * The array is initialized to indicate all vectors are currently available.
 *
 * NOTE: For portability reasons, the ROUND_UP() macro can NOT be used to
 * perform the rounding up calculation below.  Unlike both GCC and ICC, the
 * Diab compiler generates an error when a macro that takes a parameter is
 * used to define the size of an array.
 */

#define VEC_ALLOC_NUM_INTS ((CONFIG_IDT_NUM_VECTORS + 31) & ~31) / 32

static unsigned int _VectorsAllocated[VEC_ALLOC_NUM_INTS] = {
	[0 ...(VEC_ALLOC_NUM_INTS - 1)] = 0xffffffff
};

/*******************************************************************************
*
* _IntVecSet - connect a routine to an interrupt vector
*
* This routine "connects" the specified <routine> to the specified interrupt
* <vector>.  On the IA-32 architecture, an interrupt vector is a value from
* 0 to 255.  This routine merely fills in the appropriate interrupt
* descriptor table (IDT) with an interrupt-gate descriptor such that <routine>
* is invoked when interrupt <vector> is asserted.  The <dpl> argument specifies
* the privilege level for the interrupt-gate descriptor; (hardware) interrupts
* and exceptions should specify a level of 0, whereas handlers for user-mode
* software generated interrupts should specify 3.
*
* RETURNS: N/A
*
* INTERNAL
* Unlike nanoCpuExcConnect() and irq_connect(), the _IntVecSet() routine
* is a very basic API that simply updates the appropriate entry in Interrupt
* Descriptor Table (IDT) such that the specified routine is invoked when the
* specified interrupt vector is asserted.
*
*/

void _IntVecSet(
	unsigned int vector, /* interrupt vector: 0 to 255 on IA-32 */
	void (*routine)(void *),
	unsigned int dpl /* priv level for interrupt-gate descriptor */
	)
{
	unsigned long long *pIdtEntry;

	/*
	 * The <vector> parameter must be less than the value of the
	 * CONFIG_IDT_NUM_VECTORS configuration parameter, however,
	 * explicit
	 * validation will not be performed in this primitive.
	 */

	pIdtEntry = (unsigned long long *)(_idt_base_address + (vector << 3));


	_IdtEntCreate(pIdtEntry, routine, dpl);

/* not required to synchronize the instruction and data caches */

}

/*******************************************************************************
*
* irq_connect - connect a C routine to a hardware interrupt
*
* This routine connects an interrupt service routine (ISR) coded in C to
* the specified hardware <irq>.  An interrupt vector will be allocated to
* satisfy the specified <priority>.  If the interrupt service routine is being
* connected to a software generated interrupt, then <irq> must be set to
* NANO_SOFT_IRQ.
*
* The specified <irq> represents a virtualized IRQ, i.e. it does not
* necessarily represent a specific IRQ line on a given interrupt controller
* device.  The BSP presents a virtualized set of IRQs from 0 to N, where N
* is the total number of IRQs supported by all the interrupt controller devices
* on the board.  See the BSP's documentation for the mapping of virtualized
* IRQ to physical IRQ.
*
* When the device asserts an interrupt on the specified <irq>, a switch to
* the interrupt stack is performed (if not already executing on the interrupt
* stack), followed by saving the integer (i.e. non-floating point) context of
* the currently executing task, fiber, or ISR.  The ISR specified by <routine>
* will then be invoked with the single <parameter>.  When the ISR returns, a
* context switch may occur.
*
* The <pIntStubMem> argument points to memory that the system can use to
* synthesize the interrupt stub that calls <routine>.  The memory need not be
* initialized, but must be persistent (i.e. it cannot be on the caller's stack).
* Declaring a global or static variable of type NANO_INT_STUB will provide a
* suitable area of the proper size.
*
* RETURNS: the allocated interrupt vector
*
* WARNINGS
* Some boards utilize interrupt controllers where the interrupt vector
* cannot be programmed on an IRQ basis; as a result, the vector assigned
* to the <irq> during interrupt controller initialization will be returned.
* In these cases, the requested <priority> is not honoured since the interrupt
* prioritization is fixed by the interrupt controller (e.g. IRQ0 will always
* be the highest priority interrupt regardless of what interrupt vector
* was assigned to IRQ0).
*
* This routine does not perform range checking on the requested <priority>
* and thus, depending on the underlying interrupt controller, may result
* in the assignment of an interrupt vector located in the reserved range of
* the processor.
*
* INTERNAL
* For debug kernels, this routine shall return -1 when there are no
* vectors remaining in the specified <priority> level.
*/

int irq_connect(
	unsigned int irq,		  /* virtualized IRQ to connect to */
	unsigned int priority,		  /* requested priority of interrupt */
	void (*routine)(void *parameter), /* C interrupt handler */
	void *parameter,		  /* parameter passed to C routine */
	NANO_INT_STUB pIntStubMem	 /* memory for synthesized stub code */
	)
{
	unsigned char offsetAdjust;
	unsigned char numParameters = 1; /* stub always pushes ISR parameter */

	extern void _IntEnt(void);
	extern void _IntExit(void);

	int vector;
	NANO_EOI_GET_FUNC boiRtn;
	NANO_EOI_GET_FUNC eoiRtn;
	void *boiRtnParm;
	void *eoiRtnParm;
	unsigned char boiParamRequired;
	unsigned char eoiParamRequired;

#define STUB_PTR pIntStubMem

	/*
	 * Invoke the BSP provided routine _SysIntVecAlloc() which will:
	 *  a) allocate a vector satisfying the requested priority,
	 *  b) return EOI and BOI related information for stub code synthesis,
	 *and
	 *  c) program the underlying interrupt controller device such that
	 *     when <irq> is asserted, the allocated interrupt vector will be
	 *     presented to the CPU.
	 *
	 * The _SysIntVecAlloc() routine will use the "utility" routine
	 * _IntVecAlloc() provided in this module to scan the
	 *_VectorsAllocated[]
	 * array for a suitable vector.
	 */

	vector = _SysIntVecAlloc(irq,
				 priority,
				 &boiRtn,
				 &eoiRtn,
				 &boiRtnParm,
				 &eoiRtnParm,
				 &boiParamRequired,
				 &eoiParamRequired);

#if defined(DEBUG)
	/*
	 * The return value from _SysIntVecAlloc() will be -1 if an invalid
	 * <irq> or <priority> was specified, or if a vector could not be
	 * allocated to honour the requested priority (for the boards that can
	 * support programming the interrupt vector for each IRQ).
	 */

	if (vector == -1)
		return (-1);
#endif /* DEBUG */

	/*
	 * A minimal interrupt stub code will be synthesized based on the
	 * values of <boiRtn>, <eoiRtn>, <boiRtnParm>, <eoiRtnParm>,
	 * <boiParamRequired>, and <eoiParamRequired>.  The invocation of
	 * _IntEnt() and _IntExit() will always be required.
	 */

	STUB_PTR[0] = IA32_CALL_OPCODE;
	UNALIGNED_WRITE((unsigned int *)&STUB_PTR[1],
			(unsigned int)&_IntEnt - (unsigned int)&pIntStubMem[5]);

	offsetAdjust = 5;

#ifdef CONFIG_BOI_HANDLER_SUPPORTED

	/* poke in the BOI related opcodes */

	if (boiRtn == NULL)
		/* no need to insert anything */;
	else if (boiParamRequired != 0) {
		STUB_PTR[offsetAdjust] = IA32_PUSH_OPCODE;
		UNALIGNED_WRITE((unsigned int *)&STUB_PTR[1 + offsetAdjust],
				(unsigned int)boiRtnParm);

		STUB_PTR[5 + offsetAdjust] = IA32_CALL_OPCODE;
		UNALIGNED_WRITE(
			(unsigned int *)&STUB_PTR[6 + offsetAdjust],
			(unsigned int)boiRtn -
				(unsigned int)&pIntStubMem[10 + offsetAdjust]);

		offsetAdjust += 10;
		++numParameters;
	} else {
		STUB_PTR[offsetAdjust] = IA32_CALL_OPCODE;
		UNALIGNED_WRITE(
			(unsigned int *)&STUB_PTR[1 + offsetAdjust],
			(unsigned int)boiRtn -
				(unsigned int)&pIntStubMem[5 + offsetAdjust]);

		offsetAdjust += 5;
	}

#endif /* CONFIG_BOI_HANDLER_SUPPORTED */

	/* IsrParameter and IsrRoutine always required */

	STUB_PTR[offsetAdjust] = IA32_PUSH_OPCODE;
	UNALIGNED_WRITE((unsigned int *)&STUB_PTR[1 + offsetAdjust],
			(unsigned int)parameter);

	STUB_PTR[5 + offsetAdjust] = IA32_CALL_OPCODE;
	UNALIGNED_WRITE((unsigned int *)&STUB_PTR[6 + offsetAdjust],
			(unsigned int)routine -
				(unsigned int)&pIntStubMem[10 + offsetAdjust]);

	offsetAdjust += 10;

#ifdef CONFIG_EOI_HANDLER_SUPPORTED

	/* poke in the EOI related opcodes */

	if (eoiRtn == NULL)
		/* no need to insert anything */;
	else if (eoiParamRequired != 0) {
		STUB_PTR[offsetAdjust] = IA32_PUSH_OPCODE;
		UNALIGNED_WRITE((unsigned int *)&STUB_PTR[1 + offsetAdjust],
				(unsigned int)eoiRtnParm);

		STUB_PTR[5 + offsetAdjust] = IA32_CALL_OPCODE;
		UNALIGNED_WRITE(
			(unsigned int *)&STUB_PTR[6 + offsetAdjust],
			(unsigned int)eoiRtn -
				(unsigned int)&pIntStubMem[10 + offsetAdjust]);

		offsetAdjust += 10;
		++numParameters;
	} else {
		STUB_PTR[offsetAdjust] = IA32_CALL_OPCODE;
		UNALIGNED_WRITE(
			(unsigned int *)&STUB_PTR[1 + offsetAdjust],
			(unsigned int)eoiRtn -
				(unsigned int)&pIntStubMem[5 + offsetAdjust]);

		offsetAdjust += 5;
	}

#endif /* CONFIG_EOI_HANDLER_SUPPORTED */

	/*
	 * Poke in the stack popping related opcode. Do it a byte at a time
	 * because
	 * &STUB_PTR[offsetAdjust] may not be aligned which does not work for
	 * all
	 * targets.
	 */

	STUB_PTR[offsetAdjust] = IA32_ADD_OPCODE & 0xFF;
	STUB_PTR[1 + offsetAdjust] = IA32_ADD_OPCODE >> 8;
	STUB_PTR[2 + offsetAdjust] = (unsigned char)(4 * numParameters);

	offsetAdjust += 3;

	/*
	 * generate code that invokes _IntExit(); note that a jump is used,
	 * since _IntExit() takes care of returning back to the context that
	 * experienced the interrupt (i.e. branch tail optimization)
	 */

	STUB_PTR[offsetAdjust] = IA32_JMP_OPCODE;
	UNALIGNED_WRITE((unsigned int *)&STUB_PTR[1 + offsetAdjust],
			(unsigned int)&_IntExit -
				(unsigned int)&pIntStubMem[5 + offsetAdjust]);


	/*
	 * There is no need to explicitly synchronize or flush the instruction
	 * cache due to the above code synthesis.  See the Intel 64 and IA-32
	 * Architectures Software Developer's Manual: Volume 3A: System
	 *Programming
	 * Guide; specifically the section titled "Self Modifying Code".
	 *
	 * Cache synchronization/flushing is not required for the i386 as it
	 * does not contain any on-chip I-cache; likewise, post-i486 processors
	 * invalidate the I-cache automatically.  An i486 requires the CPU
	 * to perform a 'jmp' instruction before executing the synthesized code;
	 * however, the call and return that follows meets this requirement.
	 */

	_IntVecSet(vector, (void (*)(void *))pIntStubMem, 0);

	return vector;
}

/*******************************************************************************
*
* _IntVecAlloc - allocate a free interrupt vector given <priority>
*
* This routine scans the _VectorsAllocated[] array for a free vector that
* satisfies the specified <priority>.  It is a utility function for use only
* by a BSP's _SysIntVecAlloc() routine.
*
* This routine assumes that the relationship between interrupt priority and
* interrupt vector is :
*
*      priority = vector / 16;
*
* Since vectors 0 to 31 are reserved by the IA-32 architecture, the priorities
* of user defined interrupts range from 2 to 15.  Each interrupt priority level
* contains 16 vectors, and the prioritization of interrupts within a priority
* level is determined by the vector number; the higher the vector number, the
* higher the priority within that priority level.
*
* It is also assumed that the interrupt controllers are capable of managing
* interrupt requests on a per-vector level as opposed to a per-priority level.
* For example, the local APIC on Pentium4 and later processors, the in-service
* register (ISR) and the interrupt request register (IRR) are 256 bits wide.
*
* RETURNS: allocated interrupt vector
*
* INTERNAL
* For debug kernels, this routine shall return -1 when there are no
* vectors remaining in the specified <priority> level.
*/

int _IntVecAlloc(unsigned int priority)
{
	unsigned int imask;
	unsigned int entryToScan;
	unsigned int fsb; /* first set bit in entry */
	int vector;

#if defined(DEBUG)
	/*
	 * check whether the IDT was configured with sufficient vectors to
	 * satisfy the priority request.
	 */

	if (((priority << 4) + 15) > CONFIG_IDT_NUM_VECTORS)
		return (-1);
#endif /* DEBUG */

	/*
	 * Atomically allocate a vector from the _VectorsAllocated[] array
	 * to prevent race conditions with other tasks/fibers attempting to
	 * allocate an interrupt vector.
	 */

	entryToScan = priority >> 1; /* _VectorsAllocated[] entry to scan */

	/*
	 * The _VectorsAllocated[] entry specified by 'entryToScan' is a 32-bit
	 * quantity and thus represents the vectors for a pair of priority
	 *levels.
	 * Use find_last_set() to scan for the upper of the 2, and find_first_set() to
	 *scan
	 * for the lower of the 2 priorities.
	 *
	 * Note that find_first_set/find_last_set returns bit position from 1 to 32,
	 * or 0 if the argument is zero.
	 */

	imask = irq_lock();

	if ((priority % 2) == 0) {
		/* scan from the LSB for even priorities */

		fsb = find_first_set(_VectorsAllocated[entryToScan]);

#if defined(DEBUG)
		if ((fsb == 0) || (fsb > 16)) {
			/*
			 * No bits are set in the lower 16 bits, thus all
			 * vectors for this
			 * priority have been allocated.
			 */

			irq_unlock(imask);
			return (-1);
		}
#endif /* DEBUG */
	} else {
		/* scan from the MSB for odd priorities */

		fsb = find_last_set(_VectorsAllocated[entryToScan]);

#if defined(DEBUG)
		if ((fsb == 0) || (fsb < 17)) {
			/*
			 * No bits are set in the lower 16 bits, thus all
			 * vectors for this
			 * priority have been allocated.
			 */

			irq_unlock(imask);
			return (-1);
		}
#endif /* DEBUG */
	}

	/* ffsLsb/ffsMsb returns bit positions as 1 to 32 */

	--fsb;

	/* mark the vector as allocated */

	_VectorsAllocated[entryToScan] &= ~(1 << fsb);

	irq_unlock(imask);

	/* compute vector given allocated bit within the priority level */

	vector = (entryToScan << 5) + fsb;

	return vector;
}

/*******************************************************************************
*
* _IntVecMarkAllocated - mark interrupt vector as allocated
*
* This routine is used to "reserve" an interrupt vector that is allocated
* or assigned by any means other than _IntVecAllocate().  This marks the vector
* as allocated so that any future invocations of _IntVecAllocate() will not
* return that vector.
*
* RETURNS: N/A
*
*/

void _IntVecMarkAllocated(unsigned int vector)
{
	unsigned int entryToSet = vector / 32;
	unsigned int bitToSet = vector % 32;
	unsigned int imask;

	imask = irq_lock();
	_VectorsAllocated[entryToSet] &= ~(1 << bitToSet);
	irq_unlock(imask);
}

/*******************************************************************************
*
* _IntVecMarkFree - mark interrupt vector as free
*
* This routine is used to "free" an interrupt vector that is allocated
* or assigned using _IntVecAllocate() or _IntVecMarkAllocated(). This marks the
* vector as available so that any future allocations can return that vector.
*
*/

void _IntVecMarkFree(unsigned int vector)
{
	unsigned int entryToSet = vector / 32;
	unsigned int bitToSet = vector % 32;
	unsigned int imask;

	imask = irq_lock();
	_VectorsAllocated[entryToSet] |= (1 << bitToSet);
	irq_unlock(imask);
}

#endif /* CONFIG_NO_ISRS */
