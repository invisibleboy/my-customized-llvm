//===------- LegalizeVectorTypes.cpp - Legalization of vector types -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file performs vector type splitting and scalarization for LegalizeTypes.
// Scalarization is the act of changing a computation in an illegal one-element
// vector type to be a computation in its scalar element type.  For example,
// implementing <1 x f32> arithmetic in a scalar f32 register.  This is needed
// as a base case when scalarizing vector arithmetic like <4 x f32>, which
// eventually decomposes to scalars if the target doesn't support v4f32 or v2f32
// types.
// Splitting is the act of changing a computation in an invalid vector type to
// be a computation in two vectors of half the size.  For example, implementing
// <128 x f32> operations in terms of two <64 x f32> operations.
//
//===----------------------------------------------------------------------===//

#include "LegalizeTypes.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
//  Result Vector Scalarization: <1 x ty> -> ty.
//===----------------------------------------------------------------------===//

void DAGTypeLegalizer::ScalarizeVectorResult(SDNode *N, unsigned ResNo) {
  DEBUG(dbgs() << "Scalarize node result " << ResNo << ": ";
        N->dump(&DAG);
        dbgs() << "\n");
  SDValue R = SDValue();

  switch (N->getOpcode()) {
  default:
#ifndef NDEBUG
    dbgs() << "ScalarizeVectorResult #" << ResNo << ": ";
    N->dump(&DAG);
    dbgs() << "\n";
#endif
    report_fatal_error("Do not know how to scalarize the result of this "
                       "operator!\n");

  case ISD::MERGE_VALUES:      R = ScalarizeVecRes_MERGE_VALUES(N, ResNo);break;
  case ISD::BITCAST:           R = ScalarizeVecRes_BITCAST(N); break;
  case ISD::BUILD_VECTOR:      R = N->getOperand(0); break;
  case ISD::CONVERT_RNDSAT:    R = ScalarizeVecRes_CONVERT_RNDSAT(N); break;
  case ISD::EXTRACT_SUBVECTOR: R = ScalarizeVecRes_EXTRACT_SUBVECTOR(N); break;
  case ISD::FP_ROUND:          R = ScalarizeVecRes_FP_ROUND(N); break;
  case ISD::FP_ROUND_INREG:    R = ScalarizeVecRes_InregOp(N); break;
  case ISD::FPOWI:             R = ScalarizeVecRes_FPOWI(N); break;
  case ISD::INSERT_VECTOR_ELT: R = ScalarizeVecRes_INSERT_VECTOR_ELT(N); break;
  case ISD::LOAD:           R = ScalarizeVecRes_LOAD(cast<LoadSDNode>(N));break;
  case ISD::SCALAR_TO_VECTOR:  R = ScalarizeVecRes_SCALAR_TO_VECTOR(N); break;
  case ISD::SIGN_EXTEND_INREG: R = ScalarizeVecRes_InregOp(N); break;
  case ISD::SELECT:            R = ScalarizeVecRes_SELECT(N); break;
  case ISD::SELECT_CC:         R = ScalarizeVecRes_SELECT_CC(N); break;
  case ISD::SETCC:             R = ScalarizeVecRes_SETCC(N); break;
  case ISD::UNDEF:             R = ScalarizeVecRes_UNDEF(N); break;
  case ISD::VECTOR_SHUFFLE:    R = ScalarizeVecRes_VECTOR_SHUFFLE(N); break;
  case ISD::ANY_EXTEND:
  case ISD::CTLZ:
  case ISD::CTPOP:
  case ISD::CTTZ:
  case ISD::FABS:
  case ISD::FCEIL:
  case ISD::FCOS:
  case ISD::FEXP:
  case ISD::FEXP2:
  case ISD::FFLOOR:
  case ISD::FLOG:
  case ISD::FLOG10:
  case ISD::FLOG2:
  case ISD::FNEARBYINT:
  case ISD::FNEG:
  case ISD::FP_EXTEND:
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
  case ISD::FRINT:
  case ISD::FSIN:
  case ISD::FSQRT:
  case ISD::FTRUNC:
  case ISD::SIGN_EXTEND:
  case ISD::SINT_TO_FP:
  case ISD::TRUNCATE:
  case ISD::UINT_TO_FP:
  case ISD::ZERO_EXTEND:
    R = ScalarizeVecRes_UnaryOp(N);
    break;

  case ISD::ADD:
  case ISD::AND:
  case ISD::FADD:
  case ISD::FDIV:
  case ISD::FMUL:
  case ISD::FPOW:
  case ISD::FREM:
  case ISD::FSUB:
  case ISD::MUL:
  case ISD::OR:
  case ISD::SDIV:
  case ISD::SREM:
  case ISD::SUB:
  case ISD::UDIV:
  case ISD::UREM:
  case ISD::XOR:
  case ISD::SHL:
  case ISD::SRA:
  case ISD::SRL:
    R = ScalarizeVecRes_BinOp(N);
    break;
  }

  // If R is null, the sub-method took care of registering the result.
  if (R.getNode())
    SetScalarizedVector(SDValue(N, ResNo), R);
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_BinOp(SDNode *N) {
  SDValue LHS = GetScalarizedVector(N->getOperand(0));
  SDValue RHS = GetScalarizedVector(N->getOperand(1));
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(),
                     LHS.getValueType(), LHS, RHS);
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_MERGE_VALUES(SDNode *N,
                                                       unsigned ResNo) {
  SDValue Op = DisintegrateMERGE_VALUES(N, ResNo);
  return GetScalarizedVector(Op);
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_BITCAST(SDNode *N) {
  EVT NewVT = N->getValueType(0).getVectorElementType();
  return DAG.getNode(ISD::BITCAST, N->getDebugLoc(),
                     NewVT, N->getOperand(0));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_CONVERT_RNDSAT(SDNode *N) {
  EVT NewVT = N->getValueType(0).getVectorElementType();
  SDValue Op0 = GetScalarizedVector(N->getOperand(0));
  return DAG.getConvertRndSat(NewVT, N->getDebugLoc(),
                              Op0, DAG.getValueType(NewVT),
                              DAG.getValueType(Op0.getValueType()),
                              N->getOperand(3),
                              N->getOperand(4),
                              cast<CvtRndSatSDNode>(N)->getCvtCode());
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_EXTRACT_SUBVECTOR(SDNode *N) {
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, N->getDebugLoc(),
                     N->getValueType(0).getVectorElementType(),
                     N->getOperand(0), N->getOperand(1));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_FP_ROUND(SDNode *N) {
  EVT NewVT = N->getValueType(0).getVectorElementType();
  SDValue Op = GetScalarizedVector(N->getOperand(0));
  return DAG.getNode(ISD::FP_ROUND, N->getDebugLoc(),
                     NewVT, Op, N->getOperand(1));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_FPOWI(SDNode *N) {
  SDValue Op = GetScalarizedVector(N->getOperand(0));
  return DAG.getNode(ISD::FPOWI, N->getDebugLoc(),
                     Op.getValueType(), Op, N->getOperand(1));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_INSERT_VECTOR_ELT(SDNode *N) {
  // The value to insert may have a wider type than the vector element type,
  // so be sure to truncate it to the element type if necessary.
  SDValue Op = N->getOperand(1);
  EVT EltVT = N->getValueType(0).getVectorElementType();
  if (Op.getValueType() != EltVT)
    // FIXME: Can this happen for floating point types?
    Op = DAG.getNode(ISD::TRUNCATE, N->getDebugLoc(), EltVT, Op);
  return Op;
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_LOAD(LoadSDNode *N) {
  assert(N->isUnindexed() && "Indexed vector load?");

  SDValue Result = DAG.getLoad(ISD::UNINDEXED,
                               N->getExtensionType(),
                               N->getValueType(0).getVectorElementType(),
                               N->getDebugLoc(),
                               N->getChain(), N->getBasePtr(),
                               DAG.getUNDEF(N->getBasePtr().getValueType()),
                               N->getPointerInfo(),
                               N->getMemoryVT().getVectorElementType(),
                               N->isVolatile(), N->isNonTemporal(),
                               N->isInvariant(), N->getOriginalAlignment());

  // Legalized the chain result - switch anything that used the old chain to
  // use the new one.
  ReplaceValueWith(SDValue(N, 1), Result.getValue(1));
  return Result;
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_UnaryOp(SDNode *N) {
  // Get the dest type - it doesn't always match the input type, e.g. int_to_fp.
  EVT DestVT = N->getValueType(0).getVectorElementType();
  SDValue Op = GetScalarizedVector(N->getOperand(0));
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(), DestVT, Op);
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_InregOp(SDNode *N) {
  EVT EltVT = N->getValueType(0).getVectorElementType();
  EVT ExtVT = cast<VTSDNode>(N->getOperand(1))->getVT().getVectorElementType();
  SDValue LHS = GetScalarizedVector(N->getOperand(0));
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(), EltVT,
                     LHS, DAG.getValueType(ExtVT));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_SCALAR_TO_VECTOR(SDNode *N) {
  // If the operand is wider than the vector element type then it is implicitly
  // truncated.  Make that explicit here.
  EVT EltVT = N->getValueType(0).getVectorElementType();
  SDValue InOp = N->getOperand(0);
  if (InOp.getValueType() != EltVT)
    return DAG.getNode(ISD::TRUNCATE, N->getDebugLoc(), EltVT, InOp);
  return InOp;
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_SELECT(SDNode *N) {
  SDValue LHS = GetScalarizedVector(N->getOperand(1));
  return DAG.getNode(ISD::SELECT, N->getDebugLoc(),
                     LHS.getValueType(), N->getOperand(0), LHS,
                     GetScalarizedVector(N->getOperand(2)));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_SELECT_CC(SDNode *N) {
  SDValue LHS = GetScalarizedVector(N->getOperand(2));
  return DAG.getNode(ISD::SELECT_CC, N->getDebugLoc(), LHS.getValueType(),
                     N->getOperand(0), N->getOperand(1),
                     LHS, GetScalarizedVector(N->getOperand(3)),
                     N->getOperand(4));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_SETCC(SDNode *N) {
  assert(N->getValueType(0).isVector() ==
         N->getOperand(0).getValueType().isVector() &&
         "Scalar/Vector type mismatch");

  if (N->getValueType(0).isVector()) return ScalarizeVecRes_VSETCC(N);

  SDValue LHS = GetScalarizedVector(N->getOperand(0));
  SDValue RHS = GetScalarizedVector(N->getOperand(1));
  DebugLoc DL = N->getDebugLoc();

  // Turn it into a scalar SETCC.
  return DAG.getNode(ISD::SETCC, DL, MVT::i1, LHS, RHS, N->getOperand(2));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_UNDEF(SDNode *N) {
  return DAG.getUNDEF(N->getValueType(0).getVectorElementType());
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_VECTOR_SHUFFLE(SDNode *N) {
  // Figure out if the scalar is the LHS or RHS and return it.
  SDValue Arg = N->getOperand(2).getOperand(0);
  if (Arg.getOpcode() == ISD::UNDEF)
    return DAG.getUNDEF(N->getValueType(0).getVectorElementType());
  unsigned Op = !cast<ConstantSDNode>(Arg)->isNullValue();
  return GetScalarizedVector(N->getOperand(Op));
}

SDValue DAGTypeLegalizer::ScalarizeVecRes_VSETCC(SDNode *N) {
  assert(N->getValueType(0).isVector() &&
         N->getOperand(0).getValueType().isVector() &&
         "Operand types must be vectors");

  SDValue LHS = GetScalarizedVector(N->getOperand(0));
  SDValue RHS = GetScalarizedVector(N->getOperand(1));
  EVT NVT = N->getValueType(0).getVectorElementType();
  DebugLoc DL = N->getDebugLoc();

  // Turn it into a scalar SETCC.
  SDValue Res = DAG.getNode(ISD::SETCC, DL, MVT::i1, LHS, RHS,
                            N->getOperand(2));
  // Vectors may have a different boolean contents to scalars.  Promote the
  // value appropriately.
  ISD::NodeType ExtendCode =
    TargetLowering::getExtendForContent(TLI.getBooleanContents(true));
  return DAG.getNode(ExtendCode, DL, NVT, Res);
}


//===----------------------------------------------------------------------===//
//  Operand Vector Scalarization <1 x ty> -> ty.
//===----------------------------------------------------------------------===//

bool DAGTypeLegalizer::ScalarizeVectorOperand(SDNode *N, unsigned OpNo) {
  DEBUG(dbgs() << "Scalarize node operand " << OpNo << ": ";
        N->dump(&DAG);
        dbgs() << "\n");
  SDValue Res = SDValue();

  if (Res.getNode() == 0) {
    switch (N->getOpcode()) {
    default:
#ifndef NDEBUG
      dbgs() << "ScalarizeVectorOperand Op #" << OpNo << ": ";
      N->dump(&DAG);
      dbgs() << "\n";
#endif
      llvm_unreachable("Do not know how to scalarize this operator's operand!");
    case ISD::BITCAST:
      Res = ScalarizeVecOp_BITCAST(N);
      break;
    case ISD::CONCAT_VECTORS:
      Res = ScalarizeVecOp_CONCAT_VECTORS(N);
      break;
    case ISD::EXTRACT_VECTOR_ELT:
      Res = ScalarizeVecOp_EXTRACT_VECTOR_ELT(N);
      break;
    case ISD::STORE:
      Res = ScalarizeVecOp_STORE(cast<StoreSDNode>(N), OpNo);
      break;
    }
  }

  // If the result is null, the sub-method took care of registering results etc.
  if (!Res.getNode()) return false;

  // If the result is N, the sub-method updated N in place.  Tell the legalizer
  // core about this.
  if (Res.getNode() == N)
    return true;

  assert(Res.getValueType() == N->getValueType(0) && N->getNumValues() == 1 &&
         "Invalid operand expansion");

  ReplaceValueWith(SDValue(N, 0), Res);
  return false;
}

/// ScalarizeVecOp_BITCAST - If the value to convert is a vector that needs
/// to be scalarized, it must be <1 x ty>.  Convert the element instead.
SDValue DAGTypeLegalizer::ScalarizeVecOp_BITCAST(SDNode *N) {
  SDValue Elt = GetScalarizedVector(N->getOperand(0));
  return DAG.getNode(ISD::BITCAST, N->getDebugLoc(),
                     N->getValueType(0), Elt);
}

/// ScalarizeVecOp_CONCAT_VECTORS - The vectors to concatenate have length one -
/// use a BUILD_VECTOR instead.
SDValue DAGTypeLegalizer::ScalarizeVecOp_CONCAT_VECTORS(SDNode *N) {
  SmallVector<SDValue, 8> Ops(N->getNumOperands());
  for (unsigned i = 0, e = N->getNumOperands(); i < e; ++i)
    Ops[i] = GetScalarizedVector(N->getOperand(i));
  return DAG.getNode(ISD::BUILD_VECTOR, N->getDebugLoc(), N->getValueType(0),
                     &Ops[0], Ops.size());
}

/// ScalarizeVecOp_EXTRACT_VECTOR_ELT - If the input is a vector that needs to
/// be scalarized, it must be <1 x ty>, so just return the element, ignoring the
/// index.
SDValue DAGTypeLegalizer::ScalarizeVecOp_EXTRACT_VECTOR_ELT(SDNode *N) {
  SDValue Res = GetScalarizedVector(N->getOperand(0));
  if (Res.getValueType() != N->getValueType(0))
    Res = DAG.getNode(ISD::ANY_EXTEND, N->getDebugLoc(), N->getValueType(0),
                      Res);
  return Res;
}

/// ScalarizeVecOp_STORE - If the value to store is a vector that needs to be
/// scalarized, it must be <1 x ty>.  Just store the element.
SDValue DAGTypeLegalizer::ScalarizeVecOp_STORE(StoreSDNode *N, unsigned OpNo){
  assert(N->isUnindexed() && "Indexed store of one-element vector?");
  assert(OpNo == 1 && "Do not know how to scalarize this operand!");
  DebugLoc dl = N->getDebugLoc();

  if (N->isTruncatingStore())
    return DAG.getTruncStore(N->getChain(), dl,
                             GetScalarizedVector(N->getOperand(1)),
                             N->getBasePtr(), N->getPointerInfo(),
                             N->getMemoryVT().getVectorElementType(),
                             N->isVolatile(), N->isNonTemporal(),
                             N->getAlignment());

  return DAG.getStore(N->getChain(), dl, GetScalarizedVector(N->getOperand(1)),
                      N->getBasePtr(), N->getPointerInfo(),
                      N->isVolatile(), N->isNonTemporal(),
                      N->getOriginalAlignment());
}


//===----------------------------------------------------------------------===//
//  Result Vector Splitting
//===----------------------------------------------------------------------===//

/// SplitVectorResult - This method is called when the specified result of the
/// specified node is found to need vector splitting.  At this point, the node
/// may also have invalid operands or may have other results that need
/// legalization, we just know that (at least) one result needs vector
/// splitting.
void DAGTypeLegalizer::SplitVectorResult(SDNode *N, unsigned ResNo) {
  DEBUG(dbgs() << "Split node result: ";
        N->dump(&DAG);
        dbgs() << "\n");
  SDValue Lo, Hi;

  switch (N->getOpcode()) {
  default:
#ifndef NDEBUG
    dbgs() << "SplitVectorResult #" << ResNo << ": ";
    N->dump(&DAG);
    dbgs() << "\n";
#endif
    llvm_unreachable("Do not know how to split the result of this operator!");

  case ISD::MERGE_VALUES: SplitRes_MERGE_VALUES(N, ResNo, Lo, Hi); break;
  case ISD::VSELECT:
  case ISD::SELECT:       SplitRes_SELECT(N, Lo, Hi); break;
  case ISD::SELECT_CC:    SplitRes_SELECT_CC(N, Lo, Hi); break;
  case ISD::UNDEF:        SplitRes_UNDEF(N, Lo, Hi); break;
  case ISD::BITCAST:           SplitVecRes_BITCAST(N, Lo, Hi); break;
  case ISD::BUILD_VECTOR:      SplitVecRes_BUILD_VECTOR(N, Lo, Hi); break;
  case ISD::CONCAT_VECTORS:    SplitVecRes_CONCAT_VECTORS(N, Lo, Hi); break;
  case ISD::EXTRACT_SUBVECTOR: SplitVecRes_EXTRACT_SUBVECTOR(N, Lo, Hi); break;
  case ISD::FP_ROUND_INREG:    SplitVecRes_InregOp(N, Lo, Hi); break;
  case ISD::FPOWI:             SplitVecRes_FPOWI(N, Lo, Hi); break;
  case ISD::INSERT_VECTOR_ELT: SplitVecRes_INSERT_VECTOR_ELT(N, Lo, Hi); break;
  case ISD::SCALAR_TO_VECTOR:  SplitVecRes_SCALAR_TO_VECTOR(N, Lo, Hi); break;
  case ISD::SIGN_EXTEND_INREG: SplitVecRes_InregOp(N, Lo, Hi); break;
  case ISD::LOAD:
    SplitVecRes_LOAD(cast<LoadSDNode>(N), Lo, Hi);
    break;
  case ISD::SETCC:
    SplitVecRes_SETCC(N, Lo, Hi);
    break;
  case ISD::VECTOR_SHUFFLE:
    SplitVecRes_VECTOR_SHUFFLE(cast<ShuffleVectorSDNode>(N), Lo, Hi);
    break;

  case ISD::ANY_EXTEND:
  case ISD::CONVERT_RNDSAT:
  case ISD::CTLZ:
  case ISD::CTTZ:
  case ISD::CTLZ_ZERO_UNDEF:
  case ISD::CTTZ_ZERO_UNDEF:
  case ISD::CTPOP:
  case ISD::FABS:
  case ISD::FCEIL:
  case ISD::FCOS:
  case ISD::FEXP:
  case ISD::FEXP2:
  case ISD::FFLOOR:
  case ISD::FLOG:
  case ISD::FLOG10:
  case ISD::FLOG2:
  case ISD::FNEARBYINT:
  case ISD::FNEG:
  case ISD::FP_EXTEND:
  case ISD::FP_ROUND:
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
  case ISD::FRINT:
  case ISD::FSIN:
  case ISD::FSQRT:
  case ISD::FTRUNC:
  case ISD::SIGN_EXTEND:
  case ISD::SINT_TO_FP:
  case ISD::TRUNCATE:
  case ISD::UINT_TO_FP:
  case ISD::ZERO_EXTEND:
    SplitVecRes_UnaryOp(N, Lo, Hi);
    break;

  case ISD::ADD:
  case ISD::SUB:
  case ISD::MUL:
  case ISD::FADD:
  case ISD::FSUB:
  case ISD::FMUL:
  case ISD::SDIV:
  case ISD::UDIV:
  case ISD::FDIV:
  case ISD::FPOW:
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
  case ISD::SHL:
  case ISD::SRA:
  case ISD::SRL:
  case ISD::UREM:
  case ISD::SREM:
  case ISD::FREM:
    SplitVecRes_BinOp(N, Lo, Hi);
    break;
  }

  // If Lo/Hi is null, the sub-method took care of registering results etc.
  if (Lo.getNode())
    SetSplitVector(SDValue(N, ResNo), Lo, Hi);
}

void DAGTypeLegalizer::SplitVecRes_BinOp(SDNode *N, SDValue &Lo,
                                         SDValue &Hi) {
  SDValue LHSLo, LHSHi;
  GetSplitVector(N->getOperand(0), LHSLo, LHSHi);
  SDValue RHSLo, RHSHi;
  GetSplitVector(N->getOperand(1), RHSLo, RHSHi);
  DebugLoc dl = N->getDebugLoc();

  Lo = DAG.getNode(N->getOpcode(), dl, LHSLo.getValueType(), LHSLo, RHSLo);
  Hi = DAG.getNode(N->getOpcode(), dl, LHSHi.getValueType(), LHSHi, RHSHi);
}

void DAGTypeLegalizer::SplitVecRes_BITCAST(SDNode *N, SDValue &Lo,
                                           SDValue &Hi) {
  // We know the result is a vector.  The input may be either a vector or a
  // scalar value.
  EVT LoVT, HiVT;
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);
  DebugLoc dl = N->getDebugLoc();

  SDValue InOp = N->getOperand(0);
  EVT InVT = InOp.getValueType();

  // Handle some special cases efficiently.
  switch (getTypeAction(InVT)) {
  case TargetLowering::TypeLegal:
  case TargetLowering::TypePromoteInteger:
  case TargetLowering::TypeSoftenFloat:
  case TargetLowering::TypeScalarizeVector:
  case TargetLowering::TypeWidenVector:
    break;
  case TargetLowering::TypeExpandInteger:
  case TargetLowering::TypeExpandFloat:
    // A scalar to vector conversion, where the scalar needs expansion.
    // If the vector is being split in two then we can just convert the
    // expanded pieces.
    if (LoVT == HiVT) {
      GetExpandedOp(InOp, Lo, Hi);
      if (TLI.isBigEndian())
        std::swap(Lo, Hi);
      Lo = DAG.getNode(ISD::BITCAST, dl, LoVT, Lo);
      Hi = DAG.getNode(ISD::BITCAST, dl, HiVT, Hi);
      return;
    }
    break;
  case TargetLowering::TypeSplitVector:
    // If the input is a vector that needs to be split, convert each split
    // piece of the input now.
    GetSplitVector(InOp, Lo, Hi);
    Lo = DAG.getNode(ISD::BITCAST, dl, LoVT, Lo);
    Hi = DAG.getNode(ISD::BITCAST, dl, HiVT, Hi);
    return;
  }

  // In the general case, convert the input to an integer and split it by hand.
  EVT LoIntVT = EVT::getIntegerVT(*DAG.getContext(), LoVT.getSizeInBits());
  EVT HiIntVT = EVT::getIntegerVT(*DAG.getContext(), HiVT.getSizeInBits());
  if (TLI.isBigEndian())
    std::swap(LoIntVT, HiIntVT);

  SplitInteger(BitConvertToInteger(InOp), LoIntVT, HiIntVT, Lo, Hi);

  if (TLI.isBigEndian())
    std::swap(Lo, Hi);
  Lo = DAG.getNode(ISD::BITCAST, dl, LoVT, Lo);
  Hi = DAG.getNode(ISD::BITCAST, dl, HiVT, Hi);
}

void DAGTypeLegalizer::SplitVecRes_BUILD_VECTOR(SDNode *N, SDValue &Lo,
                                                SDValue &Hi) {
  EVT LoVT, HiVT;
  DebugLoc dl = N->getDebugLoc();
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);
  unsigned LoNumElts = LoVT.getVectorNumElements();
  SmallVector<SDValue, 8> LoOps(N->op_begin(), N->op_begin()+LoNumElts);
  Lo = DAG.getNode(ISD::BUILD_VECTOR, dl, LoVT, &LoOps[0], LoOps.size());

  SmallVector<SDValue, 8> HiOps(N->op_begin()+LoNumElts, N->op_end());
  Hi = DAG.getNode(ISD::BUILD_VECTOR, dl, HiVT, &HiOps[0], HiOps.size());
}

void DAGTypeLegalizer::SplitVecRes_CONCAT_VECTORS(SDNode *N, SDValue &Lo,
                                                  SDValue &Hi) {
  assert(!(N->getNumOperands() & 1) && "Unsupported CONCAT_VECTORS");
  DebugLoc dl = N->getDebugLoc();
  unsigned NumSubvectors = N->getNumOperands() / 2;
  if (NumSubvectors == 1) {
    Lo = N->getOperand(0);
    Hi = N->getOperand(1);
    return;
  }

  EVT LoVT, HiVT;
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);

  SmallVector<SDValue, 8> LoOps(N->op_begin(), N->op_begin()+NumSubvectors);
  Lo = DAG.getNode(ISD::CONCAT_VECTORS, dl, LoVT, &LoOps[0], LoOps.size());

  SmallVector<SDValue, 8> HiOps(N->op_begin()+NumSubvectors, N->op_end());
  Hi = DAG.getNode(ISD::CONCAT_VECTORS, dl, HiVT, &HiOps[0], HiOps.size());
}

void DAGTypeLegalizer::SplitVecRes_EXTRACT_SUBVECTOR(SDNode *N, SDValue &Lo,
                                                     SDValue &Hi) {
  SDValue Vec = N->getOperand(0);
  SDValue Idx = N->getOperand(1);
  DebugLoc dl = N->getDebugLoc();

  EVT LoVT, HiVT;
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);

  Lo = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, LoVT, Vec, Idx);
  uint64_t IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
  Hi = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, HiVT, Vec,
                   DAG.getIntPtrConstant(IdxVal + LoVT.getVectorNumElements()));
}

void DAGTypeLegalizer::SplitVecRes_FPOWI(SDNode *N, SDValue &Lo,
                                         SDValue &Hi) {
  DebugLoc dl = N->getDebugLoc();
  GetSplitVector(N->getOperand(0), Lo, Hi);
  Lo = DAG.getNode(ISD::FPOWI, dl, Lo.getValueType(), Lo, N->getOperand(1));
  Hi = DAG.getNode(ISD::FPOWI, dl, Hi.getValueType(), Hi, N->getOperand(1));
}

void DAGTypeLegalizer::SplitVecRes_InregOp(SDNode *N, SDValue &Lo,
                                           SDValue &Hi) {
  SDValue LHSLo, LHSHi;
  GetSplitVector(N->getOperand(0), LHSLo, LHSHi);
  DebugLoc dl = N->getDebugLoc();

  EVT LoVT, HiVT;
  GetSplitDestVTs(cast<VTSDNode>(N->getOperand(1))->getVT(), LoVT, HiVT);

  Lo = DAG.getNode(N->getOpcode(), dl, LHSLo.getValueType(), LHSLo,
                   DAG.getValueType(LoVT));
  Hi = DAG.getNode(N->getOpcode(), dl, LHSHi.getValueType(), LHSHi,
                   DAG.getValueType(HiVT));
}

void DAGTypeLegalizer::SplitVecRes_INSERT_VECTOR_ELT(SDNode *N, SDValue &Lo,
                                                     SDValue &Hi) {
  SDValue Vec = N->getOperand(0);
  SDValue Elt = N->getOperand(1);
  SDValue Idx = N->getOperand(2);
  DebugLoc dl = N->getDebugLoc();
  GetSplitVector(Vec, Lo, Hi);

  if (ConstantSDNode *CIdx = dyn_cast<ConstantSDNode>(Idx)) {
    unsigned IdxVal = CIdx->getZExtValue();
    unsigned LoNumElts = Lo.getValueType().getVectorNumElements();
    if (IdxVal < LoNumElts)
      Lo = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl,
                       Lo.getValueType(), Lo, Elt, Idx);
    else
      Hi = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, Hi.getValueType(), Hi, Elt,
                       DAG.getIntPtrConstant(IdxVal - LoNumElts));
    return;
  }

  // Spill the vector to the stack.
  EVT VecVT = Vec.getValueType();
  EVT EltVT = VecVT.getVectorElementType();
  SDValue StackPtr = DAG.CreateStackTemporary(VecVT);
  SDValue Store = DAG.getStore(DAG.getEntryNode(), dl, Vec, StackPtr,
                               MachinePointerInfo(), false, false, 0);

  // Store the new element.  This may be larger than the vector element type,
  // so use a truncating store.
  SDValue EltPtr = GetVectorElementPointer(StackPtr, EltVT, Idx);
  Type *VecType = VecVT.getTypeForEVT(*DAG.getContext());
  unsigned Alignment =
    TLI.getTargetData()->getPrefTypeAlignment(VecType);
  Store = DAG.getTruncStore(Store, dl, Elt, EltPtr, MachinePointerInfo(), EltVT,
                            false, false, 0);

  // Load the Lo part from the stack slot.
  Lo = DAG.getLoad(Lo.getValueType(), dl, Store, StackPtr, MachinePointerInfo(),
                   false, false, false, 0);

  // Increment the pointer to the other part.
  unsigned IncrementSize = Lo.getValueType().getSizeInBits() / 8;
  StackPtr = DAG.getNode(ISD::ADD, dl, StackPtr.getValueType(), StackPtr,
                         DAG.getIntPtrConstant(IncrementSize));

  // Load the Hi part from the stack slot.
  Hi = DAG.getLoad(Hi.getValueType(), dl, Store, StackPtr, MachinePointerInfo(),
                   false, false, false, MinAlign(Alignment, IncrementSize));
}

void DAGTypeLegalizer::SplitVecRes_SCALAR_TO_VECTOR(SDNode *N, SDValue &Lo,
                                                    SDValue &Hi) {
  EVT LoVT, HiVT;
  DebugLoc dl = N->getDebugLoc();
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);
  Lo = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, LoVT, N->getOperand(0));
  Hi = DAG.getUNDEF(HiVT);
}

void DAGTypeLegalizer::SplitVecRes_LOAD(LoadSDNode *LD, SDValue &Lo,
                                        SDValue &Hi) {
  assert(ISD::isUNINDEXEDLoad(LD) && "Indexed load during type legalization!");
  EVT LoVT, HiVT;
  DebugLoc dl = LD->getDebugLoc();
  GetSplitDestVTs(LD->getValueType(0), LoVT, HiVT);

  ISD::LoadExtType ExtType = LD->getExtensionType();
  SDValue Ch = LD->getChain();
  SDValue Ptr = LD->getBasePtr();
  SDValue Offset = DAG.getUNDEF(Ptr.getValueType());
  EVT MemoryVT = LD->getMemoryVT();
  unsigned Alignment = LD->getOriginalAlignment();
  bool isVolatile = LD->isVolatile();
  bool isNonTemporal = LD->isNonTemporal();
  bool isInvariant = LD->isInvariant();

  EVT LoMemVT, HiMemVT;
  GetSplitDestVTs(MemoryVT, LoMemVT, HiMemVT);

  Lo = DAG.getLoad(ISD::UNINDEXED, ExtType, LoVT, dl, Ch, Ptr, Offset,
                   LD->getPointerInfo(), LoMemVT, isVolatile, isNonTemporal,
                   isInvariant, Alignment);

  unsigned IncrementSize = LoMemVT.getSizeInBits()/8;
  Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr,
                    DAG.getIntPtrConstant(IncrementSize));
  Hi = DAG.getLoad(ISD::UNINDEXED, ExtType, HiVT, dl, Ch, Ptr, Offset,
                   LD->getPointerInfo().getWithOffset(IncrementSize),
                   HiMemVT, isVolatile, isNonTemporal, isInvariant, Alignment);

  // Build a factor node to remember that this load is independent of the
  // other one.
  Ch = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Lo.getValue(1),
                   Hi.getValue(1));

  // Legalized the chain result - switch anything that used the old chain to
  // use the new one.
  ReplaceValueWith(SDValue(LD, 1), Ch);
}

void DAGTypeLegalizer::SplitVecRes_SETCC(SDNode *N, SDValue &Lo, SDValue &Hi) {
  assert(N->getValueType(0).isVector() &&
         N->getOperand(0).getValueType().isVector() &&
         "Operand types must be vectors");

  EVT LoVT, HiVT;
  DebugLoc DL = N->getDebugLoc();
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);

  // Split the input.
  EVT InVT = N->getOperand(0).getValueType();
  SDValue LL, LH, RL, RH;
  EVT InNVT = EVT::getVectorVT(*DAG.getContext(), InVT.getVectorElementType(),
                               LoVT.getVectorNumElements());
  LL = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, InNVT, N->getOperand(0),
                   DAG.getIntPtrConstant(0));
  LH = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, InNVT, N->getOperand(0),
                   DAG.getIntPtrConstant(InNVT.getVectorNumElements()));

  RL = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, InNVT, N->getOperand(1),
                   DAG.getIntPtrConstant(0));
  RH = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, InNVT, N->getOperand(1),
                   DAG.getIntPtrConstant(InNVT.getVectorNumElements()));

  Lo = DAG.getNode(N->getOpcode(), DL, LoVT, LL, RL, N->getOperand(2));
  Hi = DAG.getNode(N->getOpcode(), DL, HiVT, LH, RH, N->getOperand(2));
}

void DAGTypeLegalizer::SplitVecRes_UnaryOp(SDNode *N, SDValue &Lo,
                                           SDValue &Hi) {
  // Get the dest types - they may not match the input types, e.g. int_to_fp.
  EVT LoVT, HiVT;
  DebugLoc dl = N->getDebugLoc();
  GetSplitDestVTs(N->getValueType(0), LoVT, HiVT);

  // If the input also splits, handle it directly for a compile time speedup.
  // Otherwise split it by hand.
  EVT InVT = N->getOperand(0).getValueType();
  if (getTypeAction(InVT) == TargetLowering::TypeSplitVector) {
    GetSplitVector(N->getOperand(0), Lo, Hi);
  } else {
    EVT InNVT = EVT::getVectorVT(*DAG.getContext(), InVT.getVectorElementType(),
                                 LoVT.getVectorNumElements());
    Lo = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, InNVT, N->getOperand(0),
                     DAG.getIntPtrConstant(0));
    Hi = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, InNVT, N->getOperand(0),
                     DAG.getIntPtrConstant(InNVT.getVectorNumElements()));
  }

  if (N->getOpcode() == ISD::FP_ROUND) {
    Lo = DAG.getNode(N->getOpcode(), dl, LoVT, Lo, N->getOperand(1));
    Hi = DAG.getNode(N->getOpcode(), dl, HiVT, Hi, N->getOperand(1));
  } else if (N->getOpcode() == ISD::CONVERT_RNDSAT) {
    SDValue DTyOpLo = DAG.getValueType(LoVT);
    SDValue DTyOpHi = DAG.getValueType(HiVT);
    SDValue STyOpLo = DAG.getValueType(Lo.getValueType());
    SDValue STyOpHi = DAG.getValueType(Hi.getValueType());
    SDValue RndOp = N->getOperand(3);
    SDValue SatOp = N->getOperand(4);
    ISD::CvtCode CvtCode = cast<CvtRndSatSDNode>(N)->getCvtCode();
    Lo = DAG.getConvertRndSat(LoVT, dl, Lo, DTyOpLo, STyOpLo, RndOp, SatOp,
                              CvtCode);
    Hi = DAG.getConvertRndSat(HiVT, dl, Hi, DTyOpHi, STyOpHi, RndOp, SatOp,
                              CvtCode);
  } else {
    Lo = DAG.getNode(N->getOpcode(), dl, LoVT, Lo);
    Hi = DAG.getNode(N->getOpcode(), dl, HiVT, Hi);
  }
}

void DAGTypeLegalizer::SplitVecRes_VECTOR_SHUFFLE(ShuffleVectorSDNode *N,
                                                  SDValue &Lo, SDValue &Hi) {
  // The low and high parts of the original input give four input vectors.
  SDValue Inputs[4];
  DebugLoc dl = N->getDebugLoc();
  GetSplitVector(N->getOperand(0), Inputs[0], Inputs[1]);
  GetSplitVector(N->getOperand(1), Inputs[2], Inputs[3]);
  EVT NewVT = Inputs[0].getValueType();
  unsigned NewElts = NewVT.getVectorNumElements();

  // If Lo or Hi uses elements from at most two of the four input vectors, then
  // express it as a vector shuffle of those two inputs.  Otherwise extract the
  // input elements by hand and construct the Lo/Hi output using a BUILD_VECTOR.
  SmallVector<int, 16> Ops;
  for (unsigned High = 0; High < 2; ++High) {
    SDValue &Output = High ? Hi : Lo;

    // Build a shuffle mask for the output, discovering on the fly which
    // input vectors to use as shuffle operands (recorded in InputUsed).
    // If building a suitable shuffle vector proves too hard, then bail
    // out with useBuildVector set.
    unsigned InputUsed[2] = { -1U, -1U }; // Not yet discovered.
    unsigned FirstMaskIdx = High * NewElts;
    bool useBuildVector = false;
    for (unsigned MaskOffset = 0; MaskOffset < NewElts; ++MaskOffset) {
      // The mask element.  This indexes into the input.
      int Idx = N->getMaskElt(FirstMaskIdx + MaskOffset);

      // The input vector this mask element indexes into.
      unsigned Input = (unsigned)Idx / NewElts;

      if (Input >= array_lengthof(Inputs)) {
        // The mask element does not index into any input vector.
        Ops.push_back(-1);
        continue;
      }

      // Turn the index into an offset from the start of the input vector.
      Idx -= Input * NewElts;

      // Find or create a shuffle vector operand to hold this input.
      unsigned OpNo;
      for (OpNo = 0; OpNo < array_lengthof(InputUsed); ++OpNo) {
        if (InputUsed[OpNo] == Input) {
          // This input vector is already an operand.
          break;
        } else if (InputUsed[OpNo] == -1U) {
          // Create a new operand for this input vector.
          InputUsed[OpNo] = Input;
          break;
        }
      }

      if (OpNo >= array_lengthof(InputUsed)) {
        // More than two input vectors used!  Give up on trying to create a
        // shuffle vector.  Insert all elements into a BUILD_VECTOR instead.
        useBuildVector = true;
        break;
      }

      // Add the mask index for the new shuffle vector.
      Ops.push_back(Idx + OpNo * NewElts);
    }

    if (useBuildVector) {
      EVT EltVT = NewVT.getVectorElementType();
      SmallVector<SDValue, 16> SVOps;

      // Extract the input elements by hand.
      for (unsigned MaskOffset = 0; MaskOffset < NewElts; ++MaskOffset) {
        // The mask element.  This indexes into the input.
        int Idx = N->getMaskElt(FirstMaskIdx + MaskOffset);

        // The input vector this mask element indexes into.
        unsigned Input = (unsigned)Idx / NewElts;

        if (Input >= array_lengthof(Inputs)) {
          // The mask element is "undef" or indexes off the end of the input.
          SVOps.push_back(DAG.getUNDEF(EltVT));
          continue;
        }

        // Turn the index into an offset from the start of the input vector.
        Idx -= Input * NewElts;

        // Extract the vector element by hand.
        SVOps.push_back(DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT,
                                    Inputs[Input], DAG.getIntPtrConstant(Idx)));
      }

      // Construct the Lo/Hi output using a BUILD_VECTOR.
      Output = DAG.getNode(ISD::BUILD_VECTOR,dl,NewVT, &SVOps[0], SVOps.size());
    } else if (InputUsed[0] == -1U) {
      // No input vectors were used!  The result is undefined.
      Output = DAG.getUNDEF(NewVT);
    } else {
      SDValue Op0 = Inputs[InputUsed[0]];
      // If only one input was used, use an undefined vector for the other.
      SDValue Op1 = InputUsed[1] == -1U ?
        DAG.getUNDEF(NewVT) : Inputs[InputUsed[1]];
      // At least one input vector was used.  Create a new shuffle vector.
      Output =  DAG.getVectorShuffle(NewVT, dl, Op0, Op1, &Ops[0]);
    }

    Ops.clear();
  }
}


//===----------------------------------------------------------------------===//
//  Operand Vector Splitting
//===----------------------------------------------------------------------===//

/// SplitVectorOperand - This method is called when the specified operand of the
/// specified node is found to need vector splitting.  At this point, all of the
/// result types of the node are known to be legal, but other operands of the
/// node may need legalization as well as the specified one.
bool DAGTypeLegalizer::SplitVectorOperand(SDNode *N, unsigned OpNo) {
  DEBUG(dbgs() << "Split node operand: ";
        N->dump(&DAG);
        dbgs() << "\n");
  SDValue Res = SDValue();

  if (Res.getNode() == 0) {
    switch (N->getOpcode()) {
    default:
#ifndef NDEBUG
      dbgs() << "SplitVectorOperand Op #" << OpNo << ": ";
      N->dump(&DAG);
      dbgs() << "\n";
#endif
      llvm_unreachable("Do not know how to split this operator's operand!");
    case ISD::SETCC:             Res = SplitVecOp_VSETCC(N); break;
    case ISD::BITCAST:           Res = SplitVecOp_BITCAST(N); break;
    case ISD::EXTRACT_SUBVECTOR: Res = SplitVecOp_EXTRACT_SUBVECTOR(N); break;
    case ISD::EXTRACT_VECTOR_ELT:Res = SplitVecOp_EXTRACT_VECTOR_ELT(N); break;
    case ISD::CONCAT_VECTORS:    Res = SplitVecOp_CONCAT_VECTORS(N); break;
    case ISD::FP_ROUND:          Res = SplitVecOp_FP_ROUND(N); break;
    case ISD::STORE:
      Res = SplitVecOp_STORE(cast<StoreSDNode>(N), OpNo);
      break;

    case ISD::CTTZ:
    case ISD::CTLZ:
    case ISD::CTPOP:
    case ISD::FP_EXTEND:
    case ISD::FP_TO_SINT:
    case ISD::FP_TO_UINT:
    case ISD::SINT_TO_FP:
    case ISD::UINT_TO_FP:
    case ISD::FTRUNC:
    case ISD::TRUNCATE:
    case ISD::SIGN_EXTEND:
    case ISD::ZERO_EXTEND:
    case ISD::ANY_EXTEND:
      Res = SplitVecOp_UnaryOp(N);
      break;
    }
  }

  // If the result is null, the sub-method took care of registering results etc.
  if (!Res.getNode()) return false;

  // If the result is N, the sub-method updated N in place.  Tell the legalizer
  // core about this.
  if (Res.getNode() == N)
    return true;

  assert(Res.getValueType() == N->getValueType(0) && N->getNumValues() == 1 &&
         "Invalid operand expansion");

  ReplaceValueWith(SDValue(N, 0), Res);
  return false;
}

SDValue DAGTypeLegalizer::SplitVecOp_UnaryOp(SDNode *N) {
  // The result has a legal vector type, but the input needs splitting.
  EVT ResVT = N->getValueType(0);
  SDValue Lo, Hi;
  DebugLoc dl = N->getDebugLoc();
  GetSplitVector(N->getOperand(0), Lo, Hi);
  EVT InVT = Lo.getValueType();

  EVT OutVT = EVT::getVectorVT(*DAG.getContext(), ResVT.getVectorElementType(),
                               InVT.getVectorNumElements());

  Lo = DAG.getNode(N->getOpcode(), dl, OutVT, Lo);
  Hi = DAG.getNode(N->getOpcode(), dl, OutVT, Hi);

  return DAG.getNode(ISD::CONCAT_VECTORS, dl, ResVT, Lo, Hi);
}

SDValue DAGTypeLegalizer::SplitVecOp_BITCAST(SDNode *N) {
  // For example, i64 = BITCAST v4i16 on alpha.  Typically the vector will
  // end up being split all the way down to individual components.  Convert the
  // split pieces into integers and reassemble.
  SDValue Lo, Hi;
  GetSplitVector(N->getOperand(0), Lo, Hi);
  Lo = BitConvertToInteger(Lo);
  Hi = BitConvertToInteger(Hi);

  if (TLI.isBigEndian())
    std::swap(Lo, Hi);

  return DAG.getNode(ISD::BITCAST, N->getDebugLoc(), N->getValueType(0),
                     JoinIntegers(Lo, Hi));
}

SDValue DAGTypeLegalizer::SplitVecOp_EXTRACT_SUBVECTOR(SDNode *N) {
  // We know that the extracted result type is legal.
  EVT SubVT = N->getValueType(0);
  SDValue Idx = N->getOperand(1);
  DebugLoc dl = N->getDebugLoc();
  SDValue Lo, Hi;
  GetSplitVector(N->getOperand(0), Lo, Hi);

  uint64_t LoElts = Lo.getValueType().getVectorNumElements();
  uint64_t IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();

  if (IdxVal < LoElts) {
    assert(IdxVal + SubVT.getVectorNumElements() <= LoElts &&
           "Extracted subvector crosses vector split!");
    return DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, SubVT, Lo, Idx);
  } else {
    return DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, SubVT, Hi,
                       DAG.getConstant(IdxVal - LoElts, Idx.getValueType()));
  }
}

SDValue DAGTypeLegalizer::SplitVecOp_EXTRACT_VECTOR_ELT(SDNode *N) {
  SDValue Vec = N->getOperand(0);
  SDValue Idx = N->getOperand(1);
  EVT VecVT = Vec.getValueType();

  if (isa<ConstantSDNode>(Idx)) {
    uint64_t IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
    assert(IdxVal < VecVT.getVectorNumElements() && "Invalid vector index!");

    SDValue Lo, Hi;
    GetSplitVector(Vec, Lo, Hi);

    uint64_t LoElts = Lo.getValueType().getVectorNumElements();

    if (IdxVal < LoElts)
      return SDValue(DAG.UpdateNodeOperands(N, Lo, Idx), 0);
    return SDValue(DAG.UpdateNodeOperands(N, Hi,
                                  DAG.getConstant(IdxVal - LoElts,
                                                  Idx.getValueType())), 0);
  }

  // Store the vector to the stack.
  EVT EltVT = VecVT.getVectorElementType();
  DebugLoc dl = N->getDebugLoc();
  SDValue StackPtr = DAG.CreateStackTemporary(VecVT);
  SDValue Store = DAG.getStore(DAG.getEntryNode(), dl, Vec, StackPtr,
                               MachinePointerInfo(), false, false, 0);

  // Load back the required element.
  StackPtr = GetVectorElementPointer(StackPtr, EltVT, Idx);
  return DAG.getExtLoad(ISD::EXTLOAD, dl, N->getValueType(0), Store, StackPtr,
                        MachinePointerInfo(), EltVT, false, false, 0);
}

SDValue DAGTypeLegalizer::SplitVecOp_STORE(StoreSDNode *N, unsigned OpNo) {
  assert(N->isUnindexed() && "Indexed store of vector?");
  assert(OpNo == 1 && "Can only split the stored value");
  DebugLoc DL = N->getDebugLoc();

  bool isTruncating = N->isTruncatingStore();
  SDValue Ch  = N->getChain();
  SDValue Ptr = N->getBasePtr();
  EVT MemoryVT = N->getMemoryVT();
  unsigned Alignment = N->getOriginalAlignment();
  bool isVol = N->isVolatile();
  bool isNT = N->isNonTemporal();
  SDValue Lo, Hi;
  GetSplitVector(N->getOperand(1), Lo, Hi);

  EVT LoMemVT, HiMemVT;
  GetSplitDestVTs(MemoryVT, LoMemVT, HiMemVT);

  unsigned IncrementSize = LoMemVT.getSizeInBits()/8;

  if (isTruncating)
    Lo = DAG.getTruncStore(Ch, DL, Lo, Ptr, N->getPointerInfo(),
                           LoMemVT, isVol, isNT, Alignment);
  else
    Lo = DAG.getStore(Ch, DL, Lo, Ptr, N->getPointerInfo(),
                      isVol, isNT, Alignment);

  // Increment the pointer to the other half.
  Ptr = DAG.getNode(ISD::ADD, DL, Ptr.getValueType(), Ptr,
                    DAG.getIntPtrConstant(IncrementSize));

  if (isTruncating)
    Hi = DAG.getTruncStore(Ch, DL, Hi, Ptr,
                           N->getPointerInfo().getWithOffset(IncrementSize),
                           HiMemVT, isVol, isNT, Alignment);
  else
    Hi = DAG.getStore(Ch, DL, Hi, Ptr,
                      N->getPointerInfo().getWithOffset(IncrementSize),
                      isVol, isNT, Alignment);

  return DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Lo, Hi);
}

SDValue DAGTypeLegalizer::SplitVecOp_CONCAT_VECTORS(SDNode *N) {
  DebugLoc DL = N->getDebugLoc();

  // The input operands all must have the same type, and we know the result the
  // result type is valid.  Convert this to a buildvector which extracts all the
  // input elements.
  // TODO: If the input elements are power-two vectors, we could convert this to
  // a new CONCAT_VECTORS node with elements that are half-wide.
  SmallVector<SDValue, 32> Elts;
  EVT EltVT = N->getValueType(0).getVectorElementType();
  for (unsigned op = 0, e = N->getNumOperands(); op != e; ++op) {
    SDValue Op = N->getOperand(op);
    for (unsigned i = 0, e = Op.getValueType().getVectorNumElements();
         i != e; ++i) {
      Elts.push_back(DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, EltVT,
                                 Op, DAG.getIntPtrConstant(i)));

    }
  }

  return DAG.getNode(ISD::BUILD_VECTOR, DL, N->getValueType(0),
                     &Elts[0], Elts.size());
}

SDValue DAGTypeLegalizer::SplitVecOp_VSETCC(SDNode *N) {
  assert(N->getValueType(0).isVector() &&
         N->getOperand(0).getValueType().isVector() &&
         "Operand types must be vectors");
  // The result has a legal vector type, but the input needs splitting.
  SDValue Lo0, Hi0, Lo1, Hi1, LoRes, HiRes;
  DebugLoc DL = N->getDebugLoc();
  GetSplitVector(N->getOperand(0), Lo0, Hi0);
  GetSplitVector(N->getOperand(1), Lo1, Hi1);
  unsigned PartElements = Lo0.getValueType().getVectorNumElements();
  EVT PartResVT = EVT::getVectorVT(*DAG.getContext(), MVT::i1, PartElements);
  EVT WideResVT = EVT::getVectorVT(*DAG.getContext(), MVT::i1, 2*PartElements);

  LoRes = DAG.getNode(ISD::SETCC, DL, PartResVT, Lo0, Lo1, N->getOperand(2));
  HiRes = DAG.getNode(ISD::SETCC, DL, PartResVT, Hi0, Hi1, N->getOperand(2));
  SDValue Con = DAG.getNode(ISD::CONCAT_VECTORS, DL, WideResVT, LoRes, HiRes);
  return PromoteTargetBoolean(Con, N->getValueType(0));
}


SDValue DAGTypeLegalizer::SplitVecOp_FP_ROUND(SDNode *N) {
  // The result has a legal vector type, but the input needs splitting.
  EVT ResVT = N->getValueType(0);
  SDValue Lo, Hi;
  DebugLoc DL = N->getDebugLoc();
  GetSplitVector(N->getOperand(0), Lo, Hi);
  EVT InVT = Lo.getValueType();
  
  EVT OutVT = EVT::getVectorVT(*DAG.getContext(), ResVT.getVectorElementType(),
                               InVT.getVectorNumElements());
  
  Lo = DAG.getNode(ISD::FP_ROUND, DL, OutVT, Lo, N->getOperand(1));
  Hi = DAG.getNode(ISD::FP_ROUND, DL, OutVT, Hi, N->getOperand(1));
  
  return DAG.getNode(ISD::CONCAT_VECTORS, DL, ResVT, Lo, Hi);
}  



//===----------------------------------------------------------------------===//
//  Result Vector Widening
//===----------------------------------------------------------------------===//

void DAGTypeLegalizer::WidenVectorResult(SDNode *N, unsigned ResNo) {
  DEBUG(dbgs() << "Widen node result " << ResNo << ": ";
        N->dump(&DAG);
        dbgs() << "\n");

  // See if the target wants to custom widen this node.
  if (CustomWidenLowerNode(N, N->getValueType(ResNo)))
    return;

  SDValue Res = SDValue();
  switch (N->getOpcode()) {
  default:
#ifndef NDEBUG
    dbgs() << "WidenVectorResult #" << ResNo << ": ";
    N->dump(&DAG);
    dbgs() << "\n";
#endif
    llvm_unreachable("Do not know how to widen the result of this operator!");

  case ISD::MERGE_VALUES:      Res = WidenVecRes_MERGE_VALUES(N, ResNo); break;
  case ISD::BITCAST:           Res = WidenVecRes_BITCAST(N); break;
  case ISD::BUILD_VECTOR:      Res = WidenVecRes_BUILD_VECTOR(N); break;
  case ISD::CONCAT_VECTORS:    Res = WidenVecRes_CONCAT_VECTORS(N); break;
  case ISD::CONVERT_RNDSAT:    Res = WidenVecRes_CONVERT_RNDSAT(N); break;
  case ISD::EXTRACT_SUBVECTOR: Res = WidenVecRes_EXTRACT_SUBVECTOR(N); break;
  case ISD::FP_ROUND_INREG:    Res = WidenVecRes_InregOp(N); break;
  case ISD::INSERT_VECTOR_ELT: Res = WidenVecRes_INSERT_VECTOR_ELT(N); break;
  case ISD::LOAD:              Res = WidenVecRes_LOAD(N); break;
  case ISD::SCALAR_TO_VECTOR:  Res = WidenVecRes_SCALAR_TO_VECTOR(N); break;
  case ISD::SIGN_EXTEND_INREG: Res = WidenVecRes_InregOp(N); break;
  case ISD::VSELECT:
  case ISD::SELECT:            Res = WidenVecRes_SELECT(N); break;
  case ISD::SELECT_CC:         Res = WidenVecRes_SELECT_CC(N); break;
  case ISD::SETCC:             Res = WidenVecRes_SETCC(N); break;
  case ISD::UNDEF:             Res = WidenVecRes_UNDEF(N); break;
  case ISD::VECTOR_SHUFFLE:
    Res = WidenVecRes_VECTOR_SHUFFLE(cast<ShuffleVectorSDNode>(N));
    break;
  case ISD::ADD:
  case ISD::AND:
  case ISD::BSWAP:
  case ISD::FADD:
  case ISD::FCOPYSIGN:
  case ISD::FDIV:
  case ISD::FMUL:
  case ISD::FPOW:
  case ISD::FREM:
  case ISD::FSUB:
  case ISD::MUL:
  case ISD::MULHS:
  case ISD::MULHU:
  case ISD::OR:
  case ISD::SDIV:
  case ISD::SREM:
  case ISD::UDIV:
  case ISD::UREM:
  case ISD::SUB:
  case ISD::XOR:
    Res = WidenVecRes_Binary(N);
    break;

  case ISD::FPOWI:
    Res = WidenVecRes_POWI(N);
    break;

  case ISD::SHL:
  case ISD::SRA:
  case ISD::SRL:
    Res = WidenVecRes_Shift(N);
    break;

  case ISD::ANY_EXTEND:
  case ISD::FP_EXTEND:
  case ISD::FP_ROUND:
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
  case ISD::SIGN_EXTEND:
  case ISD::SINT_TO_FP:
  case ISD::TRUNCATE:
  case ISD::UINT_TO_FP:
  case ISD::ZERO_EXTEND:
    Res = WidenVecRes_Convert(N);
    break;

  case ISD::CTLZ:
  case ISD::CTPOP:
  case ISD::CTTZ:
  case ISD::FABS:
  case ISD::FCEIL:
  case ISD::FCOS:
  case ISD::FEXP:
  case ISD::FEXP2:
  case ISD::FFLOOR:
  case ISD::FLOG:
  case ISD::FLOG10:
  case ISD::FLOG2:
  case ISD::FNEARBYINT:
  case ISD::FNEG:
  case ISD::FRINT:
  case ISD::FSIN:
  case ISD::FSQRT:
  case ISD::FTRUNC:
    Res = WidenVecRes_Unary(N);
    break;
  }

  // If Res is null, the sub-method took care of registering the result.
  if (Res.getNode())
    SetWidenedVector(SDValue(N, ResNo), Res);
}

SDValue DAGTypeLegalizer::WidenVecRes_Binary(SDNode *N) {
  // Binary op widening.
  unsigned Opcode = N->getOpcode();
  DebugLoc dl = N->getDebugLoc();
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  EVT WidenEltVT = WidenVT.getVectorElementType();
  EVT VT = WidenVT;
  unsigned NumElts =  VT.getVectorNumElements();
  while (!TLI.isTypeLegal(VT) && NumElts != 1) {
    NumElts = NumElts / 2;
    VT = EVT::getVectorVT(*DAG.getContext(), WidenEltVT, NumElts);
  }

  if (NumElts != 1 && !TLI.canOpTrap(N->getOpcode(), VT)) {
    // Operation doesn't trap so just widen as normal.
    SDValue InOp1 = GetWidenedVector(N->getOperand(0));
    SDValue InOp2 = GetWidenedVector(N->getOperand(1));
    return DAG.getNode(N->getOpcode(), dl, WidenVT, InOp1, InOp2);
  }

  // No legal vector version so unroll the vector operation and then widen.
  if (NumElts == 1)
    return DAG.UnrollVectorOp(N, WidenVT.getVectorNumElements());

  // Since the operation can trap, apply operation on the original vector.
  EVT MaxVT = VT;
  SDValue InOp1 = GetWidenedVector(N->getOperand(0));
  SDValue InOp2 = GetWidenedVector(N->getOperand(1));
  unsigned CurNumElts = N->getValueType(0).getVectorNumElements();

  SmallVector<SDValue, 16> ConcatOps(CurNumElts);
  unsigned ConcatEnd = 0;  // Current ConcatOps index.
  int Idx = 0;        // Current Idx into input vectors.

  // NumElts := greatest legal vector size (at most WidenVT)
  // while (orig. vector has unhandled elements) {
  //   take munches of size NumElts from the beginning and add to ConcatOps
  //   NumElts := next smaller supported vector size or 1
  // }
  while (CurNumElts != 0) {
    while (CurNumElts >= NumElts) {
      SDValue EOp1 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, VT, InOp1,
                                 DAG.getIntPtrConstant(Idx));
      SDValue EOp2 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, VT, InOp2,
                                 DAG.getIntPtrConstant(Idx));
      ConcatOps[ConcatEnd++] = DAG.getNode(Opcode, dl, VT, EOp1, EOp2);
      Idx += NumElts;
      CurNumElts -= NumElts;
    }
    do {
      NumElts = NumElts / 2;
      VT = EVT::getVectorVT(*DAG.getContext(), WidenEltVT, NumElts);
    } while (!TLI.isTypeLegal(VT) && NumElts != 1);

    if (NumElts == 1) {
      for (unsigned i = 0; i != CurNumElts; ++i, ++Idx) {
        SDValue EOp1 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, WidenEltVT,
                                   InOp1, DAG.getIntPtrConstant(Idx));
        SDValue EOp2 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, WidenEltVT,
                                   InOp2, DAG.getIntPtrConstant(Idx));
        ConcatOps[ConcatEnd++] = DAG.getNode(Opcode, dl, WidenEltVT,
                                             EOp1, EOp2);
      }
      CurNumElts = 0;
    }
  }

  // Check to see if we have a single operation with the widen type.
  if (ConcatEnd == 1) {
    VT = ConcatOps[0].getValueType();
    if (VT == WidenVT)
      return ConcatOps[0];
  }

  // while (Some element of ConcatOps is not of type MaxVT) {
  //   From the end of ConcatOps, collect elements of the same type and put
  //   them into an op of the next larger supported type
  // }
  while (ConcatOps[ConcatEnd-1].getValueType() != MaxVT) {
    Idx = ConcatEnd - 1;
    VT = ConcatOps[Idx--].getValueType();
    while (Idx >= 0 && ConcatOps[Idx].getValueType() == VT)
      Idx--;

    int NextSize = VT.isVector() ? VT.getVectorNumElements() : 1;
    EVT NextVT;
    do {
      NextSize *= 2;
      NextVT = EVT::getVectorVT(*DAG.getContext(), WidenEltVT, NextSize);
    } while (!TLI.isTypeLegal(NextVT));

    if (!VT.isVector()) {
      // Scalar type, create an INSERT_VECTOR_ELEMENT of type NextVT
      SDValue VecOp = DAG.getUNDEF(NextVT);
      unsigned NumToInsert = ConcatEnd - Idx - 1;
      for (unsigned i = 0, OpIdx = Idx+1; i < NumToInsert; i++, OpIdx++) {
        VecOp = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, NextVT, VecOp,
                            ConcatOps[OpIdx], DAG.getIntPtrConstant(i));
      }
      ConcatOps[Idx+1] = VecOp;
      ConcatEnd = Idx + 2;
    } else {
      // Vector type, create a CONCAT_VECTORS of type NextVT
      SDValue undefVec = DAG.getUNDEF(VT);
      unsigned OpsToConcat = NextSize/VT.getVectorNumElements();
      SmallVector<SDValue, 16> SubConcatOps(OpsToConcat);
      unsigned RealVals = ConcatEnd - Idx - 1;
      unsigned SubConcatEnd = 0;
      unsigned SubConcatIdx = Idx + 1;
      while (SubConcatEnd < RealVals)
        SubConcatOps[SubConcatEnd++] = ConcatOps[++Idx];
      while (SubConcatEnd < OpsToConcat)
        SubConcatOps[SubConcatEnd++] = undefVec;
      ConcatOps[SubConcatIdx] = DAG.getNode(ISD::CONCAT_VECTORS, dl,
                                            NextVT, &SubConcatOps[0],
                                            OpsToConcat);
      ConcatEnd = SubConcatIdx + 1;
    }
  }

  // Check to see if we have a single operation with the widen type.
  if (ConcatEnd == 1) {
    VT = ConcatOps[0].getValueType();
    if (VT == WidenVT)
      return ConcatOps[0];
  }

  // add undefs of size MaxVT until ConcatOps grows to length of WidenVT
  unsigned NumOps = WidenVT.getVectorNumElements()/MaxVT.getVectorNumElements();
  if (NumOps != ConcatEnd ) {
    SDValue UndefVal = DAG.getUNDEF(MaxVT);
    for (unsigned j = ConcatEnd; j < NumOps; ++j)
      ConcatOps[j] = UndefVal;
  }
  return DAG.getNode(ISD::CONCAT_VECTORS, dl, WidenVT, &ConcatOps[0], NumOps);
}

SDValue DAGTypeLegalizer::WidenVecRes_Convert(SDNode *N) {
  SDValue InOp = N->getOperand(0);
  DebugLoc DL = N->getDebugLoc();

  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  EVT InVT = InOp.getValueType();
  EVT InEltVT = InVT.getVectorElementType();
  EVT InWidenVT = EVT::getVectorVT(*DAG.getContext(), InEltVT, WidenNumElts);

  unsigned Opcode = N->getOpcode();
  unsigned InVTNumElts = InVT.getVectorNumElements();

  if (getTypeAction(InVT) == TargetLowering::TypeWidenVector) {
    InOp = GetWidenedVector(N->getOperand(0));
    InVT = InOp.getValueType();
    InVTNumElts = InVT.getVectorNumElements();
    if (InVTNumElts == WidenNumElts) {
      if (N->getNumOperands() == 1)
        return DAG.getNode(Opcode, DL, WidenVT, InOp);
      return DAG.getNode(Opcode, DL, WidenVT, InOp, N->getOperand(1));
    }
  }

  if (TLI.isTypeLegal(InWidenVT)) {
    // Because the result and the input are different vector types, widening
    // the result could create a legal type but widening the input might make
    // it an illegal type that might lead to repeatedly splitting the input
    // and then widening it. To avoid this, we widen the input only if
    // it results in a legal type.
    if (WidenNumElts % InVTNumElts == 0) {
      // Widen the input and call convert on the widened input vector.
      unsigned NumConcat = WidenNumElts/InVTNumElts;
      SmallVector<SDValue, 16> Ops(NumConcat);
      Ops[0] = InOp;
      SDValue UndefVal = DAG.getUNDEF(InVT);
      for (unsigned i = 1; i != NumConcat; ++i)
        Ops[i] = UndefVal;
      SDValue InVec = DAG.getNode(ISD::CONCAT_VECTORS, DL, InWidenVT,
                                  &Ops[0], NumConcat);
      if (N->getNumOperands() == 1)
        return DAG.getNode(Opcode, DL, WidenVT, InVec);
      return DAG.getNode(Opcode, DL, WidenVT, InVec, N->getOperand(1));
    }

    if (InVTNumElts % WidenNumElts == 0) {
      SDValue InVal = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, InWidenVT,
                                  InOp, DAG.getIntPtrConstant(0));
      // Extract the input and convert the shorten input vector.
      if (N->getNumOperands() == 1)
        return DAG.getNode(Opcode, DL, WidenVT, InVal);
      return DAG.getNode(Opcode, DL, WidenVT, InVal, N->getOperand(1));
    }
  }

  // Otherwise unroll into some nasty scalar code and rebuild the vector.
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  EVT EltVT = WidenVT.getVectorElementType();
  unsigned MinElts = std::min(InVTNumElts, WidenNumElts);
  unsigned i;
  for (i=0; i < MinElts; ++i) {
    SDValue Val = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, InEltVT, InOp,
                              DAG.getIntPtrConstant(i));
    if (N->getNumOperands() == 1)
      Ops[i] = DAG.getNode(Opcode, DL, EltVT, Val);
    else
      Ops[i] = DAG.getNode(Opcode, DL, EltVT, Val, N->getOperand(1));
  }

  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for (; i < WidenNumElts; ++i)
    Ops[i] = UndefVal;

  return DAG.getNode(ISD::BUILD_VECTOR, DL, WidenVT, &Ops[0], WidenNumElts);
}

SDValue DAGTypeLegalizer::WidenVecRes_POWI(SDNode *N) {
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  SDValue ShOp = N->getOperand(1);
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(), WidenVT, InOp, ShOp);
}

SDValue DAGTypeLegalizer::WidenVecRes_Shift(SDNode *N) {
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  SDValue ShOp = N->getOperand(1);

  EVT ShVT = ShOp.getValueType();
  if (getTypeAction(ShVT) == TargetLowering::TypeWidenVector) {
    ShOp = GetWidenedVector(ShOp);
    ShVT = ShOp.getValueType();
  }
  EVT ShWidenVT = EVT::getVectorVT(*DAG.getContext(),
                                   ShVT.getVectorElementType(),
                                   WidenVT.getVectorNumElements());
  if (ShVT != ShWidenVT)
    ShOp = ModifyToType(ShOp, ShWidenVT);

  return DAG.getNode(N->getOpcode(), N->getDebugLoc(), WidenVT, InOp, ShOp);
}

SDValue DAGTypeLegalizer::WidenVecRes_Unary(SDNode *N) {
  // Unary op widening.
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(), WidenVT, InOp);
}

SDValue DAGTypeLegalizer::WidenVecRes_InregOp(SDNode *N) {
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  EVT ExtVT = EVT::getVectorVT(*DAG.getContext(),
                               cast<VTSDNode>(N->getOperand(1))->getVT()
                                 .getVectorElementType(),
                               WidenVT.getVectorNumElements());
  SDValue WidenLHS = GetWidenedVector(N->getOperand(0));
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(),
                     WidenVT, WidenLHS, DAG.getValueType(ExtVT));
}

SDValue DAGTypeLegalizer::WidenVecRes_MERGE_VALUES(SDNode *N, unsigned ResNo) {
  SDValue WidenVec = DisintegrateMERGE_VALUES(N, ResNo);
  return GetWidenedVector(WidenVec);
}

SDValue DAGTypeLegalizer::WidenVecRes_BITCAST(SDNode *N) {
  SDValue InOp = N->getOperand(0);
  EVT InVT = InOp.getValueType();
  EVT VT = N->getValueType(0);
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), VT);
  DebugLoc dl = N->getDebugLoc();

  switch (getTypeAction(InVT)) {
  default:
    assert(false && "Unknown type action!");
    break;
  case TargetLowering::TypeLegal:
    break;
  case TargetLowering::TypePromoteInteger:
    // If the incoming type is a vector that is being promoted, then
    // we know that the elements are arranged differently and that we
    // must perform the conversion using a stack slot.
    if (InVT.isVector())
      break;

    // If the InOp is promoted to the same size, convert it.  Otherwise,
    // fall out of the switch and widen the promoted input.
    InOp = GetPromotedInteger(InOp);
    InVT = InOp.getValueType();
    if (WidenVT.bitsEq(InVT))
      return DAG.getNode(ISD::BITCAST, dl, WidenVT, InOp);
    break;
  case TargetLowering::TypeSoftenFloat:
  case TargetLowering::TypeExpandInteger:
  case TargetLowering::TypeExpandFloat:
  case TargetLowering::TypeScalarizeVector:
  case TargetLowering::TypeSplitVector:
    break;
  case TargetLowering::TypeWidenVector:
    // If the InOp is widened to the same size, convert it.  Otherwise, fall
    // out of the switch and widen the widened input.
    InOp = GetWidenedVector(InOp);
    InVT = InOp.getValueType();
    if (WidenVT.bitsEq(InVT))
      // The input widens to the same size. Convert to the widen value.
      return DAG.getNode(ISD::BITCAST, dl, WidenVT, InOp);
    break;
  }

  unsigned WidenSize = WidenVT.getSizeInBits();
  unsigned InSize = InVT.getSizeInBits();
  // x86mmx is not an acceptable vector element type, so don't try.
  if (WidenSize % InSize == 0 && InVT != MVT::x86mmx) {
    // Determine new input vector type.  The new input vector type will use
    // the same element type (if its a vector) or use the input type as a
    // vector.  It is the same size as the type to widen to.
    EVT NewInVT;
    unsigned NewNumElts = WidenSize / InSize;
    if (InVT.isVector()) {
      EVT InEltVT = InVT.getVectorElementType();
      NewInVT = EVT::getVectorVT(*DAG.getContext(), InEltVT,
                                 WidenSize / InEltVT.getSizeInBits());
    } else {
      NewInVT = EVT::getVectorVT(*DAG.getContext(), InVT, NewNumElts);
    }

    if (TLI.isTypeLegal(NewInVT)) {
      // Because the result and the input are different vector types, widening
      // the result could create a legal type but widening the input might make
      // it an illegal type that might lead to repeatedly splitting the input
      // and then widening it. To avoid this, we widen the input only if
      // it results in a legal type.
      SmallVector<SDValue, 16> Ops(NewNumElts);
      SDValue UndefVal = DAG.getUNDEF(InVT);
      Ops[0] = InOp;
      for (unsigned i = 1; i < NewNumElts; ++i)
        Ops[i] = UndefVal;

      SDValue NewVec;
      if (InVT.isVector())
        NewVec = DAG.getNode(ISD::CONCAT_VECTORS, dl,
                             NewInVT, &Ops[0], NewNumElts);
      else
        NewVec = DAG.getNode(ISD::BUILD_VECTOR, dl,
                             NewInVT, &Ops[0], NewNumElts);
      return DAG.getNode(ISD::BITCAST, dl, WidenVT, NewVec);
    }
  }

  return CreateStackStoreLoad(InOp, WidenVT);
}

SDValue DAGTypeLegalizer::WidenVecRes_BUILD_VECTOR(SDNode *N) {
  DebugLoc dl = N->getDebugLoc();
  // Build a vector with undefined for the new nodes.
  EVT VT = N->getValueType(0);
  EVT EltVT = VT.getVectorElementType();
  unsigned NumElts = VT.getVectorNumElements();

  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), VT);
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  SmallVector<SDValue, 16> NewOps(N->op_begin(), N->op_end());
  NewOps.reserve(WidenNumElts);
  for (unsigned i = NumElts; i < WidenNumElts; ++i)
    NewOps.push_back(DAG.getUNDEF(EltVT));

  return DAG.getNode(ISD::BUILD_VECTOR, dl, WidenVT, &NewOps[0], NewOps.size());
}

SDValue DAGTypeLegalizer::WidenVecRes_CONCAT_VECTORS(SDNode *N) {
  EVT InVT = N->getOperand(0).getValueType();
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  DebugLoc dl = N->getDebugLoc();
  unsigned WidenNumElts = WidenVT.getVectorNumElements();
  unsigned NumInElts = InVT.getVectorNumElements();
  unsigned NumOperands = N->getNumOperands();

  bool InputWidened = false; // Indicates we need to widen the input.
  if (getTypeAction(InVT) != TargetLowering::TypeWidenVector) {
    if (WidenVT.getVectorNumElements() % InVT.getVectorNumElements() == 0) {
      // Add undef vectors to widen to correct length.
      unsigned NumConcat = WidenVT.getVectorNumElements() /
                           InVT.getVectorNumElements();
      SDValue UndefVal = DAG.getUNDEF(InVT);
      SmallVector<SDValue, 16> Ops(NumConcat);
      for (unsigned i=0; i < NumOperands; ++i)
        Ops[i] = N->getOperand(i);
      for (unsigned i = NumOperands; i != NumConcat; ++i)
        Ops[i] = UndefVal;
      return DAG.getNode(ISD::CONCAT_VECTORS, dl, WidenVT, &Ops[0], NumConcat);
    }
  } else {
    InputWidened = true;
    if (WidenVT == TLI.getTypeToTransformTo(*DAG.getContext(), InVT)) {
      // The inputs and the result are widen to the same value.
      unsigned i;
      for (i=1; i < NumOperands; ++i)
        if (N->getOperand(i).getOpcode() != ISD::UNDEF)
          break;

      if (i == NumOperands)
        // Everything but the first operand is an UNDEF so just return the
        // widened first operand.
        return GetWidenedVector(N->getOperand(0));

      if (NumOperands == 2) {
        // Replace concat of two operands with a shuffle.
        SmallVector<int, 16> MaskOps(WidenNumElts, -1);
        for (unsigned i = 0; i < NumInElts; ++i) {
          MaskOps[i] = i;
          MaskOps[i + NumInElts] = i + WidenNumElts;
        }
        return DAG.getVectorShuffle(WidenVT, dl,
                                    GetWidenedVector(N->getOperand(0)),
                                    GetWidenedVector(N->getOperand(1)),
                                    &MaskOps[0]);
      }
    }
  }

  // Fall back to use extracts and build vector.
  EVT EltVT = WidenVT.getVectorElementType();
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  unsigned Idx = 0;
  for (unsigned i=0; i < NumOperands; ++i) {
    SDValue InOp = N->getOperand(i);
    if (InputWidened)
      InOp = GetWidenedVector(InOp);
    for (unsigned j=0; j < NumInElts; ++j)
        Ops[Idx++] = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT, InOp,
                                 DAG.getIntPtrConstant(j));
  }
  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for (; Idx < WidenNumElts; ++Idx)
    Ops[Idx] = UndefVal;
  return DAG.getNode(ISD::BUILD_VECTOR, dl, WidenVT, &Ops[0], WidenNumElts);
}

SDValue DAGTypeLegalizer::WidenVecRes_CONVERT_RNDSAT(SDNode *N) {
  DebugLoc dl = N->getDebugLoc();
  SDValue InOp  = N->getOperand(0);
  SDValue RndOp = N->getOperand(3);
  SDValue SatOp = N->getOperand(4);

  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  EVT InVT = InOp.getValueType();
  EVT InEltVT = InVT.getVectorElementType();
  EVT InWidenVT = EVT::getVectorVT(*DAG.getContext(), InEltVT, WidenNumElts);

  SDValue DTyOp = DAG.getValueType(WidenVT);
  SDValue STyOp = DAG.getValueType(InWidenVT);
  ISD::CvtCode CvtCode = cast<CvtRndSatSDNode>(N)->getCvtCode();

  unsigned InVTNumElts = InVT.getVectorNumElements();
  if (getTypeAction(InVT) == TargetLowering::TypeWidenVector) {
    InOp = GetWidenedVector(InOp);
    InVT = InOp.getValueType();
    InVTNumElts = InVT.getVectorNumElements();
    if (InVTNumElts == WidenNumElts)
      return DAG.getConvertRndSat(WidenVT, dl, InOp, DTyOp, STyOp, RndOp,
                                  SatOp, CvtCode);
  }

  if (TLI.isTypeLegal(InWidenVT)) {
    // Because the result and the input are different vector types, widening
    // the result could create a legal type but widening the input might make
    // it an illegal type that might lead to repeatedly splitting the input
    // and then widening it. To avoid this, we widen the input only if
    // it results in a legal type.
    if (WidenNumElts % InVTNumElts == 0) {
      // Widen the input and call convert on the widened input vector.
      unsigned NumConcat = WidenNumElts/InVTNumElts;
      SmallVector<SDValue, 16> Ops(NumConcat);
      Ops[0] = InOp;
      SDValue UndefVal = DAG.getUNDEF(InVT);
      for (unsigned i = 1; i != NumConcat; ++i)
        Ops[i] = UndefVal;

      InOp = DAG.getNode(ISD::CONCAT_VECTORS, dl, InWidenVT, &Ops[0],NumConcat);
      return DAG.getConvertRndSat(WidenVT, dl, InOp, DTyOp, STyOp, RndOp,
                                  SatOp, CvtCode);
    }

    if (InVTNumElts % WidenNumElts == 0) {
      // Extract the input and convert the shorten input vector.
      InOp = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, InWidenVT, InOp,
                         DAG.getIntPtrConstant(0));
      return DAG.getConvertRndSat(WidenVT, dl, InOp, DTyOp, STyOp, RndOp,
                                SatOp, CvtCode);
    }
  }

  // Otherwise unroll into some nasty scalar code and rebuild the vector.
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  EVT EltVT = WidenVT.getVectorElementType();
  DTyOp = DAG.getValueType(EltVT);
  STyOp = DAG.getValueType(InEltVT);

  unsigned MinElts = std::min(InVTNumElts, WidenNumElts);
  unsigned i;
  for (i=0; i < MinElts; ++i) {
    SDValue ExtVal = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, InEltVT, InOp,
                                 DAG.getIntPtrConstant(i));
    Ops[i] = DAG.getConvertRndSat(WidenVT, dl, ExtVal, DTyOp, STyOp, RndOp,
                                        SatOp, CvtCode);
  }

  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for (; i < WidenNumElts; ++i)
    Ops[i] = UndefVal;

  return DAG.getNode(ISD::BUILD_VECTOR, dl, WidenVT, &Ops[0], WidenNumElts);
}

SDValue DAGTypeLegalizer::WidenVecRes_EXTRACT_SUBVECTOR(SDNode *N) {
  EVT      VT = N->getValueType(0);
  EVT      WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), VT);
  unsigned WidenNumElts = WidenVT.getVectorNumElements();
  SDValue  InOp = N->getOperand(0);
  SDValue  Idx  = N->getOperand(1);
  DebugLoc dl = N->getDebugLoc();

  if (getTypeAction(InOp.getValueType()) == TargetLowering::TypeWidenVector)
    InOp = GetWidenedVector(InOp);

  EVT InVT = InOp.getValueType();

  // Check if we can just return the input vector after widening.
  uint64_t IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
  if (IdxVal == 0 && InVT == WidenVT)
    return InOp;

  // Check if we can extract from the vector.
  unsigned InNumElts = InVT.getVectorNumElements();
  if (IdxVal % WidenNumElts == 0 && IdxVal + WidenNumElts < InNumElts)
    return DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, WidenVT, InOp, Idx);

  // We could try widening the input to the right length but for now, extract
  // the original elements, fill the rest with undefs and build a vector.
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  EVT EltVT = VT.getVectorElementType();
  unsigned NumElts = VT.getVectorNumElements();
  unsigned i;
  for (i=0; i < NumElts; ++i)
    Ops[i] = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT, InOp,
                         DAG.getIntPtrConstant(IdxVal+i));

  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for (; i < WidenNumElts; ++i)
    Ops[i] = UndefVal;
  return DAG.getNode(ISD::BUILD_VECTOR, dl, WidenVT, &Ops[0], WidenNumElts);
}

SDValue DAGTypeLegalizer::WidenVecRes_INSERT_VECTOR_ELT(SDNode *N) {
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  return DAG.getNode(ISD::INSERT_VECTOR_ELT, N->getDebugLoc(),
                     InOp.getValueType(), InOp,
                     N->getOperand(1), N->getOperand(2));
}

SDValue DAGTypeLegalizer::WidenVecRes_LOAD(SDNode *N) {
  LoadSDNode *LD = cast<LoadSDNode>(N);
  ISD::LoadExtType ExtType = LD->getExtensionType();

  SDValue Result;
  SmallVector<SDValue, 16> LdChain;  // Chain for the series of load
  if (ExtType != ISD::NON_EXTLOAD)
    Result = GenWidenVectorExtLoads(LdChain, LD, ExtType);
  else
    Result = GenWidenVectorLoads(LdChain, LD);

  // If we generate a single load, we can use that for the chain.  Otherwise,
  // build a factor node to remember the multiple loads are independent and
  // chain to that.
  SDValue NewChain;
  if (LdChain.size() == 1)
    NewChain = LdChain[0];
  else
    NewChain = DAG.getNode(ISD::TokenFactor, LD->getDebugLoc(), MVT::Other,
                           &LdChain[0], LdChain.size());

  // Modified the chain - switch anything that used the old chain to use
  // the new one.
  ReplaceValueWith(SDValue(N, 1), NewChain);

  return Result;
}

SDValue DAGTypeLegalizer::WidenVecRes_SCALAR_TO_VECTOR(SDNode *N) {
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  return DAG.getNode(ISD::SCALAR_TO_VECTOR, N->getDebugLoc(),
                     WidenVT, N->getOperand(0));
}

SDValue DAGTypeLegalizer::WidenVecRes_SELECT(SDNode *N) {
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  SDValue Cond1 = N->getOperand(0);
  EVT CondVT = Cond1.getValueType();
  if (CondVT.isVector()) {
    EVT CondEltVT = CondVT.getVectorElementType();
    EVT CondWidenVT =  EVT::getVectorVT(*DAG.getContext(),
                                        CondEltVT, WidenNumElts);
    if (getTypeAction(CondVT) == TargetLowering::TypeWidenVector)
      Cond1 = GetWidenedVector(Cond1);

    if (Cond1.getValueType() != CondWidenVT)
       Cond1 = ModifyToType(Cond1, CondWidenVT);
  }

  SDValue InOp1 = GetWidenedVector(N->getOperand(1));
  SDValue InOp2 = GetWidenedVector(N->getOperand(2));
  assert(InOp1.getValueType() == WidenVT && InOp2.getValueType() == WidenVT);
  return DAG.getNode(N->getOpcode(), N->getDebugLoc(),
                     WidenVT, Cond1, InOp1, InOp2);
}

SDValue DAGTypeLegalizer::WidenVecRes_SELECT_CC(SDNode *N) {
  SDValue InOp1 = GetWidenedVector(N->getOperand(2));
  SDValue InOp2 = GetWidenedVector(N->getOperand(3));
  return DAG.getNode(ISD::SELECT_CC, N->getDebugLoc(),
                     InOp1.getValueType(), N->getOperand(0),
                     N->getOperand(1), InOp1, InOp2, N->getOperand(4));
}

SDValue DAGTypeLegalizer::WidenVecRes_SETCC(SDNode *N) {
  assert(N->getValueType(0).isVector() ==
         N->getOperand(0).getValueType().isVector() &&
         "Scalar/Vector type mismatch");
  if (N->getValueType(0).isVector()) return WidenVecRes_VSETCC(N);

  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  SDValue InOp1 = GetWidenedVector(N->getOperand(0));
  SDValue InOp2 = GetWidenedVector(N->getOperand(1));
  return DAG.getNode(ISD::SETCC, N->getDebugLoc(), WidenVT,
                     InOp1, InOp2, N->getOperand(2));
}

SDValue DAGTypeLegalizer::WidenVecRes_UNDEF(SDNode *N) {
 EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
 return DAG.getUNDEF(WidenVT);
}

SDValue DAGTypeLegalizer::WidenVecRes_VECTOR_SHUFFLE(ShuffleVectorSDNode *N) {
  EVT VT = N->getValueType(0);
  DebugLoc dl = N->getDebugLoc();

  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), VT);
  unsigned NumElts = VT.getVectorNumElements();
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  SDValue InOp1 = GetWidenedVector(N->getOperand(0));
  SDValue InOp2 = GetWidenedVector(N->getOperand(1));

  // Adjust mask based on new input vector length.
  SmallVector<int, 16> NewMask;
  for (unsigned i = 0; i != NumElts; ++i) {
    int Idx = N->getMaskElt(i);
    if (Idx < (int)NumElts)
      NewMask.push_back(Idx);
    else
      NewMask.push_back(Idx - NumElts + WidenNumElts);
  }
  for (unsigned i = NumElts; i != WidenNumElts; ++i)
    NewMask.push_back(-1);
  return DAG.getVectorShuffle(WidenVT, dl, InOp1, InOp2, &NewMask[0]);
}

SDValue DAGTypeLegalizer::WidenVecRes_VSETCC(SDNode *N) {
  assert(N->getValueType(0).isVector() &&
         N->getOperand(0).getValueType().isVector() &&
         "Operands must be vectors");
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(), N->getValueType(0));
  unsigned WidenNumElts = WidenVT.getVectorNumElements();

  SDValue InOp1 = N->getOperand(0);
  EVT InVT = InOp1.getValueType();
  assert(InVT.isVector() && "can not widen non vector type");
  EVT WidenInVT = EVT::getVectorVT(*DAG.getContext(),
                                   InVT.getVectorElementType(), WidenNumElts);
  InOp1 = GetWidenedVector(InOp1);
  SDValue InOp2 = GetWidenedVector(N->getOperand(1));

  // Assume that the input and output will be widen appropriately.  If not,
  // we will have to unroll it at some point.
  assert(InOp1.getValueType() == WidenInVT &&
         InOp2.getValueType() == WidenInVT &&
         "Input not widened to expected type!");
  (void)WidenInVT;
  return DAG.getNode(ISD::SETCC, N->getDebugLoc(),
                     WidenVT, InOp1, InOp2, N->getOperand(2));
}


//===----------------------------------------------------------------------===//
// Widen Vector Operand
//===----------------------------------------------------------------------===//
bool DAGTypeLegalizer::WidenVectorOperand(SDNode *N, unsigned ResNo) {
  DEBUG(dbgs() << "Widen node operand " << ResNo << ": ";
        N->dump(&DAG);
        dbgs() << "\n");
  SDValue Res = SDValue();

  switch (N->getOpcode()) {
  default:
#ifndef NDEBUG
    dbgs() << "WidenVectorOperand op #" << ResNo << ": ";
    N->dump(&DAG);
    dbgs() << "\n";
#endif
    llvm_unreachable("Do not know how to widen this operator's operand!");

  case ISD::BITCAST:            Res = WidenVecOp_BITCAST(N); break;
  case ISD::CONCAT_VECTORS:     Res = WidenVecOp_CONCAT_VECTORS(N); break;
  case ISD::EXTRACT_SUBVECTOR:  Res = WidenVecOp_EXTRACT_SUBVECTOR(N); break;
  case ISD::EXTRACT_VECTOR_ELT: Res = WidenVecOp_EXTRACT_VECTOR_ELT(N); break;
  case ISD::STORE:              Res = WidenVecOp_STORE(N); break;
  case ISD::SETCC:              Res = WidenVecOp_SETCC(N); break;

  case ISD::FP_EXTEND:
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP:
  case ISD::TRUNCATE:
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
  case ISD::ANY_EXTEND:
    Res = WidenVecOp_Convert(N);
    break;
  }

  // If Res is null, the sub-method took care of registering the result.
  if (!Res.getNode()) return false;

  // If the result is N, the sub-method updated N in place.  Tell the legalizer
  // core about this.
  if (Res.getNode() == N)
    return true;


  assert(Res.getValueType() == N->getValueType(0) && N->getNumValues() == 1 &&
         "Invalid operand expansion");

  ReplaceValueWith(SDValue(N, 0), Res);
  return false;
}

SDValue DAGTypeLegalizer::WidenVecOp_Convert(SDNode *N) {
  // Since the result is legal and the input is illegal, it is unlikely
  // that we can fix the input to a legal type so unroll the convert
  // into some scalar code and create a nasty build vector.
  EVT VT = N->getValueType(0);
  EVT EltVT = VT.getVectorElementType();
  DebugLoc dl = N->getDebugLoc();
  unsigned NumElts = VT.getVectorNumElements();
  SDValue InOp = N->getOperand(0);
  if (getTypeAction(InOp.getValueType()) == TargetLowering::TypeWidenVector)
    InOp = GetWidenedVector(InOp);
  EVT InVT = InOp.getValueType();
  EVT InEltVT = InVT.getVectorElementType();

  unsigned Opcode = N->getOpcode();
  SmallVector<SDValue, 16> Ops(NumElts);
  for (unsigned i=0; i < NumElts; ++i)
    Ops[i] = DAG.getNode(Opcode, dl, EltVT,
                         DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, InEltVT, InOp,
                                     DAG.getIntPtrConstant(i)));

  return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, &Ops[0], NumElts);
}

SDValue DAGTypeLegalizer::WidenVecOp_BITCAST(SDNode *N) {
  EVT VT = N->getValueType(0);
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  EVT InWidenVT = InOp.getValueType();
  DebugLoc dl = N->getDebugLoc();

  // Check if we can convert between two legal vector types and extract.
  unsigned InWidenSize = InWidenVT.getSizeInBits();
  unsigned Size = VT.getSizeInBits();
  // x86mmx is not an acceptable vector element type, so don't try.
  if (InWidenSize % Size == 0 && !VT.isVector() && VT != MVT::x86mmx) {
    unsigned NewNumElts = InWidenSize / Size;
    EVT NewVT = EVT::getVectorVT(*DAG.getContext(), VT, NewNumElts);
    if (TLI.isTypeLegal(NewVT)) {
      SDValue BitOp = DAG.getNode(ISD::BITCAST, dl, NewVT, InOp);
      return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, VT, BitOp,
                         DAG.getIntPtrConstant(0));
    }
  }

  return CreateStackStoreLoad(InOp, VT);
}

SDValue DAGTypeLegalizer::WidenVecOp_CONCAT_VECTORS(SDNode *N) {
  // If the input vector is not legal, it is likely that we will not find a
  // legal vector of the same size. Replace the concatenate vector with a
  // nasty build vector.
  EVT VT = N->getValueType(0);
  EVT EltVT = VT.getVectorElementType();
  DebugLoc dl = N->getDebugLoc();
  unsigned NumElts = VT.getVectorNumElements();
  SmallVector<SDValue, 16> Ops(NumElts);

  EVT InVT = N->getOperand(0).getValueType();
  unsigned NumInElts = InVT.getVectorNumElements();

  unsigned Idx = 0;
  unsigned NumOperands = N->getNumOperands();
  for (unsigned i=0; i < NumOperands; ++i) {
    SDValue InOp = N->getOperand(i);
    if (getTypeAction(InOp.getValueType()) == TargetLowering::TypeWidenVector)
      InOp = GetWidenedVector(InOp);
    for (unsigned j=0; j < NumInElts; ++j)
      Ops[Idx++] = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT, InOp,
                               DAG.getIntPtrConstant(j));
  }
  return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, &Ops[0], NumElts);
}

SDValue DAGTypeLegalizer::WidenVecOp_EXTRACT_SUBVECTOR(SDNode *N) {
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  return DAG.getNode(ISD::EXTRACT_SUBVECTOR, N->getDebugLoc(),
                     N->getValueType(0), InOp, N->getOperand(1));
}

SDValue DAGTypeLegalizer::WidenVecOp_EXTRACT_VECTOR_ELT(SDNode *N) {
  SDValue InOp = GetWidenedVector(N->getOperand(0));
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, N->getDebugLoc(),
                     N->getValueType(0), InOp, N->getOperand(1));
}

SDValue DAGTypeLegalizer::WidenVecOp_STORE(SDNode *N) {
  // We have to widen the value but we want only to store the original
  // vector type.
  StoreSDNode *ST = cast<StoreSDNode>(N);

  SmallVector<SDValue, 16> StChain;
  if (ST->isTruncatingStore())
    GenWidenVectorTruncStores(StChain, ST);
  else
    GenWidenVectorStores(StChain, ST);

  if (StChain.size() == 1)
    return StChain[0];
  else
    return DAG.getNode(ISD::TokenFactor, ST->getDebugLoc(),
                       MVT::Other,&StChain[0],StChain.size());
}

SDValue DAGTypeLegalizer::WidenVecOp_SETCC(SDNode *N) {
  SDValue InOp0 = GetWidenedVector(N->getOperand(0));
  SDValue InOp1 = GetWidenedVector(N->getOperand(1));
  DebugLoc dl = N->getDebugLoc();

  // WARNING: In this code we widen the compare instruction with garbage.
  // This garbage may contain denormal floats which may be slow. Is this a real
  // concern ? Should we zero the unused lanes if this is a float compare ?

  // Get a new SETCC node to compare the newly widened operands.
  // Only some of the compared elements are legal.
  EVT SVT = TLI.getSetCCResultType(InOp0.getValueType());
  SDValue WideSETCC = DAG.getNode(ISD::SETCC, N->getDebugLoc(),
                     SVT, InOp0, InOp1, N->getOperand(2));

  // Extract the needed results from the result vector.
  EVT ResVT = EVT::getVectorVT(*DAG.getContext(),
                               SVT.getVectorElementType(),
                               N->getValueType(0).getVectorNumElements());
  SDValue CC = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl,
                           ResVT, WideSETCC, DAG.getIntPtrConstant(0));

  return PromoteTargetBoolean(CC, N->getValueType(0)); 
}


//===----------------------------------------------------------------------===//
// Vector Widening Utilities
//===----------------------------------------------------------------------===//

// Utility function to find the type to chop up a widen vector for load/store
//  TLI:       Target lowering used to determine legal types.
//  Width:     Width left need to load/store.
//  WidenVT:   The widen vector type to load to/store from
//  Align:     If 0, don't allow use of a wider type
//  WidenEx:   If Align is not 0, the amount additional we can load/store from.

static EVT FindMemType(SelectionDAG& DAG, const TargetLowering &TLI,
                       unsigned Width, EVT WidenVT,
                       unsigned Align = 0, unsigned WidenEx = 0) {
  EVT WidenEltVT = WidenVT.getVectorElementType();
  unsigned WidenWidth = WidenVT.getSizeInBits();
  unsigned WidenEltWidth = WidenEltVT.getSizeInBits();
  unsigned AlignInBits = Align*8;

  // If we have one element to load/store, return it.
  EVT RetVT = WidenEltVT;
  if (Width == WidenEltWidth)
    return RetVT;

  // See if there is larger legal integer than the element type to load/store
  unsigned VT;
  for (VT = (unsigned)MVT::LAST_INTEGER_VALUETYPE;
       VT >= (unsigned)MVT::FIRST_INTEGER_VALUETYPE; --VT) {
    EVT MemVT((MVT::SimpleValueType) VT);
    unsigned MemVTWidth = MemVT.getSizeInBits();
    if (MemVT.getSizeInBits() <= WidenEltWidth)
      break;
    if (TLI.isTypeLegal(MemVT) && (WidenWidth % MemVTWidth) == 0 &&
        isPowerOf2_32(WidenWidth / MemVTWidth) &&
        (MemVTWidth <= Width ||
         (Align!=0 && MemVTWidth<=AlignInBits && MemVTWidth<=Width+WidenEx))) {
      RetVT = MemVT;
      break;
    }
  }

  // See if there is a larger vector type to load/store that has the same vector
  // element type and is evenly divisible with the WidenVT.
  for (VT = (unsigned)MVT::LAST_VECTOR_VALUETYPE;
       VT >= (unsigned)MVT::FIRST_VECTOR_VALUETYPE; --VT) {
    EVT MemVT = (MVT::SimpleValueType) VT;
    unsigned MemVTWidth = MemVT.getSizeInBits();
    if (TLI.isTypeLegal(MemVT) && WidenEltVT == MemVT.getVectorElementType() &&
        (WidenWidth % MemVTWidth) == 0 &&
        isPowerOf2_32(WidenWidth / MemVTWidth) &&
        (MemVTWidth <= Width ||
         (Align!=0 && MemVTWidth<=AlignInBits && MemVTWidth<=Width+WidenEx))) {
      if (RetVT.getSizeInBits() < MemVTWidth || MemVT == WidenVT)
        return MemVT;
    }
  }

  return RetVT;
}

// Builds a vector type from scalar loads
//  VecTy: Resulting Vector type
//  LDOps: Load operators to build a vector type
//  [Start,End) the list of loads to use.
static SDValue BuildVectorFromScalar(SelectionDAG& DAG, EVT VecTy,
                                     SmallVector<SDValue, 16>& LdOps,
                                     unsigned Start, unsigned End) {
  DebugLoc dl = LdOps[Start].getDebugLoc();
  EVT LdTy = LdOps[Start].getValueType();
  unsigned Width = VecTy.getSizeInBits();
  unsigned NumElts = Width / LdTy.getSizeInBits();
  EVT NewVecVT = EVT::getVectorVT(*DAG.getContext(), LdTy, NumElts);

  unsigned Idx = 1;
  SDValue VecOp = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, NewVecVT,LdOps[Start]);

  for (unsigned i = Start + 1; i != End; ++i) {
    EVT NewLdTy = LdOps[i].getValueType();
    if (NewLdTy != LdTy) {
      NumElts = Width / NewLdTy.getSizeInBits();
      NewVecVT = EVT::getVectorVT(*DAG.getContext(), NewLdTy, NumElts);
      VecOp = DAG.getNode(ISD::BITCAST, dl, NewVecVT, VecOp);
      // Readjust position and vector position based on new load type
      Idx = Idx * LdTy.getSizeInBits() / NewLdTy.getSizeInBits();
      LdTy = NewLdTy;
    }
    VecOp = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, NewVecVT, VecOp, LdOps[i],
                        DAG.getIntPtrConstant(Idx++));
  }
  return DAG.getNode(ISD::BITCAST, dl, VecTy, VecOp);
}

SDValue DAGTypeLegalizer::GenWidenVectorLoads(SmallVector<SDValue, 16> &LdChain,
                                              LoadSDNode *LD) {
  // The strategy assumes that we can efficiently load powers of two widths.
  // The routines chops the vector into the largest vector loads with the same
  // element type or scalar loads and then recombines it to the widen vector
  // type.
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(),LD->getValueType(0));
  unsigned WidenWidth = WidenVT.getSizeInBits();
  EVT LdVT    = LD->getMemoryVT();
  DebugLoc dl = LD->getDebugLoc();
  assert(LdVT.isVector() && WidenVT.isVector());
  assert(LdVT.getVectorElementType() == WidenVT.getVectorElementType());

  // Load information
  SDValue   Chain = LD->getChain();
  SDValue   BasePtr = LD->getBasePtr();
  unsigned  Align    = LD->getAlignment();
  bool      isVolatile = LD->isVolatile();
  bool      isNonTemporal = LD->isNonTemporal();
  bool      isInvariant = LD->isInvariant();

  int LdWidth = LdVT.getSizeInBits();
  int WidthDiff = WidenWidth - LdWidth;          // Difference
  unsigned LdAlign = (isVolatile) ? 0 : Align; // Allow wider loads

  // Find the vector type that can load from.
  EVT NewVT = FindMemType(DAG, TLI, LdWidth, WidenVT, LdAlign, WidthDiff);
  int NewVTWidth = NewVT.getSizeInBits();
  SDValue LdOp = DAG.getLoad(NewVT, dl, Chain, BasePtr, LD->getPointerInfo(),
                             isVolatile, isNonTemporal, isInvariant, Align);
  LdChain.push_back(LdOp.getValue(1));

  // Check if we can load the element with one instruction
  if (LdWidth <= NewVTWidth) {
    if (!NewVT.isVector()) {
      unsigned NumElts = WidenWidth / NewVTWidth;
      EVT NewVecVT = EVT::getVectorVT(*DAG.getContext(), NewVT, NumElts);
      SDValue VecOp = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, NewVecVT, LdOp);
      return DAG.getNode(ISD::BITCAST, dl, WidenVT, VecOp);
    }
    if (NewVT == WidenVT)
      return LdOp;

    assert(WidenWidth % NewVTWidth == 0);
    unsigned NumConcat = WidenWidth / NewVTWidth;
    SmallVector<SDValue, 16> ConcatOps(NumConcat);
    SDValue UndefVal = DAG.getUNDEF(NewVT);
    ConcatOps[0] = LdOp;
    for (unsigned i = 1; i != NumConcat; ++i)
      ConcatOps[i] = UndefVal;
    return DAG.getNode(ISD::CONCAT_VECTORS, dl, WidenVT, &ConcatOps[0],
                       NumConcat);
  }

  // Load vector by using multiple loads from largest vector to scalar
  SmallVector<SDValue, 16> LdOps;
  LdOps.push_back(LdOp);

  LdWidth -= NewVTWidth;
  unsigned Offset = 0;

  while (LdWidth > 0) {
    unsigned Increment = NewVTWidth / 8;
    Offset += Increment;
    BasePtr = DAG.getNode(ISD::ADD, dl, BasePtr.getValueType(), BasePtr,
                          DAG.getIntPtrConstant(Increment));

    if (LdWidth < NewVTWidth) {
      // Our current type we are using is too large, find a better size
      NewVT = FindMemType(DAG, TLI, LdWidth, WidenVT, LdAlign, WidthDiff);
      NewVTWidth = NewVT.getSizeInBits();
    }

    SDValue LdOp = DAG.getLoad(NewVT, dl, Chain, BasePtr,
                               LD->getPointerInfo().getWithOffset(Offset),
                               isVolatile,
                               isNonTemporal, isInvariant,
                               MinAlign(Align, Increment));
    LdChain.push_back(LdOp.getValue(1));
    LdOps.push_back(LdOp);

    LdWidth -= NewVTWidth;
  }

  // Build the vector from the loads operations
  unsigned End = LdOps.size();
  if (!LdOps[0].getValueType().isVector())
    // All the loads are scalar loads.
    return BuildVectorFromScalar(DAG, WidenVT, LdOps, 0, End);

  // If the load contains vectors, build the vector using concat vector.
  // All of the vectors used to loads are power of 2 and the scalars load
  // can be combined to make a power of 2 vector.
  SmallVector<SDValue, 16> ConcatOps(End);
  int i = End - 1;
  int Idx = End;
  EVT LdTy = LdOps[i].getValueType();
  // First combine the scalar loads to a vector
  if (!LdTy.isVector())  {
    for (--i; i >= 0; --i) {
      LdTy = LdOps[i].getValueType();
      if (LdTy.isVector())
        break;
    }
    ConcatOps[--Idx] = BuildVectorFromScalar(DAG, LdTy, LdOps, i+1, End);
  }
  ConcatOps[--Idx] = LdOps[i];
  for (--i; i >= 0; --i) {
    EVT NewLdTy = LdOps[i].getValueType();
    if (NewLdTy != LdTy) {
      // Create a larger vector
      ConcatOps[End-1] = DAG.getNode(ISD::CONCAT_VECTORS, dl, NewLdTy,
                                     &ConcatOps[Idx], End - Idx);
      Idx = End - 1;
      LdTy = NewLdTy;
    }
    ConcatOps[--Idx] = LdOps[i];
  }

  if (WidenWidth == LdTy.getSizeInBits()*(End - Idx))
    return DAG.getNode(ISD::CONCAT_VECTORS, dl, WidenVT,
                       &ConcatOps[Idx], End - Idx);

  // We need to fill the rest with undefs to build the vector
  unsigned NumOps = WidenWidth / LdTy.getSizeInBits();
  SmallVector<SDValue, 16> WidenOps(NumOps);
  SDValue UndefVal = DAG.getUNDEF(LdTy);
  {
    unsigned i = 0;
    for (; i != End-Idx; ++i)
      WidenOps[i] = ConcatOps[Idx+i];
    for (; i != NumOps; ++i)
      WidenOps[i] = UndefVal;
  }
  return DAG.getNode(ISD::CONCAT_VECTORS, dl, WidenVT, &WidenOps[0],NumOps);
}

SDValue
DAGTypeLegalizer::GenWidenVectorExtLoads(SmallVector<SDValue, 16>& LdChain,
                                         LoadSDNode * LD,
                                         ISD::LoadExtType ExtType) {
  // For extension loads, it may not be more efficient to chop up the vector
  // and then extended it.  Instead, we unroll the load and build a new vector.
  EVT WidenVT = TLI.getTypeToTransformTo(*DAG.getContext(),LD->getValueType(0));
  EVT LdVT    = LD->getMemoryVT();
  DebugLoc dl = LD->getDebugLoc();
  assert(LdVT.isVector() && WidenVT.isVector());

  // Load information
  SDValue   Chain = LD->getChain();
  SDValue   BasePtr = LD->getBasePtr();
  unsigned  Align    = LD->getAlignment();
  bool      isVolatile = LD->isVolatile();
  bool      isNonTemporal = LD->isNonTemporal();

  EVT EltVT = WidenVT.getVectorElementType();
  EVT LdEltVT = LdVT.getVectorElementType();
  unsigned NumElts = LdVT.getVectorNumElements();

  // Load each element and widen
  unsigned WidenNumElts = WidenVT.getVectorNumElements();
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  unsigned Increment = LdEltVT.getSizeInBits() / 8;
  Ops[0] = DAG.getExtLoad(ExtType, dl, EltVT, Chain, BasePtr,
                          LD->getPointerInfo(),
                          LdEltVT, isVolatile, isNonTemporal, Align);
  LdChain.push_back(Ops[0].getValue(1));
  unsigned i = 0, Offset = Increment;
  for (i=1; i < NumElts; ++i, Offset += Increment) {
    SDValue NewBasePtr = DAG.getNode(ISD::ADD, dl, BasePtr.getValueType(),
                                     BasePtr, DAG.getIntPtrConstant(Offset));
    Ops[i] = DAG.getExtLoad(ExtType, dl, EltVT, Chain, NewBasePtr,
                            LD->getPointerInfo().getWithOffset(Offset), LdEltVT,
                            isVolatile, isNonTemporal, Align);
    LdChain.push_back(Ops[i].getValue(1));
  }

  // Fill the rest with undefs
  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for (; i != WidenNumElts; ++i)
    Ops[i] = UndefVal;

  return DAG.getNode(ISD::BUILD_VECTOR, dl, WidenVT, &Ops[0], Ops.size());
}


void DAGTypeLegalizer::GenWidenVectorStores(SmallVector<SDValue, 16>& StChain,
                                            StoreSDNode *ST) {
  // The strategy assumes that we can efficiently store powers of two widths.
  // The routines chops the vector into the largest vector stores with the same
  // element type or scalar stores.
  SDValue  Chain = ST->getChain();
  SDValue  BasePtr = ST->getBasePtr();
  unsigned Align = ST->getAlignment();
  bool     isVolatile = ST->isVolatile();
  bool     isNonTemporal = ST->isNonTemporal();
  SDValue  ValOp = GetWidenedVector(ST->getValue());
  DebugLoc dl = ST->getDebugLoc();

  EVT StVT = ST->getMemoryVT();
  unsigned StWidth = StVT.getSizeInBits();
  EVT ValVT = ValOp.getValueType();
  unsigned ValWidth = ValVT.getSizeInBits();
  EVT ValEltVT = ValVT.getVectorElementType();
  unsigned ValEltWidth = ValEltVT.getSizeInBits();
  assert(StVT.getVectorElementType() == ValEltVT);

  int Idx = 0;          // current index to store
  unsigned Offset = 0;  // offset from base to store
  while (StWidth != 0) {
    // Find the largest vector type we can store with
    EVT NewVT = FindMemType(DAG, TLI, StWidth, ValVT);
    unsigned NewVTWidth = NewVT.getSizeInBits();
    unsigned Increment = NewVTWidth / 8;
    if (NewVT.isVector()) {
      unsigned NumVTElts = NewVT.getVectorNumElements();
      do {
        SDValue EOp = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, NewVT, ValOp,
                                   DAG.getIntPtrConstant(Idx));
        StChain.push_back(DAG.getStore(Chain, dl, EOp, BasePtr,
                                    ST->getPointerInfo().getWithOffset(Offset),
                                       isVolatile, isNonTemporal,
                                       MinAlign(Align, Offset)));
        StWidth -= NewVTWidth;
        Offset += Increment;
        Idx += NumVTElts;
        BasePtr = DAG.getNode(ISD::ADD, dl, BasePtr.getValueType(), BasePtr,
                              DAG.getIntPtrConstant(Increment));
      } while (StWidth != 0 && StWidth >= NewVTWidth);
    } else {
      // Cast the vector to the scalar type we can store
      unsigned NumElts = ValWidth / NewVTWidth;
      EVT NewVecVT = EVT::getVectorVT(*DAG.getContext(), NewVT, NumElts);
      SDValue VecOp = DAG.getNode(ISD::BITCAST, dl, NewVecVT, ValOp);
      // Readjust index position based on new vector type
      Idx = Idx * ValEltWidth / NewVTWidth;
      do {
        SDValue EOp = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, NewVT, VecOp,
                      DAG.getIntPtrConstant(Idx++));
        StChain.push_back(DAG.getStore(Chain, dl, EOp, BasePtr,
                                    ST->getPointerInfo().getWithOffset(Offset),
                                       isVolatile, isNonTemporal,
                                       MinAlign(Align, Offset)));
        StWidth -= NewVTWidth;
        Offset += Increment;
        BasePtr = DAG.getNode(ISD::ADD, dl, BasePtr.getValueType(), BasePtr,
                              DAG.getIntPtrConstant(Increment));
      } while (StWidth != 0  && StWidth >= NewVTWidth);
      // Restore index back to be relative to the original widen element type
      Idx = Idx * NewVTWidth / ValEltWidth;
    }
  }
}

void
DAGTypeLegalizer::GenWidenVectorTruncStores(SmallVector<SDValue, 16>& StChain,
                                            StoreSDNode *ST) {
  // For extension loads, it may not be more efficient to truncate the vector
  // and then store it.  Instead, we extract each element and then store it.
  SDValue  Chain = ST->getChain();
  SDValue  BasePtr = ST->getBasePtr();
  unsigned Align = ST->getAlignment();
  bool     isVolatile = ST->isVolatile();
  bool     isNonTemporal = ST->isNonTemporal();
  SDValue  ValOp = GetWidenedVector(ST->getValue());
  DebugLoc dl = ST->getDebugLoc();

  EVT StVT = ST->getMemoryVT();
  EVT ValVT = ValOp.getValueType();

  // It must be true that we the widen vector type is bigger than where
  // we need to store.
  assert(StVT.isVector() && ValOp.getValueType().isVector());
  assert(StVT.bitsLT(ValOp.getValueType()));

  // For truncating stores, we can not play the tricks of chopping legal
  // vector types and bit cast it to the right type.  Instead, we unroll
  // the store.
  EVT StEltVT  = StVT.getVectorElementType();
  EVT ValEltVT = ValVT.getVectorElementType();
  unsigned Increment = ValEltVT.getSizeInBits() / 8;
  unsigned NumElts = StVT.getVectorNumElements();
  SDValue EOp = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, ValEltVT, ValOp,
                            DAG.getIntPtrConstant(0));
  StChain.push_back(DAG.getTruncStore(Chain, dl, EOp, BasePtr,
                                      ST->getPointerInfo(), StEltVT,
                                      isVolatile, isNonTemporal, Align));
  unsigned Offset = Increment;
  for (unsigned i=1; i < NumElts; ++i, Offset += Increment) {
    SDValue NewBasePtr = DAG.getNode(ISD::ADD, dl, BasePtr.getValueType(),
                                     BasePtr, DAG.getIntPtrConstant(Offset));
    SDValue EOp = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, ValEltVT, ValOp,
                            DAG.getIntPtrConstant(0));
    StChain.push_back(DAG.getTruncStore(Chain, dl, EOp, NewBasePtr,
                                      ST->getPointerInfo().getWithOffset(Offset),
                                        StEltVT, isVolatile, isNonTemporal,
                                        MinAlign(Align, Offset)));
  }
}

/// Modifies a vector input (widen or narrows) to a vector of NVT.  The
/// input vector must have the same element type as NVT.
SDValue DAGTypeLegalizer::ModifyToType(SDValue InOp, EVT NVT) {
  // Note that InOp might have been widened so it might already have
  // the right width or it might need be narrowed.
  EVT InVT = InOp.getValueType();
  assert(InVT.getVectorElementType() == NVT.getVectorElementType() &&
         "input and widen element type must match");
  DebugLoc dl = InOp.getDebugLoc();

  // Check if InOp already has the right width.
  if (InVT == NVT)
    return InOp;

  unsigned InNumElts = InVT.getVectorNumElements();
  unsigned WidenNumElts = NVT.getVectorNumElements();
  if (WidenNumElts > InNumElts && WidenNumElts % InNumElts == 0) {
    unsigned NumConcat = WidenNumElts / InNumElts;
    SmallVector<SDValue, 16> Ops(NumConcat);
    SDValue UndefVal = DAG.getUNDEF(InVT);
    Ops[0] = InOp;
    for (unsigned i = 1; i != NumConcat; ++i)
      Ops[i] = UndefVal;

    return DAG.getNode(ISD::CONCAT_VECTORS, dl, NVT, &Ops[0], NumConcat);
  }

  if (WidenNumElts < InNumElts && InNumElts % WidenNumElts)
    return DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, NVT, InOp,
                       DAG.getIntPtrConstant(0));

  // Fall back to extract and build.
  SmallVector<SDValue, 16> Ops(WidenNumElts);
  EVT EltVT = NVT.getVectorElementType();
  unsigned MinNumElts = std::min(WidenNumElts, InNumElts);
  unsigned Idx;
  for (Idx = 0; Idx < MinNumElts; ++Idx)
    Ops[Idx] = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT, InOp,
                           DAG.getIntPtrConstant(Idx));

  SDValue UndefVal = DAG.getUNDEF(EltVT);
  for ( ; Idx < WidenNumElts; ++Idx)
    Ops[Idx] = UndefVal;
  return DAG.getNode(ISD::BUILD_VECTOR, dl, NVT, &Ops[0], WidenNumElts);
}
