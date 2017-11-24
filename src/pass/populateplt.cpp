#include <cassert>
#include "populateplt.h"
#include "conductor/conductor.h"
#include "disasm/disassemble.h"
#include "instr/concrete.h"
#include "instr/linked-x86_64.h"
#include "operation/find2.h"
#include "operation/mutator.h"
#include "log/log.h"

void PopulatePLTPass::visit(Module *module) {
    this->module = module;
    recurse(module->getPLTList());
}

void PopulatePLTPass::visit(PLTTrampoline *trampoline) {
    if(trampoline->isIFunc()) {
        populateLazyTrampoline(trampoline);
    }
    else {
        populateTrampoline(trampoline);
    }
}

void PopulatePLTPass::populateLazyTrampoline(PLTTrampoline *trampoline) {
#if 0
    // lea 0xNNNNNNNN(%rip), %r11
    ADD_BYTES("\x4c\x8d\x1d", 3);
    int disp = gotPLT - getAddress() - 7;
    ADD_BYTES(&disp, 4);

    // jmpq *(%r11)
    ADD_BYTES("\x41\xff\x23", 3);

    // pushq %r11
    ADD_BYTES("\x41\x53", 2);

    // jmpq ifunc_resolver
    ADD_BYTES("\xe9", 1);
    disp = reinterpret_cast<address_t>(ifunc_resolver) - getAddress() - 17;
    ADD_BYTES(&disp, 4);
#endif
    DisasmHandle handle(true);
    auto lea = new Instruction();
    auto lea_asm = DisassembleInstruction(handle).makeAssembly(
        std::vector<unsigned char>({0x4c, 0x8d, 0x1d, 0, 0, 0, 0}));
    auto lea_linked = new LinkedInstruction(lea, lea_asm);
    lea_linked->setIndex(0);
    lea->setSemantic(lea_linked);
    auto link = LinkFactory::makeDataLink(module, trampoline->getGotPLTEntry());
    assert(link);
    lea_linked->setLink(link);

    auto jmpq = DisassembleInstruction(handle).instruction(
        std::vector<unsigned char>({0x41, 0xff, 0x23}));

    auto pushq = DisassembleInstruction(handle).instruction(
        std::vector<unsigned char>({0x41, 0x53}));

    auto jmpq2 = DisassembleInstruction(handle).instruction(
        std::vector<unsigned char>({0xe9, 0, 0, 0, 0}));
    auto lib = conductor->getLibraryList()->get("(egalito)");
    if(!lib) {
        LOG(1, "PopulatePLTPass requires libegalito.so");
        throw "PopulatePLTPass error";
    }
    auto resolver = ChunkFind2(conductor).findFunctionInModule(
        "ifunc_resolver", lib->getElfSpace()->getModule());
    auto jmpq2_cfi = dynamic_cast<ControlFlowInstruction *>(
        jmpq2->getSemantic());
    auto link2 = LinkFactory::makeNormalLink(resolver, true, true);
    assert(link2);
    delete jmpq2_cfi->getLink();
    jmpq2_cfi->setLink(link2);

    auto block1 = new Block();
    auto block2 = new Block();

    ChunkMutator m(trampoline);
    m.append(block1);
    m.append(block2);

    ChunkMutator m1(block1);
    m1.append(lea);
    m1.append(jmpq);

    ChunkMutator m2(block2);
    m2.append(pushq);
    m2.append(jmpq2);
}

void PopulatePLTPass::populateTrampoline(PLTTrampoline *trampoline) {
#if 0
    // ff 25 NN NN NN NN    jmpq *0xNNNNNNNN(%rip)
    ADD_BYTES("\xff\x25", 2);
    address_t disp = gotPLT - (getAddress() + 2+4);
    ADD_BYTES(&disp, 4);
#endif
    DisasmHandle handle(true);
    auto jmpq = DisassembleInstruction(handle).instruction(
        std::vector<unsigned char>({0xff, 0x25, 0, 0, 0, 0}));
    auto cfi = dynamic_cast<ControlFlowInstruction *>(jmpq->getSemantic());
    auto link = LinkFactory::makeDataLink(module, trampoline->getGotPLTEntry());
    assert(link);
    delete cfi->getLink();
    cfi->setLink(link);

    auto block1 = new Block();

    ChunkMutator m(trampoline);
    m.append(block1);

    ChunkMutator m1(block1);
    m1.append(jmpq);
}

