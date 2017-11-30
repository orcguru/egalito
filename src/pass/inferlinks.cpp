#include "inferlinks.h"
#include "chunk/dump.h"
#include "disasm/makesemantic.h"
#include "log/log.h"
#include "log/temp.h"

void InferLinksPass::visit(Module *module) {
    this->module = module;
#ifdef ARCH_AARCH64
    LinkedInstruction::makeAllLinked(module);
#else
    recurse(module);
#endif
}

void InferLinksPass::visit(Function *function) {
    recurse(function);
}

void InferLinksPass::visit(Instruction *instruction) {
    auto semantic = instruction->getSemantic();
    if(dynamic_cast<IndirectCallInstruction *>(semantic)) {
        // if this is RIP-relative, we should try to convert this to
        // ControlFlowInstruction
        return;
    }
    if(dynamic_cast<IndirectJumpInstruction *>(semantic)) {
        return;
    }
    if(auto v = dynamic_cast<DisassembledInstruction *>(semantic)) {
        if(v->getLink()) return;
        auto assembly = v->getAssembly();
        if(!assembly) return;

#ifdef ARCH_X86_64
        // see if this instruction has any operands that need links
        // (can return NULL if not)
        auto linked = LinkedInstruction::makeLinked(module, instruction, assembly);
        if(linked) {
            instruction->setSemantic(linked);
            ChunkDumper d;
            IF_LOG(10) instruction->accept(&d);
            delete v;
        }
#elif defined(ARCH_ARM)
        auto linked = LinkedInstruction::makeLinked(module, instruction, assembly);
        if(linked) {
            instruction->setSemantic(linked);
            delete v;
        }
#endif
    }
}
