#include <algorithm>  // for std::max, std::min
#include <iomanip>
#include <cassert>
#include "mutator.h"
#include "chunk/position.h"
#include "pass/positiondump.h"
#include "instr/instr.h"
#ifdef ARCH_X86_64
    #include "instr/linked-x86_64.h"
#endif
#ifdef ARCH_AARCH64
    #include "instr/linked-aarch64.h"
#endif
#include "log/log.h"

void ChunkMutator::makePositionFor(Chunk *child) {
    PositionFactory *positionFactory = PositionFactory::getInstance();
    Position *pos = nullptr;
    ChunkCursor cursor(chunk, child);
    ChunkCursor prev = cursor;
    if(prev.prev()) {
        pos = positionFactory->makePosition(
            child, prev.get()->getAddress() - chunk->getAddress());
    }
    else {
        pos = positionFactory->makePosition(child, 0);
    }
    child->setPosition(pos);
}

void ChunkMutator::prepend(Chunk *child) {
    if(chunk->getChildren()->genericGetSize() == 0) {
        append(child);
    }
    else {
        insertBefore(chunk->getChildren()->genericGetAt(0), child);
    }
}

void ChunkMutator::append(Chunk *child) {
    // set children and parent pointers
    // parent pointer must be set first for it to return correct address
    // which is needed for spatial map, which is updated in genericAdd()
    child->setParent(chunk);
    firstChildChanged = std::min(firstChildChanged,
        chunk->getChildren()->genericGetSize());
    chunk->getChildren()->genericAdd(child);

    if(!child->getPosition()) makePositionFor(child);
    updateSizesAndAuthorities(child);
}

void ChunkMutator::insertAfter(Chunk *insertPoint, Chunk *newChunk) {
    // set children and parent pointers
    auto list = chunk->getChildren();
    size_t index = (insertPoint ? list->genericIndexOf(insertPoint) + 1 : 0);
    list->genericInsertAt(index, newChunk);
    firstChildChanged = std::min(firstChildChanged, index);
    newChunk->setParent(chunk);

    if(!newChunk->getPosition()) makePositionFor(newChunk);
    updateSizesAndAuthorities(newChunk);
}

void ChunkMutator::insertBefore(Chunk *insertPoint, Chunk *newChunk) {
    if(!insertPoint) {
        append(newChunk);
        return;
    }

    // set children and parent pointers
    auto list = chunk->getChildren();
    size_t index = list->genericIndexOf(insertPoint);
    list->genericInsertAt(index, newChunk);
    firstChildChanged = std::min(firstChildChanged, index);
    newChunk->setParent(chunk);

    // must run before the first-entry update below
    if(!newChunk->getPosition()) makePositionFor(newChunk);

    updateSizesAndAuthorities(newChunk);
}

void ChunkMutator::insertBeforeJumpTo(Instruction *insertPoint, Instruction *newChunk) {
    if(insertPoint == nullptr) {
        insertBefore(nullptr, newChunk);
        return;
    }

    insertAfter(insertPoint, newChunk);

    // swap semantics of these two instructions
    auto sem1 = insertPoint->getSemantic();
    auto sem2 = newChunk->getSemantic();
    insertPoint->setSemantic(sem2);
    newChunk->setSemantic(sem1);

    if(auto linked1 = dynamic_cast<LinkedInstruction *>(sem1)) {
        linked1->setInstruction(newChunk);
    }
    if(auto linked2 = dynamic_cast<LinkedInstruction *>(sem2)) {
        linked2->setInstruction(insertPoint);
    }
}

void ChunkMutator::remove(Chunk *child) {
    firstChildChanged = std::min(firstChildChanged,
        ChunkCursor::getIndex(child));

    // remove from parent
    chunk->getChildren()->genericRemove(child);

    // update sizes of parents and grandparents
    addToSizeRecursively(-child->getSize());

    // update authority pointers in positions
    updateGenerationCounts(chunk);  // ???
}

void ChunkMutator::removeLast(int n) {
    size_t removedSize = 0;
    for(int i = 0; i < n; i ++) {
        Chunk *last = chunk->getChildren()->genericGetLast();
        removedSize += last->getSize();
        chunk->getChildren()->genericRemoveLast();
    }

    // update sizes of parents and grandparents
    addToSizeRecursively(-removedSize);

    // update authority pointers in positions
    updateGenerationCounts(chunk);  // ???
}

void ChunkMutator::splitBlockBefore(Instruction *point) {
#if 0
    auto block = dynamic_cast<Block *>(point->getParent());
    Block *block2 = nullptr;

    PositionFactory *positionFactory = PositionFactory::getInstance();

    std::vector<Instruction *> moveList;
    for(auto child : CIter::children(block)) {
        if(!block2) {
            if(child == point) {
                block2 = new Block();
            }
        }
        if(block2) {
            moveList.push_back(child);
        }
    }

    for(auto child : moveList) {
        ChunkMutator(block).remove(child);
        delete child->getPosition();
    }
    insertAfter(block, block2);
    Chunk *prevChunk = block;
    for(auto child : moveList) {
        child->setPosition(positionFactory->makePosition(
            prevChunk, child, block2->getSize()));

        ChunkMutator(block2).append(child);
        prevChunk = child;
    }
    if(auto block3 = dynamic_cast<Block *>(block2->getNextSibling())) {
        auto instr = block3->getChildren()->getIterable()->get(0);
        delete instr->getPosition();
        instr->setPosition(positionFactory->makePosition(
            block2, instr, block2->getSize()));
    }
#else
    Block *block = dynamic_cast<Block *>(point->getParent());
    size_t totalChildren = block->getChildren()->getIterable()->getCount();
    if(totalChildren == 0) return;

    // Create new block for the new instructions. point will be the first
    // instruction in newBlock.
    Block *newBlock = new Block();
    newBlock->setPosition(PositionFactory::getInstance()->makePosition(
        newBlock, point->getAddress() - chunk->getAddress()));

    // How many instructions to leave in the original block?
    size_t leaveBehindCount = block->getChildren()->getIterable()
        ->indexOf(point);

    // Staging area to temporarily store Instructions being moved from block
    // to newBlock.
    std::vector<Instruction *> moveInstr;
    auto childrenList = block->getChildren()->getIterable();
    for(size_t i = leaveBehindCount; i < totalChildren; i ++) {
        auto last = childrenList->get(i);
        moveInstr.push_back(last);
    }
    ChunkMutator(block, false).removeLast(totalChildren - leaveBehindCount);

    // Clear old pointers and references from instructions.
    for(auto instr : moveInstr) {
        instr->setParent(nullptr);
        delete instr->getPosition();
        instr->setPosition(nullptr);
    }

    // Append instructions from moveInstr to newBlock.
    {
        ChunkMutator newMutator(newBlock, false);
        for(auto instr : moveInstr) {
            newMutator.append(instr);
            newMutator.makePositionFor(instr);
        }
    }

    insertAfter(block, newBlock);
#endif
}

void ChunkMutator::splitFunctionBefore(Block *point) {
    PositionFactory *positionFactory = PositionFactory::getInstance();

    auto function = dynamic_cast<Function *>(point->getParent());
    Function *function2 = nullptr;

    std::vector<Block *> moveList;
    for(auto child : CIter::children(function)) {
        if(!function2) {
            if(child == point) {
                function2 = new Function(point->getAddress());
                function2->setPosition(
                    positionFactory->makeAbsolutePosition(point->getAddress()));
                function2->setParent(function->getParent());
            }
        }
        if(function2) {
            moveList.push_back(child);
        }
    }

    for(auto child : moveList) {
        ChunkMutator(function).remove(child);
        delete child->getPosition();
    }
    auto functionList = dynamic_cast<FunctionList *>(function->getParent());
    functionList->getChildren()->add(function2);

    for(auto child : moveList) {
        child->setPosition(positionFactory->makePosition(
            child, function2->getSize()));

        ChunkMutator(function2).append(child);
    }
}

void ChunkMutator::modifiedChildSize(Chunk *child, int added) {
    firstChildChanged = std::min(firstChildChanged,
        ChunkCursor::getIndex(child));

    // update sizes of parents and grandparents
    addToSizeRecursively(added);

    // update authority pointers in positions
    updateGenerationCounts(child);
}

void ChunkMutator::setPosition(address_t address) {
    //chunk->getPosition()->set(address);
    PositionManager::setAddress(chunk, address);
}

void ChunkMutator::updateSizesAndAuthorities(Chunk *child) {
    // update sizes of parents and grandparents
    addToSizeRecursively(child->getSize());

    // update authority pointers in positions
    updateGenerationCounts(child);
}

void ChunkMutator::updateGenerationCounts(Chunk *child) {
    if(!PositionFactory::getInstance()->needsGenerationTracking()) return;

    // first, find the max of all generations from child on up
    int gen = 0;
    for(Chunk *c = child; c; c = c->getParent()) {
        gen = std::max(gen, c->getPosition()->getGeneration());
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) break;
    }
    gen ++;  // increment generation by one

    // now, set generations of child and up to higher numbers
    for(Chunk *c = child; c; c = c->getParent()) {
        c->getPosition()->setGeneration(gen);
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) break;

        // NOTE: each parent has a higher generation number. This ensures that
        // the authority has a higher number than any of its dependencies,
        // and lookups in any siblings will see this higher number.
        gen ++;
    }

    updateAuthorityHelper(child);
}

void ChunkMutator::updatePositions() {
    if(!allowUpdates) return;
    if(!PositionFactory::getInstance()->needsUpdatePasses()) return;

#if 0  // fully safe, update all children in this function
    updatePositionsFully();
#elif 1  // only update parent siblings if size changed, skip cousins
    for(Chunk *c = chunk; c; c = c->getParent()) {
        if(!c->getPosition()) break;

        Chunk *prev = nullptr;
        for(auto cursor = ChunkCursor::makeBegin(c); !cursor.isEnd();
            cursor.next()) {

            auto child = cursor.get();
            child->getPosition()->recalculate(prev);
            prev = child;
        }

        // If the size of this Chunk hasn't changed, its siblings higher up
        // in the tree have the same offsets as before.
        if(!sizeChanged) break;
    }
#elif 0  // when updating lists, begin at the modified index
    size_t changeStart = firstChildChanged;
    for(Chunk *c = chunk; c; c = c->getParent()) {
        if(!c->getPosition()) break;

        size_t genericSize = c->getChildren()->genericGetSize();
        Chunk *prev = nullptr;
        for(size_t i = changeStart; i < genericSize; i ++) {
            auto child = c->getChildren()->genericGetAt(i);
            child->getPosition()->recalculate(prev);
            prev = child;
        }

        // If the size of this Chunk hasn't changed, its siblings higher up
        // in the tree have the same offsets as before.
        if(!sizeChanged) break;

        // We're going up the tree, get the index of this Chunk, since that's
        // where changes have happened.
        if(c->getParent()) {
            changeStart = ChunkCursor::getIndex(c);
        }
    }
#endif
}

void ChunkMutator::updatePositionsFully() {
    // Find the Function that contains this chunk, and recalculate the offset
    // in every Position within the Function.
    for(Chunk *c = chunk; c; c = c->getParent()) {
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) {
            updatePositionHelper(c);
            break;
        }
    }
}

void ChunkMutator::updateAuthorityHelper(Chunk *root) {
    root->getPosition()->updateAuthority();

    if(root->getChildren()) {
        for(auto child : root->getChildren()->genericIterable()) {
            updateAuthorityHelper(child);
        }
    }
}

void ChunkMutator::updatePositionHelper(Chunk *root) {
    if(!root->getChildren()) return;

    Chunk *previous = nullptr;
    for(auto child : root->getChildren()->genericIterable()) {
        // Since the root has a position, we assume that the child does
        // too.
        //assert(child->getPosition());

        child->getPosition()->recalculate(previous);
        updatePositionHelper(child);

        previous = child;
    }
}

void ChunkMutator::addToSizeRecursively(int added) {
    // update sizes of parents and grandparents
    for(Chunk *c = chunk; c && !dynamic_cast<Module *>(c); c = c->getParent()) {
        c->addToSize(added);
    }

    sizeChanged = true;
}
