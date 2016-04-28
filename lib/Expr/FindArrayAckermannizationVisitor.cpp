//===-- FindArrayAckermannizationVisitor.cpp --------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "FindArrayAckermannizationVisitor.h"

namespace klee {

ArrayAckermannizationInfo::ArrayAckermannizationInfo()
    : contiguousMSBitIndex(0), contiguousLSBitIndex(0) {}

bool ArrayAckermannizationInfo::isContiguousArrayRead() {
  if (isa<ConcatExpr>(toReplace)) {
    return true;
  }
  return false;
}

const Array *ArrayAckermannizationInfo::getArray() {
  if (ReadExpr *re = dyn_cast<ReadExpr>(toReplace)) {
    return re->updates.root;
  } else if (ConcatExpr *ce = dyn_cast<ConcatExpr>(toReplace)) {
    assert(ce->getKid(0)->getKind() == Expr::Read &&
           "left child must be a ReadExpr");
    ReadExpr *re = dyn_cast<ReadExpr>(ce->getKid(0));
    return re->updates.root;
  }
  llvm_unreachable("Unhandled expr type");
  return NULL;
}

bool ArrayAckermannizationInfo::isWholeArray() {
  const Array *theArray = getArray();
  if (isContiguousArrayRead()) {
    unsigned bitWidthOfArray = theArray->size * theArray->range;
    assert(contiguousMSBitIndex > contiguousLSBitIndex);
    unsigned bitWidthOfRegion = contiguousMSBitIndex - contiguousLSBitIndex +1;
    if (bitWidthOfArray == bitWidthOfRegion) {
      return true;
    }
  }
  return false;
}

FindArrayAckermannizationVisitor::FindArrayAckermannizationVisitor(
    bool recursive, unsigned maxArrayWidth)
    : ExprVisitor(recursive), maxArrayWidth(maxArrayWidth) {}

std::vector<ArrayAckermannizationInfo> *
FindArrayAckermannizationVisitor::getOrInsertAckermannizationInfo(
    const Array *arr, bool *wasInsert) {
  // Try to insert empty info. This will fail if we already have info on this
  // Array and we'll get the existing ArrayAckermannizationInfo instead.
  std::pair<ArrayToAckermannizationInfoMapTy::iterator, bool> pair =
      ackermannizationInfo.insert(
          std::make_pair(arr, std::vector<ArrayAckermannizationInfo>(0)));
  if (wasInsert)
    *wasInsert = pair.second;

  return &((pair.first)->second);
}

/* This method looks for nested concatenated ReadExpr that don't involve
 * updates or constant arrays that are contigous. We look for ConcatExpr
 * unbalanced to the right that operate on the same array. E.g.
 *
 *                  ConcatExpr
 *                 /       \
 *    ReadExpr 3bv32 Ar     \
 *                        ConcatExpr
 *                         /    \
 *           ReadExpr 2bv32 Ar   \
 *                              ConcatExpr
 *                              /        \
 *                             /          \
 *                ReadExpr 1bv32 Ar       ReadExpr 0bv32 Ar
 */
ExprVisitor::Action
FindArrayAckermannizationVisitor::visitConcat(const ConcatExpr &ce) {
  const Array *theArray = NULL;
  std::vector<ArrayAckermannizationInfo> *ackInfos = NULL;
  ArrayAckermannizationInfo ackInfo;
  const ConcatExpr *currentConcat = &ce;
  std::vector<ref<ReadExpr> > reads;
  bool isFirst = true;
  unsigned MSBitIndex = 0;
  unsigned LSBitIndex = 0;
  unsigned widthReadSoFar = 0; // In bits

  // I'm sorry about the use of goto here but without having lambdas (we're
  // using C++03) it's kind of hard to have readable and efficient code that
  // handles the case when we fail to match without using gotos.

  // Try to find the array
  if (ReadExpr *lhsRead = dyn_cast<ReadExpr>(ce.getKid(0))) {
    theArray = lhsRead->updates.root;
    assert(theArray && "theArray cannot be NULL");

    // Try getting existing ArrayAckermannizationInfos
    bool wasInsert = true;
    ackInfos = getOrInsertAckermannizationInfo(theArray, &wasInsert);
    if (!wasInsert && ackInfos->size() == 0) {
      // We've seen this array before and it can't be ackermannized
      goto failedMatch;
    }

    // FIXME: We should be able to handle no-overlapping contiguous
    // reads from an array
    if ((theArray->size * theArray->range) >= this->maxArrayWidth) {
      goto failedMatch;
    }

    // FIXME: We can probably handle constant arrays using bitwise masking,
    // or-ing and shifting. For now say we can't ackermannize this array
    if (theArray->isConstantArray()) {
      goto failedMatch;
    }
  } else {
    goto failedMatch;
  }

  // Collect the ordered ReadExprs
  while (true) {
    ref<Expr> lhs = currentConcat->getKid(0);
    ref<Expr> rhs = currentConcat->getKid(1);

    // Lhs must be a ReadExpr
    if (ReadExpr *lhsre = dyn_cast<ReadExpr>(lhs)) {
      reads.push_back(lhsre);
    } else {
      goto failedMatch;
    }

    // Rhs must be a ConcatExpr or ReadExpr
    if (ReadExpr *rhsre = dyn_cast<ReadExpr>(rhs)) {
      reads.push_back(rhsre);
      break; // Hit the right most leaf node.
    } else if (ConcatExpr *rhsconcat = dyn_cast<ConcatExpr>(rhs)) {
      currentConcat = rhsconcat;
      continue;
    }
    goto failedMatch;
  }

  // Go through the ordered reads checking they match the expected pattern
  assert(isFirst);
  for (std::vector<ref<ReadExpr> >::const_iterator bi = reads.begin(),
                                                   be = reads.end();
       bi != be; ++bi) {

    ref<ReadExpr> read = *bi;
    // Check we are looking at the same array that we found earlier
    if (theArray != read->updates.root) {
      goto failedMatch;
    }

    // FIXME: Figure out how to handle updates. For now pretend we can't
    // ackermannize this array
    if (read->updates.head != NULL) {
      goto failedMatch;
    }

    widthReadSoFar += read->getWidth();

    // Check index is constant and that we are doing contiguous reads
    if (ConstantExpr *index = dyn_cast<ConstantExpr>(read->index)) {
      if (!isFirst) {
        // Check we are reading the next region along in the array. This
        // implementation supports ReadExpr of different sizes although
        // currently in KLEE they are always 8-bits (1 byte).
        unsigned difference =
            LSBitIndex - (index->getZExtValue() * read->getWidth());
        if (difference != read->getWidth()) {
          goto failedMatch;
        }
      } else {
        // Compute most significant bit
        // E.g. if index was 2 and width is 8 then this is a byte read
        // but the most significant bit read is not 16, it is 23.
        MSBitIndex = (index->getZExtValue() * read->getWidth()) + (read->getWidth() -1);
      }
      // Record the least significant bit read
      LSBitIndex = index->getZExtValue() * read->getWidth();
    } else {
      goto failedMatch;
    }

    isFirst = false;
  }
  // FIXME: We can probably support partially contiguous regions as different variables.
  // Check that the width we are reading is the whole array
  if (widthReadSoFar != (theArray->size * theArray->range)) {
    goto failedMatch;
  }

  // We found a match
  ackInfo.toReplace = ref<Expr>(const_cast<ConcatExpr *>(&ce));
  ackInfo.contiguousMSBitIndex = MSBitIndex;
  ackInfo.contiguousLSBitIndex = LSBitIndex;
  assert(ackInfo.contiguousMSBitIndex > ackInfo.contiguousLSBitIndex && "bit indicies incorrectly ordered");
  ackInfos->push_back(ackInfo);
  // We know the indices are simple constants so need to traverse children
  return Action::skipChildren();

failedMatch :
  // Any empty list of ArrayAckermannizationInfo indicates that the array cannot
  // be ackermannized.
  if (ackInfos)
    ackInfos->clear();
  return Action::doChildren();
}

ExprVisitor::Action
FindArrayAckermannizationVisitor::visitRead(const ReadExpr &re) {
  const Array *theArray = re.updates.root;
  bool wasInsert = true;
  ArrayAckermannizationInfo ackInfo;
  std::vector<ArrayAckermannizationInfo> *ackInfos =
      getOrInsertAckermannizationInfo(theArray, &wasInsert);

  // I'm sorry about the use of goto here but without having lambdas (we're
  // using C++03) it's kind of hard to have readable and efficient code that
  // handles the case when we fail to match without using gotos.

  if ((theArray->size * theArray->range) > this->maxArrayWidth) {
    // Array is too large for allowed ackermannization
    goto failedMatch;
  }

  if (!wasInsert && ackInfos->size() == 0) {
    // We've seen this array before and it can't be ackermannized.
    goto failedMatch;
  }

  // FIXME: Figure out how to handle constant arrays. We can probably generate
  // large constants but we can't return immediatly as there may be updates to
  // handle.  For now say that they can't be ackermannized
  if (theArray->isConstantArray()) {
    goto failedMatch;
  }

  // FIXME: Figure out how to handle updates to the array. I think we can
  // handle this using bitwise masking and or'ing and shifting. For now pretend
  // that they can't be ackermannized.
  if (re.updates.head != NULL) {
    goto failedMatch;
  }

  // This is an array read without constants values or updates so we
  // can definitely ackermannize this based on what we've seen so far.
  ackInfo.toReplace = ref<Expr>(const_cast<ReadExpr *>(&re));
  ackInfos->push_back(ackInfo);
  return Action::doChildren(); // Traverse index expression

failedMatch :
  // Clear any existing ArrayAckermannizationInfos to indicate that the Array
  // cannot be ackermannized.
  ackInfos->clear();
  return Action::doChildren(); // Traverse index expression
}

void FindArrayAckermannizationVisitor::clear() { ackermannizationInfo.clear(); }
}
