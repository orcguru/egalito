#include <vector>
#include <cassert>
#include "shadowstack.h"
#include "disasm/disassemble.h"
#include "instr/register.h"
#include "instr/concrete.h"
#include "operation/mutator.h"
#include "operation/addinline.h"
#include "operation/find2.h"
#include "pass/switchcontext.h"
#include "types.h"

void ShadowStackPass::visit(Program *program) {
    auto allocateFunc = ChunkFind2(program).findFunction(
        mode == MODE_GS ? "egalito_allocate_shadow_stack_gs"
        : "egalito_allocate_shadow_stack_const");

    if(allocateFunc) {
        SwitchContextPass switchContext;
        allocateFunc->accept(&switchContext);

        // add call to shadow stack allocation function in __libc_start_main
        auto call = new Instruction();
        auto callSem = new ControlFlowInstruction(
            X86_INS_CALL, call, "\xe8", "call", 4);
        callSem->setLink(new NormalLink(allocateFunc, Link::SCOPE_EXTERNAL_JUMP));
        call->setSemantic(callSem);
        
        {
            auto sourceFunc = ChunkFind2(program).findFunction(
                "__libc_start_main");
            assert(sourceFunc);
            auto block1 = sourceFunc->getChildren()->getIterable()->get(0);

            {
                ChunkMutator m(block1, true);
                m.prepend(call);
            }
        }
    }

    recurse(program);
}

void ShadowStackPass::visit(Module *module) {
#ifdef ARCH_X86_64
    auto instr = Disassemble::instruction({0x0f, 0x0b});  // ud2
    auto block = new Block();

    auto symbol = new Symbol(0x0, 0, "egalito_shadowstack_violation",
       Symbol::TYPE_FUNC, Symbol::BIND_GLOBAL, 0, 0);
    auto function = new Function(symbol);
    function->setName(symbol->getName());
    function->setPosition(new AbsolutePosition(0x0));

    module->getFunctionList()->getChildren()->add(function);
    function->setParent(module->getFunctionList());
    ChunkMutator(function).append(block);
    ChunkMutator(block).append(instr);

    this->violationTarget = function;
    recurse(module);
#endif
}

void ShadowStackPass::visit(Function *function) {
    if(function->getName() == "egalito_endbr_violation") return;
    if(function->getName() == "egalito_shadowstack_violation") return;
    if(function->getName() == "egalito_allocate_shadow_stack_gs") return;
    if(function->getName() == "egalito_allocate_shadow_stack_const") return;

    if(function->getName() == "_start") return;
    if(function->getName() == "__libc_start_main") return;
    if(function->getName() == "mmap64") return;
    if(function->getName() == "mmap") return;
    if(function->getName() == "arch_prctl") return;

    pushToShadowStack(function);
    recurse(function);
}


void ShadowStackPass::visit(Instruction *instruction) {
    auto semantic = instruction->getSemantic();
    if(auto v = dynamic_cast<ReturnInstruction *>(semantic)) {
        popFromShadowStack(instruction);
    }
    else if(auto v = dynamic_cast<ControlFlowInstruction *>(semantic)) {
        if(v->getMnemonic() != "callq"
            && v->getLink() && v->getLink()->isExternalJump()) {  // tail recursion

            popFromShadowStack(instruction);
        }
    }
    else if(auto v = dynamic_cast<IndirectJumpInstruction *>(semantic)) {
        if(!v->isForJumpTable()) {  // indirect tail recursion
            popFromShadowStack(instruction);
        }
    }
    /*else if(dynamic_cast<IndirectCallInstruction *>(semantic)) {
        popFromShadowStack(instruction);
    }*/
}

void ShadowStackPass::pushToShadowStack(Function *function) {
	if(mode == MODE_CONST) {
		pushToShadowStackConst(function);
	}
	else {
		pushToShadowStackGS(function);
	}
}

void ShadowStackPass::pushToShadowStackConst(Function *function) {
    ChunkAddInline ai({X86_REG_R11}, [] (bool redzone) {
        // 0:   41 53                   push   %r11
        // 2:   4c 8b 5c 24 08          mov    0x8(%rsp),%r11
        // 7:   4c 89 9c 24 00 00 50    mov    %r11,-0xb00000(%rsp)
        // e:   ff
        // f:   41 5b                   pop    %r11

        //auto pushInstr = Disassemble::instruction({0x41, 0x53});
        Instruction *mov1Instr = nullptr;
        if(redzone) {
            //   5:    4c 8b 9c 24 88 00 00    mov    0x88(%rsp),%r11
            //   c:    00 
            mov1Instr = Disassemble::instruction({0x4c, 0x8b, 0x9c, 0x24, 0x88, 0x00, 0x00, 0x00});
        } else {
            mov1Instr = Disassemble::instruction({0x4c, 0x8b, 0x5c, 0x24, 0x08});
        }
        Instruction *mov2Instr = nullptr;
        {
            unsigned int offset = -0xb00000;
            if(redzone) offset += 0x80;
#define BITS_FROM(x, shift) static_cast<unsigned char>(((x) >> shift) & 0xff)
            mov2Instr = Disassemble::instruction({0x4c, 0x89, 0x9c, 0x24,
                BITS_FROM(offset, 0), BITS_FROM(offset, 8), BITS_FROM(offset, 16), BITS_FROM(offset, 24)});
#undef BITS_FROM
        }
        //auto popInstr = Disassemble::instruction({0x41, 0x5b});

        return std::vector<Instruction *>{ mov1Instr, mov2Instr };
    });
	auto block1 = function->getChildren()->getIterable()->get(0);
	auto instr1 = block1->getChildren()->getIterable()->get(0);
    ai.insertBefore(instr1, false);
}

void ShadowStackPass::pushToShadowStackGS(Function *function) {
    /*  
	   0:   65 4c 8b 1c 25 00 00    mov    %gs:0x0,%r11
	   7:   00 00
	   9:   4d 8d 5b 08             lea    0x8(%r11),%r11
	   d:   4c 8b 14 24             mov    (%rsp),%r10
	  11:   65 4d 89 13             mov    %r10,%gs:(%r11)
	  15:   65 4c 89 1c 25 00 00    mov    %r11,%gs:0x0
	  1c:   00 00
	*/
	auto mov1Instr = Disassemble::instruction({0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x00, 0x00, 0x00, 0x00});
	auto leaInstr = Disassemble::instruction({0x4d, 0x8d, 0x5b, 0x08});
	auto mov2Instr = Disassemble::instruction({0x4c, 0x8b, 0x14, 0x24});
	auto mov3Instr = Disassemble::instruction({0x65, 0x4d, 0x89, 0x13});
	auto mov4Instr = Disassemble::instruction({0x65, 0x4c, 0x89, 0x1c, 0x25, 0x00, 0x00, 0x00, 0x00});

	auto block1 = function->getChildren()->getIterable()->get(0);
	auto instr1 = block1->getChildren()->getIterable()->get(0);
    {
        ChunkMutator m(block1, true);
        m.insertBefore(instr1,
            std::vector<Instruction *>{ mov1Instr, leaInstr, mov2Instr,
		 		mov3Instr, mov4Instr }, false);
    }
}

void ShadowStackPass::popFromShadowStack(Instruction *instruction) {
	if(mode == MODE_CONST) {
		popFromShadowStackConst(instruction);
	}
	else {
		popFromShadowStackGS(instruction);
	}
}

void ShadowStackPass::popFromShadowStackConst(Instruction *instruction) {
    ChunkAddInline ai({X86_REG_EFLAGS, X86_REG_R11}, [this] (bool redzone) {
        /*
                                         pushfd
            0:   41 53                   push   %r11
            2:   4c 8b 5c 24 08          mov    0x8(%rsp),%r11
            7:   4c 39 9c 24 00 00 50    cmp    %r11,-0xb00000(%rsp)
            e:   ff
            f:   0f 85 00 00 00 00       jne    0x15
           15:   41 5b                   pop    %r11
                                         popfd
        */
        //auto pushInstr = Disassemble::instruction({0x41, 0x53});
        Instruction *movInstr = nullptr;
        if(redzone) {
            //   5:    4c 8b 9c 24 88 00 00    mov    0x88(%rsp),%r11
            //   c:    00 
            // (optional 0x80 for redzone), 0x8 for pushfd, 0x8 for push %r11
            movInstr = Disassemble::instruction({0x4c, 0x8b, 0x9c, 0x24, 0x90, 0x00, 0x00, 0x00});
        } else {
            movInstr = Disassemble::instruction({0x4c, 0x8b, 0x5c, 0x24, 0x10});
        }
        Instruction *cmpInstr = nullptr;
        {
            unsigned int offset = -0xb00000;
            offset += 0x8;  // pushfd
            if(redzone) offset += 0x80;
#define BITS_FROM(x, shift) static_cast<unsigned char>(((x) >> shift) & 0xff)
            cmpInstr = Disassemble::instruction({0x4c, 0x39, 0x9c, 0x24,
                BITS_FROM(offset, 0), BITS_FROM(offset, 8), BITS_FROM(offset, 16), BITS_FROM(offset, 24)});
#undef BITS_FROM
        }
        // jne goes here
        //auto popInstr = Disassemble::instruction({0x41, 0x5b});

        auto jne = new Instruction();
        auto jneSem = new ControlFlowInstruction(
            X86_INS_JNE, jne, "\x0f\x85", "jnz", 4);
        jneSem->setLink(new NormalLink(violationTarget, Link::SCOPE_EXTERNAL_JUMP));
        jne->setSemantic(jneSem);
        return std::vector<Instruction *>{ movInstr, cmpInstr, jne };
    });
    ai.insertBefore(instruction, true);
}

void ShadowStackPass::popFromShadowStackGS(Instruction *instruction) {
	/*
	  1f:   65 4c 8b 1c 25 00 00    mov    %gs:0x0,%r11
	  26:   00 00
	  28:   4c 8b 14 24             mov    (%rsp),%r10
	  2c:   65 4d 39 13             cmp    %r10,%gs:(%r11)
	  30:   0f 85 00 00 00 00       jne    0x36
	  36:   4d 8d 5b f8             lea    -0x8(%r11),%r11
	  3a:   65 4c 89 1c 25 00 00    mov    %r11,%gs:0x0
	  41:   00 00
	*/
	auto mov1Instr = Disassemble::instruction({0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x00, 0x00, 0x00, 0x00});
	auto mov2Instr = Disassemble::instruction({0x4c, 0x8b, 0x14, 0x24});
	auto cmpInstr = Disassemble::instruction({0x65, 0x4d, 0x39, 0x13});
	// jmp instr goes here
	auto leaInstr = Disassemble::instruction({0x4d, 0x8d, 0x5b, 0xf8});
	auto mov3Instr = Disassemble::instruction({0x65, 0x4c, 0x89, 0x1c, 0x25, 0x00, 0x00, 0x00, 0x00});

    auto jne = new Instruction();
    auto jneSem = new ControlFlowInstruction(
        X86_INS_JNE, jne, "\x0f\x85", "jnz", 4);
    jneSem->setLink(new NormalLink(violationTarget, Link::SCOPE_EXTERNAL_JUMP));
    jne->setSemantic(jneSem);

    auto block = static_cast<Block *>(instruction->getParent());
    {
        ChunkMutator m(block, true);
        m.insertBefore(instruction,
            std::vector<Instruction *>{ mov1Instr, mov2Instr, cmpInstr, jne, leaInstr, mov3Instr }, true);
    }
    if(0) {
        ChunkMutator m(block, true);
        m.splitBlockBefore(instruction);
    }
}
