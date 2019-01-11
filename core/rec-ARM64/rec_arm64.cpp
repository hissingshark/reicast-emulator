/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT

#include <unistd.h>
#include <sys/mman.h>

#include "deps/vixl/aarch64/macro-assembler-aarch64.h"
using namespace vixl::aarch64;

#include "hw/sh4/sh4_opcode_list.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "arm64_regalloc.h"

struct DynaRBI : RuntimeBlockInfo
{
	virtual u32 Relink() {
		return 0;
	}

	virtual void Relocate(void* dst) {
		verify(false);
	}
};

// Code borrowed from Dolphin https://github.com/dolphin-emu/dolphin
static void CacheFlush(void* start, void* end)
{
	if (start == end)
		return;

#if HOST_OS == OS_DARWIN
	// Header file says this is equivalent to: sys_icache_invalidate(start, end - start);
	sys_cache_control(kCacheFunctionPrepareForExecution, start, end - start);
#else
	// Don't rely on GCC's __clear_cache implementation, as it caches
	// icache/dcache cache line sizes, that can vary between cores on
	// big.LITTLE architectures.
	u64 addr, ctr_el0;
	static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
	size_t isize, dsize;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
	isize = 4 << ((ctr_el0 >> 0) & 0xf);
	dsize = 4 << ((ctr_el0 >> 16) & 0xf);

	// use the global minimum cache line size
	icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
	dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

	addr = (u64)start & ~(u64)(dsize - 1);
	for (; addr < (u64)end; addr += dsize)
		// use "civac" instead of "cvau", as this is the suggested workaround for
		// Cortex-A53 errata 819472, 826319, 827319 and 824069.
		__asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
	__asm__ volatile("dsb ish" : : : "memory");

	addr = (u64)start & ~(u64)(isize - 1);
	for (; addr < (u64)end; addr += isize)
		__asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

	__asm__ volatile("dsb ish" : : : "memory");
	__asm__ volatile("isb" : : : "memory");
#endif
}

static void ngen_FailedToFindBlock_internal() {
	rdv_FailedToFindBlock(Sh4cntx.pc);
}

void(*ngen_FailedToFindBlock)() = &ngen_FailedToFindBlock_internal;

extern "C" {

void *bm_GetCodeInternal(u32 pc)
{
	return (void*)bm_GetCode(pc);
}

void UpdateSystemInternal(u32 pc)
{
	if (UpdateSystem())
		rdv_DoInterrupts_pc(pc);
}

}

void ngen_mainloop(void* v_cntx)
{
	Sh4RCB* ctx = (Sh4RCB*)((u8*)v_cntx - sizeof(Sh4RCB));

	__asm__ volatile
	(
		"stp x19, x20, [sp, #-144]!	\n\t"
		"stp x21, x22, [sp, #16]	\n\t"
		"stp x23, x24, [sp, #32]	\n\t"
		"stp x25, x26, [sp, #48]	\n\t"
		"stp x27, x28, [sp, #64]	\n\t"
		"stp s8, s9, [sp, #80]		\n\t"
		"stp s10, s11, [sp, #96]	\n\t"
		"stp s12, s13, [sp, #112]	\n\t"
		"stp s14, s15, [sp, #128]	\n\t"
		// Use x28 as sh4 context pointer
		"mov x28, %0				\n\t"
		// Use x27 as cycle_counter
		"mov w27, %2				\n\t"	// SH4_TIMESLICE

	"run_loop:						\n\t"
		"ldr w0, [x28, %3]			\n\t"	// CpuRunning
		"cmp w0, #0					\n\t"
		"b.eq end_run_loop			\n\t"

	"slice_loop:					\n\t"
		"ldr w0, [x28, %1]			\n\t"	// pc
		"bl bm_GetCodeInternal		\n\t"
		"blr x0						\n\t"
		"cmp w27, #0				\n\t"
		"b.gt slice_loop			\n\t"

		"add w27, w27, %2			\n\t"	// SH4_TIMESLICE
		"ldr w0, [x28, %1]			\n\t"	// pc
		"bl UpdateSystemInternal	\n\t"
		"b run_loop					\n\t"

	"end_run_loop:					\n\t"
		"ldp s14, s15, [sp, #128]	\n\t"
		"ldp s12, s13, [sp, #112]	\n\t"
		"ldp s10, s11, [sp, #96]	\n\t"
		"ldp s8, s9, [sp, #80]		\n\t"
		"ldp x27, x28, [sp, #64]	\n\t"
		"ldp x25, x26, [sp, #48]	\n\t"
		"ldp x23, x24, [sp, #32]	\n\t"
		"ldp x21, x22, [sp, #16]	\n\t"
		"ldp x19, x20, [sp], #144	\n\t"
		:
		: "r"(reinterpret_cast<uintptr_t>(&ctx->cntx)),
		  "i"(offsetof(Sh4Context, pc)),
		  "i"(SH4_TIMESLICE),
		  "i"(offsetof(Sh4Context, CpuRunning))
		: "memory"
	);
}

void ngen_init()
{
}

void ngen_ResetBlocks()
{
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds = false;
}

RuntimeBlockInfo* ngen_AllocateBlock()
{
	return new DynaRBI();
}

u32* GetRegPtr(u32 reg)
{
	return Sh4_int_GetRegisterPtr((Sh4RegType)reg);
}

void ngen_blockcheckfail(u32 pc) {
	printf("arm64 JIT: SMC invalidation at %08X\n", pc);
	rdv_BlockCheckFail(pc);
}

class Arm64Assembler : public MacroAssembler
{
	typedef void (MacroAssembler::*Arm64Op)(const Register&, const Register&, const Operand&);
	typedef void (MacroAssembler::*Arm64Op2)(const Register&, const Register&, const Register&);
	typedef void (MacroAssembler::*Arm64Op3)(const Register&, const Register&, const Operand&, enum FlagsUpdate);

public:
	Arm64Assembler() : MacroAssembler((u8 *)emit_GetCCPtr(), 64 * 1024), regalloc(this)
	{
		call_regs.push_back(&w0);
		call_regs.push_back(&w1);
		call_regs.push_back(&w2);
		call_regs.push_back(&w3);
		call_regs.push_back(&w4);
		call_regs.push_back(&w5);
		call_regs.push_back(&w6);
		call_regs.push_back(&w7);

		call_regs64.push_back(&x0);
		call_regs64.push_back(&x1);
		call_regs64.push_back(&x2);
		call_regs64.push_back(&x3);
		call_regs64.push_back(&x4);
		call_regs64.push_back(&x5);
		call_regs64.push_back(&x6);
		call_regs64.push_back(&x7);

		call_fregs.push_back(&s0);
		call_fregs.push_back(&s1);
		call_fregs.push_back(&s2);
		call_fregs.push_back(&s3);
		call_fregs.push_back(&s4);
		call_fregs.push_back(&s5);
		call_fregs.push_back(&s6);
		call_fregs.push_back(&s7);
	}

	void ngen_BinaryOp(shil_opcode* op, Arm64Op arm_op, Arm64Op2 arm_op2, Arm64Op3 arm_op3)
	{
		const Register* reg3 = &wzr;
		if (op->rs2.is_imm())
		{
			Mov(w10, op->rs2._imm);
			reg3 = &w10;
		}
		else if (op->rs2.is_r32i())
		{
			reg3 = &regalloc.MapRegister(op->rs2);
		}
		if (arm_op != NULL)
			((*this).*arm_op)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), *reg3);
		else if (arm_op2 != NULL)
			((*this).*arm_op2)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), *reg3);
		else
			((*this).*arm_op3)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), *reg3, LeaveFlags);
	}

	void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
	{
		//printf("REC-ARM64 compiling %08x\n", block->addr);
		if (force_checks)
			CheckBlock(block);

		Str(x30, MemOperand(sp, -16, PreIndex));

		// run register allocator
		regalloc.DoAlloc(block);

		// scheduler
		Sub(w27, w27, block->guest_cycles);

		for (size_t i = 0; i < block->oplist.size(); i++)
		{
			shil_opcode& op  = block->oplist[i];
			regalloc.OpBegin(&op, i);

			switch (op.op)
			{
			case shop_ifb:	// Interpreter fallback
				if (op.rs1._imm)	// if NeedPC()
				{
					Mov(w10, op.rs2._imm);
					Str(w10, sh4_context_mem_operand(&next_pc));
				}
				Mov(*call_regs[0], op.rs3._imm);

				CallRuntime(OpDesc[op.rs3._imm]->oph);
				break;

			case shop_jcond:
			case shop_jdyn:
				Mov(w10, regalloc.MapRegister(op.rs1));

				if (op.rs2.is_imm()) {
					Mov(w9, op.rs2._imm);
					Add(w10, w10, w9);
				}

				Mov(regalloc.MapRegister(op.rd), w10);
				break;

			case shop_mov32:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

				if (regalloc.IsAllocf(op.rd))
				{
					if (op.rs1.is_imm())
						Fmov(regalloc.MapVRegister(op.rd), (float&)op.rs1._imm);
					else if (regalloc.IsAllocf(op.rs1))
						Fmov(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
					else
						Fmov(regalloc.MapVRegister(op.rd), regalloc.MapRegister(op.rs1));
				}
				else
				{
					if (op.rs1.is_imm())
						Mov(regalloc.MapRegister(op.rd), op.rs1._imm);
					else if (regalloc.IsAllocg(op.rs1))
						Mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
					else
						Fmov(regalloc.MapRegister(op.rd), regalloc.MapVRegister(op.rs1));
				}
				break;

			case shop_mov64:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

				shil_param_to_host_reg(op.rs1, x15);
				host_reg_to_shil_param(op.rd, x15);
				break;

			case shop_readm:
			{
				u32 size = op.flags & 0x7f;
				bool is_float = op.rs2.is_r32f() || op.rd.is_r32f();

				if (op.rs1.is_imm())
				{
					bool isram = false;
					void* ptr = _vmem_read_const(op.rs1._imm, isram, size);

					if (isram)
					{
						Mov(x1, reinterpret_cast<uintptr_t>(ptr));
						switch (size)
						{
						case 2:
							Ldrsh(regalloc.MapRegister(op.rd), MemOperand(x1, xzr, SXTW));
							break;

						case 4:
							if (is_float)
								Ldr(regalloc.MapVRegister(op.rd), MemOperand(x1));
							else
								Ldr(regalloc.MapRegister(op.rd), MemOperand(x1));
							break;

						default:
							die("Invalid size");
							break;
						}
					}
					else
					{
						// Not RAM
						Mov(w0, op.rs1._imm);

						switch(size)
						{
						case 1:
							CallRuntime((void (*)())ptr);
							Sxtb(w0, w0);
							break;

						case 2:
							CallRuntime((void (*)())ptr);
							Sxth(w0, w0);
							break;

						case 4:
							CallRuntime((void (*)())ptr);
							break;

						case 8:
							die("SZ_64F not supported");
							break;
						}

						if (regalloc.IsAllocg(op.rd))
							Mov(regalloc.MapRegister(op.rd), w0);
						else
							Fmov(regalloc.MapVRegister(op.rd), w0);
					}
				}
				else
				{
#if 0	// Direct memory access. Need to handle SIGSEGV and rewrite block as needed (?)
					const Register& raddr = GenMemAddr(&op);

					if (_nvmem_enabled())
					{
						Add(w1, raddr, sizeof(Sh4Context));
						Bfc(w1, 29, 3);		// addr &= ~0xE0000000

						switch(size)
						{
						case 1:
							Ldrsb(regalloc.MapRegister(op.rd), MemOperand(x28, x1, SXTW));
							break;

						case 2:
							Ldrsh(regalloc.MapRegister(op.rd), MemOperand(x28, x1, SXTW));
							break;

						case 4:
							if (!is_float)
								Ldr(regalloc.MapRegister(op.rd), MemOperand(x28, x1));
							else
								Ldr(regalloc.MapVRegister(op.rd), MemOperand(x28, x1));
							break;

						case 8:
							// TODO use regalloc
							Ldr(x0, MemOperand(x28, x1));
							Str(x0, sh4_context_mem_operand(op.rd.reg_ptr()));
							break;
						}
					}
					else
					{
						// TODO
						die("Not implemented")
					}
#endif

					shil_param_to_host_reg(op.rs1, *call_regs[0]);
					if (!op.rs3.is_null())
					{
						shil_param_to_host_reg(op.rs3, w10);
						Add(*call_regs[0], *call_regs[0], w10);
					}

					switch (size)
					{
					case 1:
						CallRuntime(ReadMem8);
						Sxtb(w0, w0);
						break;

					case 2:
						CallRuntime(ReadMem16);
						Sxth(w0, w0);
						break;

					case 4:
						CallRuntime(ReadMem32);
						break;

					case 8:
						CallRuntime(ReadMem64);
						break;

					default:
						die("1..8 bytes");
						break;
					}

					if (size != 8)
						host_reg_to_shil_param(op.rd, w0);
					else
						host_reg_to_shil_param(op.rd, x0);
				}
			}
			break;

			case shop_writem:
			{
				shil_param_to_host_reg(op.rs1, *call_regs[0]);
				if (!op.rs3.is_null())
				{
					shil_param_to_host_reg(op.rs3, w10);
					Add(*call_regs[0], *call_regs[0], w10);
				}

				u32 size = op.flags & 0x7f;
				if (size != 8)
					shil_param_to_host_reg(op.rs2, *call_regs[1]);
				else
					shil_param_to_host_reg(op.rs2, *call_regs64[1]);

				switch (size)
				{
				case 1:
					CallRuntime(WriteMem8);
					break;

				case 2:
					CallRuntime(WriteMem16);
					break;

				case 4:
					CallRuntime(WriteMem32);
					break;

				case 8:
					CallRuntime(WriteMem64);
					break;

				default:
					die("1..8 bytes");
					break;
				}
			}
			break;

			case shop_sync_sr:
				CallRuntime(UpdateSR);
				break;

			case shop_neg:
				Neg(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_not:
				Mvn(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			case shop_shl:
				ngen_BinaryOp(&op, NULL, &MacroAssembler::Lsl, NULL);
				break;
			case shop_shr:
				ngen_BinaryOp(&op,  NULL, &MacroAssembler::Lsr, NULL);
				break;
			case shop_sar:
				ngen_BinaryOp(& op,  NULL, &MacroAssembler::Asr, NULL);
				break;
			case shop_and:
				ngen_BinaryOp(&op, &MacroAssembler::And, NULL, NULL);
				break;
			case shop_or:
				ngen_BinaryOp(&op, &MacroAssembler::Orr, NULL, NULL);
				break;
			case shop_xor:
				ngen_BinaryOp(&op, &MacroAssembler::Eor, NULL, NULL);
				break;
			case shop_add:
				ngen_BinaryOp(&op, NULL, NULL, &MacroAssembler::Add);
				break;
			case shop_sub:
				ngen_BinaryOp(&op, NULL, NULL, &MacroAssembler::Sub);
				break;
			case shop_ror:
				ngen_BinaryOp(&op, NULL, &MacroAssembler::Ror, NULL);
				break;

			case shop_adc:
				Cmp(regalloc.MapRegister(op.rs3), 1);	// C = rs3
				Adcs(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2)); // (C,rd)=rs1+rs2+rs3(C)
				Cset(regalloc.MapRegister(op.rd2), cs);	// rd2 = C
				break;
			case shop_sbc:
				Cmp(wzr, regalloc.MapRegister(op.rs3));	// C = ~rs3
				Sbcs(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2)); // (C,rd) = rs1 - rs2 - ~rs3(C)
				Cset(regalloc.MapRegister(op.rd2), cc);	// rd2 = ~C
				break;

			case shop_rocr:
				Ubfm(w0, regalloc.MapRegister(op.rs1), 0, 0);
				Mov(regalloc.MapRegister(op.rd), Operand(regalloc.MapRegister(op.rs1), LSR, 1));
				Orr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), Operand(regalloc.MapRegister(op.rs2), LSL, 31));
				Mov(regalloc.MapRegister(op.rd2), w0);
				break;
			case shop_rocl:
				Tst(regalloc.MapRegister(op.rs1), 0x80000000);	// Z = ~rs1[31]
				Orr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs2), Operand(regalloc.MapRegister(op.rs1), LSL, 1)); // rd = rs1 << 1 | rs2(C)
				Cset(regalloc.MapRegister(op.rd2), ne);			// rd2 = ~Z(C)
				break;

			case shop_shld:
				// TODO optimize
				Cmp(regalloc.MapRegister(op.rs2), 0);
				Csel(w1, regalloc.MapRegister(op.rs2), wzr, ge);
				Mov(w0, wzr);	// wzr not supported by csneg
				Csneg(w2, w0, regalloc.MapRegister(op.rs2), ge);
				Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), w1);
				Lsr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2);
				break;
			case shop_shad:
				// TODO optimize
				Cmp(regalloc.MapRegister(op.rs2), 0);
				Csel(w1, regalloc.MapRegister(op.rs2), wzr, ge);
				Mov(w0, wzr);	// wzr not supported by csneg
				Csneg(w2, w0, regalloc.MapRegister(op.rs2), ge);
				Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), w1);
				Asr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2);
				break;

			case shop_test:
			case shop_seteq:
			case shop_setge:
			case shop_setgt:
			case shop_setae:
			case shop_setab:
				{
					if (op.op == shop_test)
					{
						if (op.rs2.is_imm())
							Tst(regalloc.MapRegister(op.rs1), op.rs2._imm);
						else
							Tst(regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					}
					else
					{
						if (op.rs2.is_imm())
							Cmp(regalloc.MapRegister(op.rs1), op.rs2._imm);
						else
							Cmp(regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					}

					static const Condition shop_conditions[] = { eq, eq, ge, gt, hs, hi };

					Cset(regalloc.MapRegister(op.rd), shop_conditions[op.op - shop_test]);
				}
				break;
			case shop_setpeq:
				Eor(w1, regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));

				Mov(regalloc.MapRegister(op.rd), wzr);
				Mov(w2, wzr);	// wzr not supported by csinc (?!)
				Tst(w1, 0xFF000000);
				Csinc(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2, ne);
				Tst(w1, 0x00FF0000);
				Csinc(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2, ne);
				Tst(w1, 0x0000FF00);
				Csinc(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2, ne);
				Tst(w1, 0x000000FF);
				Csinc(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rd), w2, ne);
				break;

			case shop_mul_u16:
				Uxth(w10, regalloc.MapRegister(op.rs1));
				Uxth(w11, regalloc.MapRegister(op.rs2));
				Mul(regalloc.MapRegister(op.rd), w10, w11);
				break;
			case shop_mul_s16:
				Sxth(w10, regalloc.MapRegister(op.rs1));
				Sxth(w11, regalloc.MapRegister(op.rs2));
				Mul(regalloc.MapRegister(op.rd), w10, w11);
				break;
			case shop_mul_i32:
				Mul(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_mul_u64:
			case shop_mul_s64:
				{
					const Register& rd_xreg = Register::GetXRegFromCode(regalloc.MapRegister(op.rd).GetCode());
					if (op.op == shop_mul_u64)
						Umull(rd_xreg, regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					else
						Smull(rd_xreg, regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					const Register& rd2_xreg = Register::GetXRegFromCode(regalloc.MapRegister(op.rd2).GetCode());
					Lsr(rd2_xreg, rd_xreg, 32);
				}
				break;

			case shop_ext_s8:
				Sxtb(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_ext_s16:
				Sxth(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			//
			// FPU
			//

			case shop_fadd:
				Fadd(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				break;
			case shop_fsub:
				Fsub(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				break;
			case shop_fmul:
				Fmul(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				break;
			case shop_fdiv:
				Fdiv(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				break;

			case shop_fabs:
				Fabs(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fneg:
				Fneg(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fsqrt:
				Fsqrt(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fmac:
				Fmadd(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs3), regalloc.MapVRegister(op.rs2), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fsrra:
				Fsqrt(s0, regalloc.MapVRegister(op.rs1));
				Fmov(s1, 1.f);
				Fdiv(regalloc.MapVRegister(op.rd), s1, s0);
				break;

			case shop_fsetgt:
			case shop_fseteq:
				Fcmp(regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				Cset(regalloc.MapRegister(op.rd), op.op == shop_fsetgt ? gt : eq);
				break;

			case shop_fsca:
				Mov(x1, reinterpret_cast<uintptr_t>(&sin_table));
				Add(x1, x1, Operand(regalloc.MapRegister(op.rs1), UXTH, 3));
				//Ldr(regalloc.MapVRegister(op.rd, 0), MemOperand(x1, 4, PostIndex));
				//Ldr(regalloc.MapVRegister(op.rd, 1), MemOperand(x1));
				regalloc.writeback_fpu += 2;
				Ldr(w2, MemOperand(x1, 4, PostIndex));
				Str(w2, sh4_context_mem_operand(op.rd.reg_ptr()));				// TODO use regalloc
				Ldr(w2, MemOperand(x1));
				Str(w2, sh4_context_mem_operand(GetRegPtr(op.rd._reg + 1)));	// TODO use regalloc
				break;

			case shop_fipr:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Ld1(v0.V4S(), MemOperand(x9));
				if (op.rs1._reg != op.rs2._reg)
				{
					Add(x9, x28, sh4_context_mem_operand(op.rs2.reg_ptr()).GetOffset());
					Ld1(v1.V4S(), MemOperand(x9));
					Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				}
				else
					Fmul(v0.V4S(), v0.V4S(), v0.V4S());
				Faddp(v1.V4S(), v0.V4S(), v0.V4S());
				Faddp(regalloc.MapVRegister(op.rd), v1.V2S());
				break;

			case shop_ftrv:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Ld1(v0.V4S(), MemOperand(x9));
				Add(x9, x28, sh4_context_mem_operand(op.rs2.reg_ptr()).GetOffset());
				Ld1(v1.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v2.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v3.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v4.V4S(), MemOperand(x9, 16, PostIndex));
				Fmul(v5.V4S(), v1.V4S(), s0, 0);
				Fmla(v5.V4S(), v2.V4S(), s0, 1);
				Fmla(v5.V4S(), v3.V4S(), s0, 2);
				Fmla(v5.V4S(), v4.V4S(), s0, 3);
				Add(x9, x28, sh4_context_mem_operand(op.rd.reg_ptr()).GetOffset());
				St1(v5.V4S(), MemOperand(x9));
				break;

			case shop_frswap:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Add(x10, x28, sh4_context_mem_operand(op.rd.reg_ptr()).GetOffset());
				Ld4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x9));
				Ld4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x10));
				St4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x9));
				St4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x10));
				break;

			case shop_cvt_f2i_t:
				Fcvtzs(regalloc.MapRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_cvt_i2f_n:
			case shop_cvt_i2f_z:
				Scvtf(regalloc.MapVRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			default:
				shil_chf[op.op](&op);
				break;
			}
			regalloc.OpEnd(&op);
		}

		switch (block->BlockType)
		{

		case BET_StaticJump:
		case BET_StaticCall:
			// next_pc = block->BranchBlock;
			Ldr(w10, block->BranchBlock);
			Str(w10, sh4_context_mem_operand(&next_pc));
			break;

		case BET_Cond_0:
		case BET_Cond_1:
			{
				// next_pc = next_pc_value;
				// if (*jdyn == 0)
				//   next_pc = branch_pc_value;

				Mov(w10, block->NextBlock);

				if (block->has_jcond)
					Ldr(w11, sh4_context_mem_operand(&Sh4cntx.jdyn));
				else
					Ldr(w11, sh4_context_mem_operand(&sr.T));

				Cmp(w11, block->BlockType & 1);
				Label branch_not_taken;

				B(ne, &branch_not_taken);
				Mov(w10, block->BranchBlock);
				Bind(&branch_not_taken);

				Str(w10, sh4_context_mem_operand(&next_pc));
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			// next_pc = *jdyn;
			Ldr(w10, sh4_context_mem_operand(&Sh4cntx.jdyn));
			Str(w10, sh4_context_mem_operand(&next_pc));
			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_DynamicIntr)
			{
				// next_pc = *jdyn;
				Ldr(w10, sh4_context_mem_operand(&Sh4cntx.jdyn));
			}
			else
			{
				// next_pc = next_pc_value;
				Mov(w10, block->NextBlock);
			}
			Str(w10, sh4_context_mem_operand(&next_pc));

			CallRuntime(UpdateINTC);
			break;

		default:
			die("Invalid block end type");
		}

		Ldr(x30, MemOperand(sp, 16, PostIndex));
		Ret();

		Label code_end;
		Bind(&code_end);

		FinalizeCode();

		block->code = GetBuffer()->GetStartAddress<DynarecCodeEntryPtr>();
		block->host_code_size = GetBuffer()->GetSizeInBytes();
		block->host_opcodes = GetLabelAddress<u32*>(&code_end) - GetBuffer()->GetStartAddress<u32*>();

		emit_Skip(block->host_code_size);
		CacheFlush((void*)block->code, GetBuffer()->GetEndAddress<void*>());
#if 0
		Instruction* instr_start = GetBuffer()->GetStartAddress<Instruction*>();
		Instruction* instr_end = GetLabelAddress<Instruction*>(&code_end);
		Decoder decoder;
		Disassembler disasm;
		decoder.AppendVisitor(&disasm);
		Instruction* instr;
		for (instr = instr_start; instr < instr_end; instr += kInstructionSize) {
			decoder.Decode(instr);
			printf("VIXL\t %p:\t%s\n",
			           reinterpret_cast<void*>(instr),
			           disasm.GetOutput());
		}
#endif
	}

	void ngen_CC_Start(shil_opcode* op)
	{
		CC_pars.clear();
	}

	void ngen_CC_Param(shil_opcode& op, shil_param& prm, CanonicalParamType tp)
	{
		switch (tp)
		{

		case CPT_u32:
		case CPT_ptr:
		case CPT_f32:
		{
			CC_PS t = { tp, &prm };
			CC_pars.push_back(t);
		}
		break;

		case CPT_u64rvL:
		case CPT_u32rv:
			host_reg_to_shil_param(prm, w0);
			break;

		case CPT_u64rvH:
			Lsr(x10, x0, 32);
			host_reg_to_shil_param(prm, w10);
			break;

		case CPT_f32rv:
			host_reg_to_shil_param(prm, s0);
			break;
		}
	}

	void ngen_CC_Call(shil_opcode*op, void* function)
	{
		int regused = 0;
		int fregused = 0;

		// Args are pushed in reverse order by shil_canonical
		for (int i = CC_pars.size(); i-- > 0;)
		{
			verify(fregused < call_fregs.size() && regused < call_regs.size());
			shil_param& prm = *CC_pars[i].prm;
			switch (CC_pars[i].type)
			{
			// push the params

			case CPT_u32:
				shil_param_to_host_reg(prm, *call_regs[regused++]);

				break;

			case CPT_f32:
				if (prm.is_reg()) {
					Fmov(*call_fregs[fregused], regalloc.MapVRegister(prm));
				}
				else {
					verify(prm.is_null());
				}
				fregused++;
				break;

			case CPT_ptr:
				verify(prm.is_reg());
				// push the ptr itself
				Mov(*call_regs64[regused++], reinterpret_cast<uintptr_t>(prm.reg_ptr()));

				break;
			case CPT_u32rv:
			case CPT_u64rvL:
			case CPT_u64rvH:
			case CPT_f32rv:
				// return values are handled in ngen_CC_param()
				break;
			}
		}
		CallRuntime((void (*)())function);
	}

	MemOperand sh4_context_mem_operand(void *p)
	{
		u32 offset = (u8*)p - (u8*)&p_sh4rcb->cntx;
		verify((offset & 3) == 0 && offset <= 16380);	// FIXME 64-bit regs need multiple of 8 up to 32760
		return MemOperand(x28, offset);
	}

private:
	void CheckBlock(RuntimeBlockInfo* block)
	{
		s32 sz = block->sh4_code_size;

		Label blockcheck_fail;
		Label blockcheck_success;

		u8* ptr = GetMemPtr(block->addr, sz);
		if (ptr == NULL)
			// FIXME Can a block cross a RAM / non-RAM boundary??
			return;

		Mov(x9, reinterpret_cast<uintptr_t>(ptr));

		while (sz > 0)
		{
			if (sz >= 8 && (reinterpret_cast<uintptr_t>(ptr) & 7) == 0)
			{
				Ldr(x10, MemOperand(x9, 8, PostIndex));
				Ldr(x11, *(u64*)ptr);
				Cmp(x10, x11);
				sz -= 8;
				ptr += 8;
			}
			else if (sz >= 4 && (reinterpret_cast<uintptr_t>(ptr) & 3) == 0)
			{
				Ldr(w10, MemOperand(x9, 4, PostIndex));
				Ldr(w11, *(u32*)ptr);
				Cmp(w10, w11);
				sz -= 4;
				ptr += 4;
			}
			else
			{
				Ldrh(w10, MemOperand(x9, 2, PostIndex));
				Mov(w11, *(u16*)ptr);
				Cmp(w10, w11);
				sz -= 2;
				ptr += 2;
			}
			B(ne, &blockcheck_fail);
		}
		B(&blockcheck_success);

		Bind(&blockcheck_fail);
		Ldr(w0, block->addr);
		TailCallRuntime(ngen_blockcheckfail);

		Bind(&blockcheck_success);
	}

	void shil_param_to_host_reg(const shil_param& param, const Register& reg)
	{
		if (param.is_imm())
		{
			Mov(reg, param._imm);
		}
		else if (param.is_reg())
		{
			if (param.is_r64f())
			{
				// TODO use regalloc
				Ldr(reg, sh4_context_mem_operand(param.reg_ptr()));
			}
			else if (param.is_r32f())
				Fmov(reg, regalloc.MapVRegister(param));
			else
				Mov(reg, regalloc.MapRegister(param));
		}
		else
		{
			verify(param.is_null());
		}
	}

	void host_reg_to_shil_param(const shil_param& param, const CPURegister& reg)
	{
		if (reg.Is64Bits())
		{
			// TODO use regalloc
			Str((const Register&)reg, sh4_context_mem_operand(param.reg_ptr()));
		}
		else if (regalloc.IsAllocg(param))
		{
			if (reg.IsRegister())
				Mov(regalloc.MapRegister(param), (const Register&)reg);
			else
				Fmov(regalloc.MapRegister(param), (const VRegister&)reg);
		}
		else
		{
			if (reg.IsVRegister())
				Fmov(regalloc.MapVRegister(param), (const VRegister&)reg);
			else
				Fmov(regalloc.MapVRegister(param), (const Register&)reg);
		}
	}

	struct CC_PS
	{
		CanonicalParamType type;
		shil_param* prm;
	};
	vector<CC_PS> CC_pars;
	std::vector<const WRegister*> call_regs;
	std::vector<const XRegister*> call_regs64;
	std::vector<const VRegister*> call_fregs;
	Arm64RegAlloc regalloc;
};

static Arm64Assembler* compiler;

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	verify(emit_FreeSpace() >= 16 * 1024);

	compiler = new Arm64Assembler();

	compiler->ngen_Compile(block, force_checks, reset, staging, optimise);

	delete compiler;
	compiler = NULL;
}

void ngen_CC_Start(shil_opcode* op)
{
	compiler->ngen_CC_Start(op);
}

void ngen_CC_Param(shil_opcode* op, shil_param* par, CanonicalParamType tp)
{
	compiler->ngen_CC_Param(*op, *par, tp);
}

void ngen_CC_Call(shil_opcode*op, void* function)
{
	compiler->ngen_CC_Call(op, function);
}

void ngen_CC_Finish(shil_opcode* op)
{

}

void Arm64RegAlloc::Preload(u32 reg, eReg nreg)
{
	assembler->Ldr(Register(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Writeback(u32 reg, eReg nreg)
{
	assembler->Str(Register(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Preload_FPU(u32 reg, eFReg nreg)
{
	assembler->Ldr(VRegister(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Writeback_FPU(u32 reg, eFReg nreg)
{
	assembler->Str(VRegister(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}


extern "C" void do_sqw_nommu_area_3(u32 dst, u8* sqb)
{
	__asm__ volatile
	(
		"movz x11, #0x0C00, lsl #16 \n\t"
		"add x11, x1, x11			\n\t"	// get ram ptr from x1, part 1
		"and x12, x0, #0x20			\n\t"	// SQ# selection, isolate
		"ubfx x0, x0, #5, #20		\n\t"	// get ram offset
		"add x1, x12, x1			\n\t"	// SQ# selection, add to SQ ptr
		"add x11, x11, #512			\n\t"	// get ram ptr from x1, part 2
		"add x11, x11, x0, lsl #5	\n\t"	// ram + offset
		"ldp x9, x10, [x1], #16		\n\t"
		"stp x9, x10, [x11], #16	\n\t"
		"ldp x9, x10, [x1]			\n\t"
		"stp x9, x10, [x11]			\n\t"
		"ret						\n"

		: : : "memory"
	);
}
#endif	// FEAT_SHREC == DYNAREC_JIT