//====- LowerToLLVM.cpp - Lowering from CIR to LLVMIR ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements lowering of CIR operations to LLVMIR.
//
//===----------------------------------------------------------------------===//

#include "LowerToLLVM.h"

#include <deque>
#include <optional>

#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Types.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/DialectConversion.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/Passes.h"
#include "clang/CIR/LoweringHelpers.h"
#include "clang/CIR/MissingFeatures.h"
#include "clang/CIR/Passes.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TimeProfiler.h"

using namespace cir;
using namespace llvm;

namespace cir {
namespace direct {

//===----------------------------------------------------------------------===//
// Helper Methods
//===----------------------------------------------------------------------===//

namespace {
/// If the given type is a vector type, return the vector's element type.
/// Otherwise return the given type unchanged.
mlir::Type elementTypeIfVector(mlir::Type type) {
  return llvm::TypeSwitch<mlir::Type, mlir::Type>(type)
      .Case<cir::VectorType, mlir::VectorType>(
          [](auto p) { return p.getElementType(); })
      .Default([](mlir::Type p) { return p; });
}
} // namespace

/// Given a type convertor and a data layout, convert the given type to a type
/// that is suitable for memory operations. For example, this can be used to
/// lower cir.bool accesses to i8.
static mlir::Type convertTypeForMemory(const mlir::TypeConverter &converter,
                                       mlir::DataLayout const &dataLayout,
                                       mlir::Type type) {
  // TODO(cir): Handle other types similarly to clang's codegen
  // convertTypeForMemory
  if (isa<cir::BoolType>(type)) {
    return mlir::IntegerType::get(type.getContext(),
                                  dataLayout.getTypeSizeInBits(type));
  }

  return converter.convertType(type);
}

static mlir::Value createIntCast(mlir::OpBuilder &bld, mlir::Value src,
                                 mlir::IntegerType dstTy,
                                 bool isSigned = false) {
  mlir::Type srcTy = src.getType();
  assert(mlir::isa<mlir::IntegerType>(srcTy));

  unsigned srcWidth = mlir::cast<mlir::IntegerType>(srcTy).getWidth();
  unsigned dstWidth = mlir::cast<mlir::IntegerType>(dstTy).getWidth();
  mlir::Location loc = src.getLoc();

  if (dstWidth > srcWidth && isSigned)
    return bld.create<mlir::LLVM::SExtOp>(loc, dstTy, src);
  if (dstWidth > srcWidth)
    return bld.create<mlir::LLVM::ZExtOp>(loc, dstTy, src);
  if (dstWidth < srcWidth)
    return bld.create<mlir::LLVM::TruncOp>(loc, dstTy, src);
  return bld.create<mlir::LLVM::BitcastOp>(loc, dstTy, src);
}

static mlir::LLVM::Visibility
lowerCIRVisibilityToLLVMVisibility(cir::VisibilityKind visibilityKind) {
  switch (visibilityKind) {
  case cir::VisibilityKind::Default:
    return ::mlir::LLVM::Visibility::Default;
  case cir::VisibilityKind::Hidden:
    return ::mlir::LLVM::Visibility::Hidden;
  case cir::VisibilityKind::Protected:
    return ::mlir::LLVM::Visibility::Protected;
  }
}

/// Emits the value from memory as expected by its users. Should be called when
/// the memory represetnation of a CIR type is not equal to its scalar
/// representation.
static mlir::Value emitFromMemory(mlir::ConversionPatternRewriter &rewriter,
                                  mlir::DataLayout const &dataLayout,
                                  cir::LoadOp op, mlir::Value value) {

  // TODO(cir): Handle other types similarly to clang's codegen EmitFromMemory
  if (auto boolTy = mlir::dyn_cast<cir::BoolType>(op.getType())) {
    // Create a cast value from specified size in datalayout to i1
    assert(value.getType().isInteger(dataLayout.getTypeSizeInBits(boolTy)));
    return createIntCast(rewriter, value, rewriter.getI1Type());
  }

  return value;
}

/// Emits a value to memory with the expected scalar type. Should be called when
/// the memory represetnation of a CIR type is not equal to its scalar
/// representation.
static mlir::Value emitToMemory(mlir::ConversionPatternRewriter &rewriter,
                                mlir::DataLayout const &dataLayout,
                                mlir::Type origType, mlir::Value value) {

  // TODO(cir): Handle other types similarly to clang's codegen EmitToMemory
  if (auto boolTy = mlir::dyn_cast<cir::BoolType>(origType)) {
    // Create zext of value from i1 to i8
    mlir::IntegerType memType =
        rewriter.getIntegerType(dataLayout.getTypeSizeInBits(boolTy));
    return createIntCast(rewriter, value, memType);
  }

  return value;
}

mlir::LLVM::Linkage convertLinkage(cir::GlobalLinkageKind linkage) {
  using CIR = cir::GlobalLinkageKind;
  using LLVM = mlir::LLVM::Linkage;

  switch (linkage) {
  case CIR::AvailableExternallyLinkage:
    return LLVM::AvailableExternally;
  case CIR::CommonLinkage:
    return LLVM::Common;
  case CIR::ExternalLinkage:
    return LLVM::External;
  case CIR::ExternalWeakLinkage:
    return LLVM::ExternWeak;
  case CIR::InternalLinkage:
    return LLVM::Internal;
  case CIR::LinkOnceAnyLinkage:
    return LLVM::Linkonce;
  case CIR::LinkOnceODRLinkage:
    return LLVM::LinkonceODR;
  case CIR::PrivateLinkage:
    return LLVM::Private;
  case CIR::WeakAnyLinkage:
    return LLVM::Weak;
  case CIR::WeakODRLinkage:
    return LLVM::WeakODR;
  };
  llvm_unreachable("Unknown CIR linkage type");
}

static mlir::Value getLLVMIntCast(mlir::ConversionPatternRewriter &rewriter,
                                  mlir::Value llvmSrc, mlir::Type llvmDstIntTy,
                                  bool isUnsigned, uint64_t cirSrcWidth,
                                  uint64_t cirDstIntWidth) {
  if (cirSrcWidth == cirDstIntWidth)
    return llvmSrc;

  auto loc = llvmSrc.getLoc();
  if (cirSrcWidth < cirDstIntWidth) {
    if (isUnsigned)
      return rewriter.create<mlir::LLVM::ZExtOp>(loc, llvmDstIntTy, llvmSrc);
    return rewriter.create<mlir::LLVM::SExtOp>(loc, llvmDstIntTy, llvmSrc);
  }

  // Otherwise truncate
  return rewriter.create<mlir::LLVM::TruncOp>(loc, llvmDstIntTy, llvmSrc);
}

class CIRAttrToValue {
public:
  CIRAttrToValue(mlir::Operation *parentOp,
                 mlir::ConversionPatternRewriter &rewriter,
                 const mlir::TypeConverter *converter)
      : parentOp(parentOp), rewriter(rewriter), converter(converter) {}

  mlir::Value visit(mlir::Attribute attr) {
    return llvm::TypeSwitch<mlir::Attribute, mlir::Value>(attr)
        .Case<cir::IntAttr, cir::FPAttr, cir::ConstComplexAttr,
              cir::ConstArrayAttr, cir::ConstVectorAttr, cir::ConstPtrAttr,
              cir::ZeroAttr>([&](auto attrT) { return visitCirAttr(attrT); })
        .Default([&](auto attrT) { return mlir::Value(); });
  }

  mlir::Value visitCirAttr(cir::IntAttr intAttr);
  mlir::Value visitCirAttr(cir::FPAttr fltAttr);
  mlir::Value visitCirAttr(cir::ConstComplexAttr complexAttr);
  mlir::Value visitCirAttr(cir::ConstPtrAttr ptrAttr);
  mlir::Value visitCirAttr(cir::ConstArrayAttr attr);
  mlir::Value visitCirAttr(cir::ConstVectorAttr attr);
  mlir::Value visitCirAttr(cir::ZeroAttr attr);

private:
  mlir::Operation *parentOp;
  mlir::ConversionPatternRewriter &rewriter;
  const mlir::TypeConverter *converter;
};

/// Switches on the type of attribute and calls the appropriate conversion.
mlir::Value lowerCirAttrAsValue(mlir::Operation *parentOp,
                                const mlir::Attribute attr,
                                mlir::ConversionPatternRewriter &rewriter,
                                const mlir::TypeConverter *converter) {
  CIRAttrToValue valueConverter(parentOp, rewriter, converter);
  mlir::Value value = valueConverter.visit(attr);
  if (!value)
    llvm_unreachable("unhandled attribute type");
  return value;
}

void convertSideEffectForCall(mlir::Operation *callOp, bool isNothrow,
                              cir::SideEffect sideEffect,
                              mlir::LLVM::MemoryEffectsAttr &memoryEffect,
                              bool &noUnwind, bool &willReturn) {
  using mlir::LLVM::ModRefInfo;

  switch (sideEffect) {
  case cir::SideEffect::All:
    memoryEffect = {};
    noUnwind = isNothrow;
    willReturn = false;
    break;

  case cir::SideEffect::Pure:
    memoryEffect = mlir::LLVM::MemoryEffectsAttr::get(
        callOp->getContext(), /*other=*/ModRefInfo::Ref,
        /*argMem=*/ModRefInfo::Ref,
        /*inaccessibleMem=*/ModRefInfo::Ref);
    noUnwind = true;
    willReturn = true;
    break;

  case cir::SideEffect::Const:
    memoryEffect = mlir::LLVM::MemoryEffectsAttr::get(
        callOp->getContext(), /*other=*/ModRefInfo::NoModRef,
        /*argMem=*/ModRefInfo::NoModRef,
        /*inaccessibleMem=*/ModRefInfo::NoModRef);
    noUnwind = true;
    willReturn = true;
    break;
  }
}

/// IntAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::IntAttr intAttr) {
  mlir::Location loc = parentOp->getLoc();
  return rewriter.create<mlir::LLVM::ConstantOp>(
      loc, converter->convertType(intAttr.getType()), intAttr.getValue());
}

/// FPAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::FPAttr fltAttr) {
  mlir::Location loc = parentOp->getLoc();
  return rewriter.create<mlir::LLVM::ConstantOp>(
      loc, converter->convertType(fltAttr.getType()), fltAttr.getValue());
}

/// ConstComplexAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::ConstComplexAttr complexAttr) {
  auto complexType = mlir::cast<cir::ComplexType>(complexAttr.getType());
  mlir::Type complexElemTy = complexType.getElementType();
  mlir::Type complexElemLLVMTy = converter->convertType(complexElemTy);

  mlir::Attribute components[2];
  if (const auto intType = mlir::dyn_cast<cir::IntType>(complexElemTy)) {
    components[0] = rewriter.getIntegerAttr(
        complexElemLLVMTy,
        mlir::cast<cir::IntAttr>(complexAttr.getReal()).getValue());
    components[1] = rewriter.getIntegerAttr(
        complexElemLLVMTy,
        mlir::cast<cir::IntAttr>(complexAttr.getImag()).getValue());
  } else {
    components[0] = rewriter.getFloatAttr(
        complexElemLLVMTy,
        mlir::cast<cir::FPAttr>(complexAttr.getReal()).getValue());
    components[1] = rewriter.getFloatAttr(
        complexElemLLVMTy,
        mlir::cast<cir::FPAttr>(complexAttr.getImag()).getValue());
  }

  mlir::Location loc = parentOp->getLoc();
  return rewriter.create<mlir::LLVM::ConstantOp>(
      loc, converter->convertType(complexAttr.getType()),
      rewriter.getArrayAttr(components));
}

/// ConstPtrAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::ConstPtrAttr ptrAttr) {
  mlir::Location loc = parentOp->getLoc();
  if (ptrAttr.isNullValue()) {
    return rewriter.create<mlir::LLVM::ZeroOp>(
        loc, converter->convertType(ptrAttr.getType()));
  }
  mlir::DataLayout layout(parentOp->getParentOfType<mlir::ModuleOp>());
  mlir::Value ptrVal = rewriter.create<mlir::LLVM::ConstantOp>(
      loc, rewriter.getIntegerType(layout.getTypeSizeInBits(ptrAttr.getType())),
      ptrAttr.getValue().getInt());
  return rewriter.create<mlir::LLVM::IntToPtrOp>(
      loc, converter->convertType(ptrAttr.getType()), ptrVal);
}

// ConstArrayAttr visitor
mlir::Value CIRAttrToValue::visitCirAttr(cir::ConstArrayAttr attr) {
  mlir::Type llvmTy = converter->convertType(attr.getType());
  mlir::Location loc = parentOp->getLoc();
  mlir::Value result;

  if (attr.hasTrailingZeros()) {
    mlir::Type arrayTy = attr.getType();
    result = rewriter.create<mlir::LLVM::ZeroOp>(
        loc, converter->convertType(arrayTy));
  } else {
    result = rewriter.create<mlir::LLVM::UndefOp>(loc, llvmTy);
  }

  // Iteratively lower each constant element of the array.
  if (auto arrayAttr = mlir::dyn_cast<mlir::ArrayAttr>(attr.getElts())) {
    for (auto [idx, elt] : llvm::enumerate(arrayAttr)) {
      mlir::DataLayout dataLayout(parentOp->getParentOfType<mlir::ModuleOp>());
      mlir::Value init = visit(elt);
      result =
          rewriter.create<mlir::LLVM::InsertValueOp>(loc, result, init, idx);
    }
  } else if (auto strAttr = mlir::dyn_cast<mlir::StringAttr>(attr.getElts())) {
    // TODO(cir): this diverges from traditional lowering. Normally the string
    // would be a global constant that is memcopied.
    auto arrayTy = mlir::dyn_cast<cir::ArrayType>(strAttr.getType());
    assert(arrayTy && "String attribute must have an array type");
    mlir::Type eltTy = arrayTy.getElementType();
    for (auto [idx, elt] : llvm::enumerate(strAttr)) {
      auto init = rewriter.create<mlir::LLVM::ConstantOp>(
          loc, converter->convertType(eltTy), elt);
      result =
          rewriter.create<mlir::LLVM::InsertValueOp>(loc, result, init, idx);
    }
  } else {
    llvm_unreachable("unexpected ConstArrayAttr elements");
  }

  return result;
}

/// ConstVectorAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::ConstVectorAttr attr) {
  const mlir::Type llvmTy = converter->convertType(attr.getType());
  const mlir::Location loc = parentOp->getLoc();

  SmallVector<mlir::Attribute> mlirValues;
  for (const mlir::Attribute elementAttr : attr.getElts()) {
    mlir::Attribute mlirAttr;
    if (auto intAttr = mlir::dyn_cast<cir::IntAttr>(elementAttr)) {
      mlirAttr = rewriter.getIntegerAttr(
          converter->convertType(intAttr.getType()), intAttr.getValue());
    } else if (auto floatAttr = mlir::dyn_cast<cir::FPAttr>(elementAttr)) {
      mlirAttr = rewriter.getFloatAttr(
          converter->convertType(floatAttr.getType()), floatAttr.getValue());
    } else {
      llvm_unreachable(
          "vector constant with an element that is neither an int nor a float");
    }
    mlirValues.push_back(mlirAttr);
  }

  return rewriter.create<mlir::LLVM::ConstantOp>(
      loc, llvmTy,
      mlir::DenseElementsAttr::get(mlir::cast<mlir::ShapedType>(llvmTy),
                                   mlirValues));
}

/// ZeroAttr visitor.
mlir::Value CIRAttrToValue::visitCirAttr(cir::ZeroAttr attr) {
  mlir::Location loc = parentOp->getLoc();
  return rewriter.create<mlir::LLVM::ZeroOp>(
      loc, converter->convertType(attr.getType()));
}

// This class handles rewriting initializer attributes for types that do not
// require region initialization.
class GlobalInitAttrRewriter {
public:
  GlobalInitAttrRewriter(mlir::Type type,
                         mlir::ConversionPatternRewriter &rewriter)
      : llvmType(type), rewriter(rewriter) {}

  mlir::Attribute visit(mlir::Attribute attr) {
    return llvm::TypeSwitch<mlir::Attribute, mlir::Attribute>(attr)
        .Case<cir::IntAttr, cir::FPAttr, cir::BoolAttr>(
            [&](auto attrT) { return visitCirAttr(attrT); })
        .Default([&](auto attrT) { return mlir::Attribute(); });
  }

  mlir::Attribute visitCirAttr(cir::IntAttr attr) {
    return rewriter.getIntegerAttr(llvmType, attr.getValue());
  }

  mlir::Attribute visitCirAttr(cir::FPAttr attr) {
    return rewriter.getFloatAttr(llvmType, attr.getValue());
  }

  mlir::Attribute visitCirAttr(cir::BoolAttr attr) {
    return rewriter.getBoolAttr(attr.getValue());
  }

private:
  mlir::Type llvmType;
  mlir::ConversionPatternRewriter &rewriter;
};

// This pass requires the CIR to be in a "flat" state. All blocks in each
// function must belong to the parent region. Once scopes and control flow
// are implemented in CIR, a pass will be run before this one to flatten
// the CIR and get it into the state that this pass requires.
struct ConvertCIRToLLVMPass
    : public mlir::PassWrapper<ConvertCIRToLLVMPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  void getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::BuiltinDialect, mlir::DLTIDialect,
                    mlir::LLVM::LLVMDialect, mlir::func::FuncDialect>();
  }
  void runOnOperation() final;

  void processCIRAttrs(mlir::ModuleOp module);

  StringRef getDescription() const override {
    return "Convert the prepared CIR dialect module to LLVM dialect";
  }

  StringRef getArgument() const override { return "cir-flat-to-llvm"; }
};

mlir::LogicalResult CIRToLLVMAssumeOpLowering::matchAndRewrite(
    cir::AssumeOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto cond = adaptor.getPredicate();
  rewriter.replaceOpWithNewOp<mlir::LLVM::AssumeOp>(op, cond);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMBitClrsbOpLowering::matchAndRewrite(
    cir::BitClrsbOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto zero = rewriter.create<mlir::LLVM::ConstantOp>(
      op.getLoc(), adaptor.getInput().getType(), 0);
  auto isNeg = rewriter.create<mlir::LLVM::ICmpOp>(
      op.getLoc(),
      mlir::LLVM::ICmpPredicateAttr::get(rewriter.getContext(),
                                         mlir::LLVM::ICmpPredicate::slt),
      adaptor.getInput(), zero);

  auto negOne = rewriter.create<mlir::LLVM::ConstantOp>(
      op.getLoc(), adaptor.getInput().getType(), -1);
  auto flipped = rewriter.create<mlir::LLVM::XOrOp>(op.getLoc(),
                                                    adaptor.getInput(), negOne);

  auto select = rewriter.create<mlir::LLVM::SelectOp>(
      op.getLoc(), isNeg, flipped, adaptor.getInput());

  auto resTy = getTypeConverter()->convertType(op.getType());
  auto clz = rewriter.create<mlir::LLVM::CountLeadingZerosOp>(
      op.getLoc(), resTy, select, /*is_zero_poison=*/false);

  auto one = rewriter.create<mlir::LLVM::ConstantOp>(op.getLoc(), resTy, 1);
  auto res = rewriter.create<mlir::LLVM::SubOp>(op.getLoc(), clz, one);
  rewriter.replaceOp(op, res);

  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMBitClzOpLowering::matchAndRewrite(
    cir::BitClzOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto resTy = getTypeConverter()->convertType(op.getType());
  auto llvmOp = rewriter.create<mlir::LLVM::CountLeadingZerosOp>(
      op.getLoc(), resTy, adaptor.getInput(), op.getPoisonZero());
  rewriter.replaceOp(op, llvmOp);
  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMBitCtzOpLowering::matchAndRewrite(
    cir::BitCtzOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto resTy = getTypeConverter()->convertType(op.getType());
  auto llvmOp = rewriter.create<mlir::LLVM::CountTrailingZerosOp>(
      op.getLoc(), resTy, adaptor.getInput(), op.getPoisonZero());
  rewriter.replaceOp(op, llvmOp);
  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMBitParityOpLowering::matchAndRewrite(
    cir::BitParityOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto resTy = getTypeConverter()->convertType(op.getType());
  auto popcnt = rewriter.create<mlir::LLVM::CtPopOp>(op.getLoc(), resTy,
                                                     adaptor.getInput());

  auto one = rewriter.create<mlir::LLVM::ConstantOp>(op.getLoc(), resTy, 1);
  auto popcntMod2 =
      rewriter.create<mlir::LLVM::AndOp>(op.getLoc(), popcnt, one);
  rewriter.replaceOp(op, popcntMod2);

  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMBitPopcountOpLowering::matchAndRewrite(
    cir::BitPopcountOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto resTy = getTypeConverter()->convertType(op.getType());
  auto llvmOp = rewriter.create<mlir::LLVM::CtPopOp>(op.getLoc(), resTy,
                                                     adaptor.getInput());
  rewriter.replaceOp(op, llvmOp);
  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMBitReverseOpLowering::matchAndRewrite(
    cir::BitReverseOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::BitReverseOp>(op, adaptor.getInput());
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMBrCondOpLowering::matchAndRewrite(
    cir::BrCondOp brOp, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // When ZExtOp is implemented, we'll need to check if the condition is a
  // ZExtOp and if so, delete it if it has a single use.
  assert(!cir::MissingFeatures::zextOp());

  mlir::Value i1Condition = adaptor.getCond();

  rewriter.replaceOpWithNewOp<mlir::LLVM::CondBrOp>(
      brOp, i1Condition, brOp.getDestTrue(), adaptor.getDestOperandsTrue(),
      brOp.getDestFalse(), adaptor.getDestOperandsFalse());

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMByteSwapOpLowering::matchAndRewrite(
    cir::ByteSwapOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::ByteSwapOp>(op, adaptor.getInput());
  return mlir::LogicalResult::success();
}

mlir::Type CIRToLLVMCastOpLowering::convertTy(mlir::Type ty) const {
  return getTypeConverter()->convertType(ty);
}

mlir::LogicalResult CIRToLLVMCastOpLowering::matchAndRewrite(
    cir::CastOp castOp, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // For arithmetic conversions, LLVM IR uses the same instruction to convert
  // both individual scalars and entire vectors. This lowering pass handles
  // both situations.

  switch (castOp.getKind()) {
  case cir::CastKind::array_to_ptrdecay: {
    const auto ptrTy = mlir::cast<cir::PointerType>(castOp.getType());
    mlir::Value sourceValue = adaptor.getSrc();
    mlir::Type targetType = convertTy(ptrTy);
    mlir::Type elementTy = convertTypeForMemory(*getTypeConverter(), dataLayout,
                                                ptrTy.getPointee());
    llvm::SmallVector<mlir::LLVM::GEPArg> offset{0};
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        castOp, targetType, elementTy, sourceValue, offset);
    break;
  }
  case cir::CastKind::int_to_bool: {
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Value zeroInt = rewriter.create<mlir::LLVM::ConstantOp>(
        castOp.getLoc(), llvmSrcVal.getType(), 0);
    rewriter.replaceOpWithNewOp<mlir::LLVM::ICmpOp>(
        castOp, mlir::LLVM::ICmpPredicate::ne, llvmSrcVal, zeroInt);
    break;
  }
  case cir::CastKind::integral: {
    mlir::Type srcType = castOp.getSrc().getType();
    mlir::Type dstType = castOp.getType();
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstType = getTypeConverter()->convertType(dstType);
    cir::IntType srcIntType =
        mlir::cast<cir::IntType>(elementTypeIfVector(srcType));
    cir::IntType dstIntType =
        mlir::cast<cir::IntType>(elementTypeIfVector(dstType));
    rewriter.replaceOp(castOp, getLLVMIntCast(rewriter, llvmSrcVal, llvmDstType,
                                              srcIntType.isUnsigned(),
                                              srcIntType.getWidth(),
                                              dstIntType.getWidth()));
    break;
  }
  case cir::CastKind::floating: {
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(castOp.getType());

    mlir::Type srcTy = elementTypeIfVector(castOp.getSrc().getType());
    mlir::Type dstTy = elementTypeIfVector(castOp.getType());

    if (!mlir::isa<cir::FPTypeInterface>(dstTy) ||
        !mlir::isa<cir::FPTypeInterface>(srcTy))
      return castOp.emitError() << "NYI cast from " << srcTy << " to " << dstTy;

    auto getFloatWidth = [](mlir::Type ty) -> unsigned {
      return mlir::cast<cir::FPTypeInterface>(ty).getWidth();
    };

    if (getFloatWidth(srcTy) > getFloatWidth(dstTy))
      rewriter.replaceOpWithNewOp<mlir::LLVM::FPTruncOp>(castOp, llvmDstTy,
                                                         llvmSrcVal);
    else
      rewriter.replaceOpWithNewOp<mlir::LLVM::FPExtOp>(castOp, llvmDstTy,
                                                       llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::int_to_ptr: {
    auto dstTy = mlir::cast<cir::PointerType>(castOp.getType());
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    rewriter.replaceOpWithNewOp<mlir::LLVM::IntToPtrOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::ptr_to_int: {
    auto dstTy = mlir::cast<cir::IntType>(castOp.getType());
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    rewriter.replaceOpWithNewOp<mlir::LLVM::PtrToIntOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::float_to_bool: {
    mlir::Value llvmSrcVal = adaptor.getSrc();
    auto kind = mlir::LLVM::FCmpPredicate::une;

    // Check if float is not equal to zero.
    auto zeroFloat = rewriter.create<mlir::LLVM::ConstantOp>(
        castOp.getLoc(), llvmSrcVal.getType(),
        mlir::FloatAttr::get(llvmSrcVal.getType(), 0.0));

    // Extend comparison result to either bool (C++) or int (C).
    rewriter.replaceOpWithNewOp<mlir::LLVM::FCmpOp>(castOp, kind, llvmSrcVal,
                                                    zeroFloat);

    return mlir::success();
  }
  case cir::CastKind::bool_to_int: {
    auto dstTy = mlir::cast<cir::IntType>(castOp.getType());
    mlir::Value llvmSrcVal = adaptor.getSrc();
    auto llvmSrcTy = mlir::cast<mlir::IntegerType>(llvmSrcVal.getType());
    auto llvmDstTy =
        mlir::cast<mlir::IntegerType>(getTypeConverter()->convertType(dstTy));
    if (llvmSrcTy.getWidth() == llvmDstTy.getWidth())
      rewriter.replaceOpWithNewOp<mlir::LLVM::BitcastOp>(castOp, llvmDstTy,
                                                         llvmSrcVal);
    else
      rewriter.replaceOpWithNewOp<mlir::LLVM::ZExtOp>(castOp, llvmDstTy,
                                                      llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::bool_to_float: {
    mlir::Type dstTy = castOp.getType();
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    rewriter.replaceOpWithNewOp<mlir::LLVM::UIToFPOp>(castOp, llvmDstTy,
                                                      llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::int_to_float: {
    mlir::Type dstTy = castOp.getType();
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    if (mlir::cast<cir::IntType>(elementTypeIfVector(castOp.getSrc().getType()))
            .isSigned())
      rewriter.replaceOpWithNewOp<mlir::LLVM::SIToFPOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    else
      rewriter.replaceOpWithNewOp<mlir::LLVM::UIToFPOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::float_to_int: {
    mlir::Type dstTy = castOp.getType();
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    if (mlir::cast<cir::IntType>(elementTypeIfVector(castOp.getType()))
            .isSigned())
      rewriter.replaceOpWithNewOp<mlir::LLVM::FPToSIOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    else
      rewriter.replaceOpWithNewOp<mlir::LLVM::FPToUIOp>(castOp, llvmDstTy,
                                                        llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::bitcast: {
    mlir::Type dstTy = castOp.getType();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);

    assert(!MissingFeatures::cxxABI());
    assert(!MissingFeatures::dataMemberType());

    mlir::Value llvmSrcVal = adaptor.getSrc();
    rewriter.replaceOpWithNewOp<mlir::LLVM::BitcastOp>(castOp, llvmDstTy,
                                                       llvmSrcVal);
    return mlir::success();
  }
  case cir::CastKind::ptr_to_bool: {
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Value zeroPtr = rewriter.create<mlir::LLVM::ZeroOp>(
        castOp.getLoc(), llvmSrcVal.getType());
    rewriter.replaceOpWithNewOp<mlir::LLVM::ICmpOp>(
        castOp, mlir::LLVM::ICmpPredicate::ne, llvmSrcVal, zeroPtr);
    break;
  }
  case cir::CastKind::address_space: {
    mlir::Type dstTy = castOp.getType();
    mlir::Value llvmSrcVal = adaptor.getSrc();
    mlir::Type llvmDstTy = getTypeConverter()->convertType(dstTy);
    rewriter.replaceOpWithNewOp<mlir::LLVM::AddrSpaceCastOp>(castOp, llvmDstTy,
                                                             llvmSrcVal);
    break;
  }
  case cir::CastKind::member_ptr_to_bool:
    assert(!MissingFeatures::cxxABI());
    assert(!MissingFeatures::methodType());
    break;
  default: {
    return castOp.emitError("Unhandled cast kind: ")
           << castOp.getKindAttrName();
  }
  }

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMPtrStrideOpLowering::matchAndRewrite(
    cir::PtrStrideOp ptrStrideOp, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {

  const mlir::TypeConverter *tc = getTypeConverter();
  const mlir::Type resultTy = tc->convertType(ptrStrideOp.getType());

  mlir::Type elementTy =
      convertTypeForMemory(*tc, dataLayout, ptrStrideOp.getElementTy());
  mlir::MLIRContext *ctx = elementTy.getContext();

  // void and function types doesn't really have a layout to use in GEPs,
  // make it i8 instead.
  if (mlir::isa<mlir::LLVM::LLVMVoidType>(elementTy) ||
      mlir::isa<mlir::LLVM::LLVMFunctionType>(elementTy))
    elementTy = mlir::IntegerType::get(elementTy.getContext(), 8,
                                       mlir::IntegerType::Signless);
  // Zero-extend, sign-extend or trunc the pointer value.
  mlir::Value index = adaptor.getStride();
  const unsigned width =
      mlir::cast<mlir::IntegerType>(index.getType()).getWidth();
  const std::optional<std::uint64_t> layoutWidth =
      dataLayout.getTypeIndexBitwidth(adaptor.getBase().getType());

  mlir::Operation *indexOp = index.getDefiningOp();
  if (indexOp && layoutWidth && width != *layoutWidth) {
    // If the index comes from a subtraction, make sure the extension happens
    // before it. To achieve that, look at unary minus, which already got
    // lowered to "sub 0, x".
    const auto sub = dyn_cast<mlir::LLVM::SubOp>(indexOp);
    auto unary = dyn_cast_if_present<cir::UnaryOp>(
        ptrStrideOp.getStride().getDefiningOp());
    bool rewriteSub =
        unary && unary.getKind() == cir::UnaryOpKind::Minus && sub;
    if (rewriteSub)
      index = indexOp->getOperand(1);

    // Handle the cast
    const auto llvmDstType = mlir::IntegerType::get(ctx, *layoutWidth);
    index = getLLVMIntCast(rewriter, index, llvmDstType,
                           ptrStrideOp.getStride().getType().isUnsigned(),
                           width, *layoutWidth);

    // Rewrite the sub in front of extensions/trunc
    if (rewriteSub) {
      index = rewriter.create<mlir::LLVM::SubOp>(
          index.getLoc(), index.getType(),
          rewriter.create<mlir::LLVM::ConstantOp>(index.getLoc(),
                                                  index.getType(), 0),
          index);
      rewriter.eraseOp(sub);
    }
  }

  rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
      ptrStrideOp, resultTy, elementTy, adaptor.getBase(), index);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMBaseClassAddrOpLowering::matchAndRewrite(
    cir::BaseClassAddrOp baseClassOp, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  const mlir::Type resultType =
      getTypeConverter()->convertType(baseClassOp.getType());
  mlir::Value derivedAddr = adaptor.getDerivedAddr();
  llvm::SmallVector<mlir::LLVM::GEPArg, 1> offset = {
      adaptor.getOffset().getZExtValue()};
  mlir::Type byteType = mlir::IntegerType::get(resultType.getContext(), 8,
                                               mlir::IntegerType::Signless);
  if (adaptor.getOffset().getZExtValue() == 0) {
    rewriter.replaceOpWithNewOp<mlir::LLVM::BitcastOp>(
        baseClassOp, resultType, adaptor.getDerivedAddr());
    return mlir::success();
  }

  if (baseClassOp.getAssumeNotNull()) {
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        baseClassOp, resultType, byteType, derivedAddr, offset);
  } else {
    auto loc = baseClassOp.getLoc();
    mlir::Value isNull = rewriter.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, derivedAddr,
        rewriter.create<mlir::LLVM::ZeroOp>(loc, derivedAddr.getType()));
    mlir::Value adjusted = rewriter.create<mlir::LLVM::GEPOp>(
        loc, resultType, byteType, derivedAddr, offset);
    rewriter.replaceOpWithNewOp<mlir::LLVM::SelectOp>(baseClassOp, isNull,
                                                      derivedAddr, adjusted);
  }
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMAllocaOpLowering::matchAndRewrite(
    cir::AllocaOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  assert(!cir::MissingFeatures::opAllocaDynAllocSize());
  mlir::Value size = rewriter.create<mlir::LLVM::ConstantOp>(
      op.getLoc(), typeConverter->convertType(rewriter.getIndexType()), 1);
  mlir::Type elementTy =
      convertTypeForMemory(*getTypeConverter(), dataLayout, op.getAllocaType());
  mlir::Type resultTy =
      convertTypeForMemory(*getTypeConverter(), dataLayout, op.getType());

  assert(!cir::MissingFeatures::addressSpace());
  assert(!cir::MissingFeatures::opAllocaAnnotations());

  rewriter.replaceOpWithNewOp<mlir::LLVM::AllocaOp>(
      op, resultTy, elementTy, size, op.getAlignmentAttr().getInt());

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMReturnOpLowering::matchAndRewrite(
    cir::ReturnOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::ReturnOp>(op, adaptor.getOperands());
  return mlir::LogicalResult::success();
}

static mlir::LogicalResult
rewriteCallOrInvoke(mlir::Operation *op, mlir::ValueRange callOperands,
                    mlir::ConversionPatternRewriter &rewriter,
                    const mlir::TypeConverter *converter,
                    mlir::FlatSymbolRefAttr calleeAttr) {
  llvm::SmallVector<mlir::Type, 8> llvmResults;
  mlir::ValueTypeRange<mlir::ResultRange> cirResults = op->getResultTypes();
  auto call = cast<cir::CIRCallOpInterface>(op);

  if (converter->convertTypes(cirResults, llvmResults).failed())
    return mlir::failure();

  assert(!cir::MissingFeatures::opCallCallConv());

  mlir::LLVM::MemoryEffectsAttr memoryEffects;
  bool noUnwind = false;
  bool willReturn = false;
  convertSideEffectForCall(op, call.getNothrow(), call.getSideEffect(),
                           memoryEffects, noUnwind, willReturn);

  mlir::LLVM::LLVMFunctionType llvmFnTy;
  if (calleeAttr) { // direct call
    mlir::FunctionOpInterface fn =
        mlir::SymbolTable::lookupNearestSymbolFrom<mlir::FunctionOpInterface>(
            op, calleeAttr);
    assert(fn && "Did not find function for call");
    llvmFnTy = cast<mlir::LLVM::LLVMFunctionType>(
        converter->convertType(fn.getFunctionType()));
  } else { // indirect call
    assert(!op->getOperands().empty() &&
           "operands list must no be empty for the indirect call");
    auto calleeTy = op->getOperands().front().getType();
    auto calleePtrTy = cast<cir::PointerType>(calleeTy);
    auto calleeFuncTy = cast<cir::FuncType>(calleePtrTy.getPointee());
    calleeFuncTy.dump();
    converter->convertType(calleeFuncTy).dump();
    llvmFnTy = cast<mlir::LLVM::LLVMFunctionType>(
        converter->convertType(calleeFuncTy));
  }

  assert(!cir::MissingFeatures::opCallLandingPad());
  assert(!cir::MissingFeatures::opCallContinueBlock());
  assert(!cir::MissingFeatures::opCallCallConv());

  auto newOp = rewriter.replaceOpWithNewOp<mlir::LLVM::CallOp>(
      op, llvmFnTy, calleeAttr, callOperands);
  if (memoryEffects)
    newOp.setMemoryEffectsAttr(memoryEffects);
  newOp.setNoUnwind(noUnwind);
  newOp.setWillReturn(willReturn);

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMCallOpLowering::matchAndRewrite(
    cir::CallOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  return rewriteCallOrInvoke(op.getOperation(), adaptor.getOperands(), rewriter,
                             getTypeConverter(), op.getCalleeAttr());
}

mlir::LogicalResult CIRToLLVMLoadOpLowering::matchAndRewrite(
    cir::LoadOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  const mlir::Type llvmTy =
      convertTypeForMemory(*getTypeConverter(), dataLayout, op.getType());
  assert(!cir::MissingFeatures::opLoadStoreMemOrder());
  std::optional<size_t> opAlign = op.getAlignment();
  unsigned alignment =
      (unsigned)opAlign.value_or(dataLayout.getTypeABIAlignment(llvmTy));

  assert(!cir::MissingFeatures::lowerModeOptLevel());

  // TODO: nontemporal, syncscope.
  assert(!cir::MissingFeatures::opLoadStoreVolatile());
  mlir::LLVM::LoadOp newLoad = rewriter.create<mlir::LLVM::LoadOp>(
      op->getLoc(), llvmTy, adaptor.getAddr(), alignment,
      /*volatile=*/false, /*nontemporal=*/false,
      /*invariant=*/false, /*invariantGroup=*/false,
      mlir::LLVM::AtomicOrdering::not_atomic);

  // Convert adapted result to its original type if needed.
  mlir::Value result =
      emitFromMemory(rewriter, dataLayout, op, newLoad.getResult());
  rewriter.replaceOp(op, result);
  assert(!cir::MissingFeatures::opLoadStoreTbaa());
  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMStoreOpLowering::matchAndRewrite(
    cir::StoreOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  assert(!cir::MissingFeatures::opLoadStoreMemOrder());
  const mlir::Type llvmTy =
      getTypeConverter()->convertType(op.getValue().getType());
  std::optional<size_t> opAlign = op.getAlignment();
  unsigned alignment =
      (unsigned)opAlign.value_or(dataLayout.getTypeABIAlignment(llvmTy));

  assert(!cir::MissingFeatures::lowerModeOptLevel());

  // Convert adapted value to its memory type if needed.
  mlir::Value value = emitToMemory(rewriter, dataLayout,
                                   op.getValue().getType(), adaptor.getValue());
  // TODO: nontemporal, syncscope.
  assert(!cir::MissingFeatures::opLoadStoreVolatile());
  mlir::LLVM::StoreOp storeOp = rewriter.create<mlir::LLVM::StoreOp>(
      op->getLoc(), value, adaptor.getAddr(), alignment, /*volatile=*/false,
      /*nontemporal=*/false, /*invariantGroup=*/false,
      mlir::LLVM::AtomicOrdering::not_atomic);
  rewriter.replaceOp(op, storeOp);
  assert(!cir::MissingFeatures::opLoadStoreTbaa());
  return mlir::LogicalResult::success();
}

bool hasTrailingZeros(cir::ConstArrayAttr attr) {
  auto array = mlir::dyn_cast<mlir::ArrayAttr>(attr.getElts());
  return attr.hasTrailingZeros() ||
         (array && std::count_if(array.begin(), array.end(), [](auto elt) {
            auto ar = dyn_cast<cir::ConstArrayAttr>(elt);
            return ar && hasTrailingZeros(ar);
          }));
}

mlir::LogicalResult CIRToLLVMConstantOpLowering::matchAndRewrite(
    cir::ConstantOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Attribute attr = op.getValue();

  if (mlir::isa<mlir::IntegerType>(op.getType())) {
    // Verified cir.const operations cannot actually be of these types, but the
    // lowering pass may generate temporary cir.const operations with these
    // types. This is OK since MLIR allows unverified operations to be alive
    // during a pass as long as they don't live past the end of the pass.
    attr = op.getValue();
  } else if (mlir::isa<cir::BoolType>(op.getType())) {
    int value = mlir::cast<cir::BoolAttr>(op.getValue()).getValue();
    attr = rewriter.getIntegerAttr(typeConverter->convertType(op.getType()),
                                   value);
  } else if (mlir::isa<cir::IntType>(op.getType())) {
    assert(!cir::MissingFeatures::opGlobalViewAttr());

    attr = rewriter.getIntegerAttr(
        typeConverter->convertType(op.getType()),
        mlir::cast<cir::IntAttr>(op.getValue()).getValue());
  } else if (mlir::isa<cir::FPTypeInterface>(op.getType())) {
    attr = rewriter.getFloatAttr(
        typeConverter->convertType(op.getType()),
        mlir::cast<cir::FPAttr>(op.getValue()).getValue());
  } else if (mlir::isa<cir::PointerType>(op.getType())) {
    // Optimize with dedicated LLVM op for null pointers.
    if (mlir::isa<cir::ConstPtrAttr>(op.getValue())) {
      if (mlir::cast<cir::ConstPtrAttr>(op.getValue()).isNullValue()) {
        rewriter.replaceOpWithNewOp<mlir::LLVM::ZeroOp>(
            op, typeConverter->convertType(op.getType()));
        return mlir::success();
      }
    }
    assert(!cir::MissingFeatures::opGlobalViewAttr());
    attr = op.getValue();
  } else if (const auto arrTy = mlir::dyn_cast<cir::ArrayType>(op.getType())) {
    const auto constArr = mlir::dyn_cast<cir::ConstArrayAttr>(op.getValue());
    if (!constArr && !isa<cir::ZeroAttr, cir::UndefAttr>(op.getValue()))
      return op.emitError() << "array does not have a constant initializer";

    std::optional<mlir::Attribute> denseAttr;
    if (constArr && hasTrailingZeros(constArr)) {
      const mlir::Value newOp =
          lowerCirAttrAsValue(op, constArr, rewriter, getTypeConverter());
      rewriter.replaceOp(op, newOp);
      return mlir::success();
    } else if (constArr &&
               (denseAttr = lowerConstArrayAttr(constArr, typeConverter))) {
      attr = denseAttr.value();
    } else {
      const mlir::Value initVal =
          lowerCirAttrAsValue(op, op.getValue(), rewriter, typeConverter);
      rewriter.replaceAllUsesWith(op, initVal);
      rewriter.eraseOp(op);
      return mlir::success();
    }
  } else if (const auto vecTy = mlir::dyn_cast<cir::VectorType>(op.getType())) {
    rewriter.replaceOp(op, lowerCirAttrAsValue(op, op.getValue(), rewriter,
                                               getTypeConverter()));
    return mlir::success();
  } else if (auto complexTy = mlir::dyn_cast<cir::ComplexType>(op.getType())) {
    mlir::Type complexElemTy = complexTy.getElementType();
    mlir::Type complexElemLLVMTy = typeConverter->convertType(complexElemTy);

    if (auto zeroInitAttr = mlir::dyn_cast<cir::ZeroAttr>(op.getValue())) {
      mlir::TypedAttr zeroAttr = rewriter.getZeroAttr(complexElemLLVMTy);
      mlir::ArrayAttr array = rewriter.getArrayAttr({zeroAttr, zeroAttr});
      rewriter.replaceOpWithNewOp<mlir::LLVM::ConstantOp>(
          op, getTypeConverter()->convertType(op.getType()), array);
      return mlir::success();
    }

    auto complexAttr = mlir::cast<cir::ConstComplexAttr>(op.getValue());

    mlir::Attribute components[2];
    if (mlir::isa<cir::IntType>(complexElemTy)) {
      components[0] = rewriter.getIntegerAttr(
          complexElemLLVMTy,
          mlir::cast<cir::IntAttr>(complexAttr.getReal()).getValue());
      components[1] = rewriter.getIntegerAttr(
          complexElemLLVMTy,
          mlir::cast<cir::IntAttr>(complexAttr.getImag()).getValue());
    } else {
      components[0] = rewriter.getFloatAttr(
          complexElemLLVMTy,
          mlir::cast<cir::FPAttr>(complexAttr.getReal()).getValue());
      components[1] = rewriter.getFloatAttr(
          complexElemLLVMTy,
          mlir::cast<cir::FPAttr>(complexAttr.getImag()).getValue());
    }

    attr = rewriter.getArrayAttr(components);
  } else {
    return op.emitError() << "unsupported constant type " << op.getType();
  }

  rewriter.replaceOpWithNewOp<mlir::LLVM::ConstantOp>(
      op, getTypeConverter()->convertType(op.getType()), attr);

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMExpectOpLowering::matchAndRewrite(
    cir::ExpectOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // TODO(cir): do not generate LLVM intrinsics under -O0
  assert(!cir::MissingFeatures::optInfoAttr());

  std::optional<llvm::APFloat> prob = op.getProb();
  if (prob)
    rewriter.replaceOpWithNewOp<mlir::LLVM::ExpectWithProbabilityOp>(
        op, adaptor.getVal(), adaptor.getExpected(), prob.value());
  else
    rewriter.replaceOpWithNewOp<mlir::LLVM::ExpectOp>(op, adaptor.getVal(),
                                                      adaptor.getExpected());
  return mlir::success();
}

/// Convert the `cir.func` attributes to `llvm.func` attributes.
/// Only retain those attributes that are not constructed by
/// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out
/// argument attributes.
void CIRToLLVMFuncOpLowering::lowerFuncAttributes(
    cir::FuncOp func, bool filterArgAndResAttrs,
    SmallVectorImpl<mlir::NamedAttribute> &result) const {
  assert(!cir::MissingFeatures::opFuncCallingConv());
  for (mlir::NamedAttribute attr : func->getAttrs()) {
    assert(!cir::MissingFeatures::opFuncCallingConv());
    if (attr.getName() == mlir::SymbolTable::getSymbolAttrName() ||
        attr.getName() == func.getFunctionTypeAttrName() ||
        attr.getName() == getLinkageAttrNameString() ||
        attr.getName() == func.getGlobalVisibilityAttrName() ||
        attr.getName() == func.getDsoLocalAttrName() ||
        (filterArgAndResAttrs &&
         (attr.getName() == func.getArgAttrsAttrName() ||
          attr.getName() == func.getResAttrsAttrName())))
      continue;

    assert(!cir::MissingFeatures::opFuncExtraAttrs());
    result.push_back(attr);
  }
}

mlir::LogicalResult CIRToLLVMFuncOpLowering::matchAndRewrite(
    cir::FuncOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {

  cir::FuncType fnType = op.getFunctionType();
  bool isDsoLocal = op.getDsoLocal();
  mlir::TypeConverter::SignatureConversion signatureConversion(
      fnType.getNumInputs());

  for (const auto &argType : llvm::enumerate(fnType.getInputs())) {
    mlir::Type convertedType = typeConverter->convertType(argType.value());
    if (!convertedType)
      return mlir::failure();
    signatureConversion.addInputs(argType.index(), convertedType);
  }

  mlir::Type resultType =
      getTypeConverter()->convertType(fnType.getReturnType());

  // Create the LLVM function operation.
  mlir::Type llvmFnTy = mlir::LLVM::LLVMFunctionType::get(
      resultType ? resultType : mlir::LLVM::LLVMVoidType::get(getContext()),
      signatureConversion.getConvertedTypes(),
      /*isVarArg=*/fnType.isVarArg());
  // LLVMFuncOp expects a single FileLine Location instead of a fused
  // location.
  mlir::Location loc = op.getLoc();
  if (mlir::FusedLoc fusedLoc = mlir::dyn_cast<mlir::FusedLoc>(loc))
    loc = fusedLoc.getLocations()[0];
  assert((mlir::isa<mlir::FileLineColLoc>(loc) ||
          mlir::isa<mlir::UnknownLoc>(loc)) &&
         "expected single location or unknown location here");

  mlir::LLVM::Linkage linkage = convertLinkage(op.getLinkage());
  assert(!cir::MissingFeatures::opFuncCallingConv());
  mlir::LLVM::CConv cconv = mlir::LLVM::CConv::C;
  SmallVector<mlir::NamedAttribute, 4> attributes;
  lowerFuncAttributes(op, /*filterArgAndResAttrs=*/false, attributes);

  mlir::LLVM::LLVMFuncOp fn = rewriter.create<mlir::LLVM::LLVMFuncOp>(
      loc, op.getName(), llvmFnTy, linkage, isDsoLocal, cconv,
      mlir::SymbolRefAttr(), attributes);

  assert(!cir::MissingFeatures::opFuncMultipleReturnVals());

  fn.setVisibility_Attr(mlir::LLVM::VisibilityAttr::get(
      getContext(), lowerCIRVisibilityToLLVMVisibility(
                        op.getGlobalVisibilityAttr().getValue())));

  rewriter.inlineRegionBefore(op.getBody(), fn.getBody(), fn.end());
  if (failed(rewriter.convertRegionTypes(&fn.getBody(), *typeConverter,
                                         &signatureConversion)))
    return mlir::failure();

  rewriter.eraseOp(op);

  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMGetGlobalOpLowering::matchAndRewrite(
    cir::GetGlobalOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // FIXME(cir): Premature DCE to avoid lowering stuff we're not using.
  // CIRGen should mitigate this and not emit the get_global.
  if (op->getUses().empty()) {
    rewriter.eraseOp(op);
    return mlir::success();
  }

  mlir::Type type = getTypeConverter()->convertType(op.getType());
  mlir::Operation *newop =
      rewriter.create<mlir::LLVM::AddressOfOp>(op.getLoc(), type, op.getName());

  assert(!cir::MissingFeatures::opGlobalThreadLocal());

  rewriter.replaceOp(op, newop);
  return mlir::success();
}

/// Replace CIR global with a region initialized LLVM global and update
/// insertion point to the end of the initializer block.
void CIRToLLVMGlobalOpLowering::setupRegionInitializedLLVMGlobalOp(
    cir::GlobalOp op, mlir::ConversionPatternRewriter &rewriter) const {
  const mlir::Type llvmType =
      convertTypeForMemory(*getTypeConverter(), dataLayout, op.getSymType());

  // FIXME: These default values are placeholders until the the equivalent
  //        attributes are available on cir.global ops. This duplicates code
  //        in CIRToLLVMGlobalOpLowering::matchAndRewrite() but that will go
  //        away when the placeholders are no longer needed.
  assert(!cir::MissingFeatures::opGlobalConstant());
  const bool isConst = false;
  assert(!cir::MissingFeatures::addressSpace());
  const unsigned addrSpace = 0;
  const bool isDsoLocal = op.getDsoLocal();
  assert(!cir::MissingFeatures::opGlobalThreadLocal());
  const bool isThreadLocal = false;
  const uint64_t alignment = op.getAlignment().value_or(0);
  const mlir::LLVM::Linkage linkage = convertLinkage(op.getLinkage());
  const StringRef symbol = op.getSymName();
  mlir::SymbolRefAttr comdatAttr = getComdatAttr(op, rewriter);

  SmallVector<mlir::NamedAttribute> attributes;
  mlir::LLVM::GlobalOp newGlobalOp =
      rewriter.replaceOpWithNewOp<mlir::LLVM::GlobalOp>(
          op, llvmType, isConst, linkage, symbol, nullptr, alignment, addrSpace,
          isDsoLocal, isThreadLocal, comdatAttr, attributes);
  newGlobalOp.getRegion().emplaceBlock();
  rewriter.setInsertionPointToEnd(newGlobalOp.getInitializerBlock());
}

mlir::LogicalResult
CIRToLLVMGlobalOpLowering::matchAndRewriteRegionInitializedGlobal(
    cir::GlobalOp op, mlir::Attribute init,
    mlir::ConversionPatternRewriter &rewriter) const {
  // TODO: Generalize this handling when more types are needed here.
  assert((isa<cir::ConstArrayAttr, cir::ConstVectorAttr, cir::ConstPtrAttr,
              cir::ConstComplexAttr, cir::ZeroAttr>(init)));

  // TODO(cir): once LLVM's dialect has proper equivalent attributes this
  // should be updated. For now, we use a custom op to initialize globals
  // to the appropriate value.
  const mlir::Location loc = op.getLoc();
  setupRegionInitializedLLVMGlobalOp(op, rewriter);
  CIRAttrToValue valueConverter(op, rewriter, typeConverter);
  mlir::Value value = valueConverter.visit(init);
  rewriter.create<mlir::LLVM::ReturnOp>(loc, value);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMGlobalOpLowering::matchAndRewrite(
    cir::GlobalOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {

  std::optional<mlir::Attribute> init = op.getInitialValue();

  // Fetch required values to create LLVM op.
  const mlir::Type cirSymType = op.getSymType();

  // This is the LLVM dialect type.
  const mlir::Type llvmType =
      convertTypeForMemory(*getTypeConverter(), dataLayout, cirSymType);
  // FIXME: These default values are placeholders until the the equivalent
  //        attributes are available on cir.global ops.
  assert(!cir::MissingFeatures::opGlobalConstant());
  const bool isConst = false;
  assert(!cir::MissingFeatures::addressSpace());
  const unsigned addrSpace = 0;
  const bool isDsoLocal = op.getDsoLocal();
  assert(!cir::MissingFeatures::opGlobalThreadLocal());
  const bool isThreadLocal = false;
  const uint64_t alignment = op.getAlignment().value_or(0);
  const mlir::LLVM::Linkage linkage = convertLinkage(op.getLinkage());
  const StringRef symbol = op.getSymName();
  SmallVector<mlir::NamedAttribute> attributes;
  mlir::SymbolRefAttr comdatAttr = getComdatAttr(op, rewriter);

  if (init.has_value()) {
    if (mlir::isa<cir::FPAttr, cir::IntAttr, cir::BoolAttr>(init.value())) {
      GlobalInitAttrRewriter initRewriter(llvmType, rewriter);
      init = initRewriter.visit(init.value());
      // If initRewriter returned a null attribute, init will have a value but
      // the value will be null. If that happens, initRewriter didn't handle the
      // attribute type. It probably needs to be added to
      // GlobalInitAttrRewriter.
      if (!init.value()) {
        op.emitError() << "unsupported initializer '" << init.value() << "'";
        return mlir::failure();
      }
    } else if (mlir::isa<cir::ConstArrayAttr, cir::ConstVectorAttr,
                         cir::ConstPtrAttr, cir::ConstComplexAttr,
                         cir::ZeroAttr>(init.value())) {
      // TODO(cir): once LLVM's dialect has proper equivalent attributes this
      // should be updated. For now, we use a custom op to initialize globals
      // to the appropriate value.
      return matchAndRewriteRegionInitializedGlobal(op, init.value(), rewriter);
    } else {
      // We will only get here if new initializer types are added and this
      // code is not updated to handle them.
      op.emitError() << "unsupported initializer '" << init.value() << "'";
      return mlir::failure();
    }
  }

  // Rewrite op.
  rewriter.replaceOpWithNewOp<mlir::LLVM::GlobalOp>(
      op, llvmType, isConst, linkage, symbol, init.value_or(mlir::Attribute()),
      alignment, addrSpace, isDsoLocal, isThreadLocal, comdatAttr, attributes);
  return mlir::success();
}

mlir::SymbolRefAttr
CIRToLLVMGlobalOpLowering::getComdatAttr(cir::GlobalOp &op,
                                         mlir::OpBuilder &builder) const {
  if (!op.getComdat())
    return mlir::SymbolRefAttr{};

  mlir::ModuleOp module = op->getParentOfType<mlir::ModuleOp>();
  mlir::OpBuilder::InsertionGuard guard(builder);
  StringRef comdatName("__llvm_comdat_globals");
  if (!comdatOp) {
    builder.setInsertionPointToStart(module.getBody());
    comdatOp =
        builder.create<mlir::LLVM::ComdatOp>(module.getLoc(), comdatName);
  }

  builder.setInsertionPointToStart(&comdatOp.getBody().back());
  auto selectorOp = builder.create<mlir::LLVM::ComdatSelectorOp>(
      comdatOp.getLoc(), op.getSymName(), mlir::LLVM::comdat::Comdat::Any);
  return mlir::SymbolRefAttr::get(
      builder.getContext(), comdatName,
      mlir::FlatSymbolRefAttr::get(selectorOp.getSymNameAttr()));
}

mlir::LogicalResult CIRToLLVMSwitchFlatOpLowering::matchAndRewrite(
    cir::SwitchFlatOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {

  llvm::SmallVector<mlir::APInt, 8> caseValues;
  for (mlir::Attribute val : op.getCaseValues()) {
    auto intAttr = cast<cir::IntAttr>(val);
    caseValues.push_back(intAttr.getValue());
  }

  llvm::SmallVector<mlir::Block *, 8> caseDestinations;
  llvm::SmallVector<mlir::ValueRange, 8> caseOperands;

  for (mlir::Block *x : op.getCaseDestinations())
    caseDestinations.push_back(x);

  for (mlir::OperandRange x : op.getCaseOperands())
    caseOperands.push_back(x);

  // Set switch op to branch to the newly created blocks.
  rewriter.setInsertionPoint(op);
  rewriter.replaceOpWithNewOp<mlir::LLVM::SwitchOp>(
      op, adaptor.getCondition(), op.getDefaultDestination(),
      op.getDefaultOperands(), caseValues, caseDestinations, caseOperands);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMUnaryOpLowering::matchAndRewrite(
    cir::UnaryOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  assert(op.getType() == op.getInput().getType() &&
         "Unary operation's operand type and result type are different");
  mlir::Type type = op.getType();
  mlir::Type elementType = elementTypeIfVector(type);
  bool isVector = mlir::isa<cir::VectorType>(type);
  mlir::Type llvmType = getTypeConverter()->convertType(type);
  mlir::Location loc = op.getLoc();

  // Integer unary operations: + - ~ ++ --
  if (mlir::isa<cir::IntType>(elementType)) {
    mlir::LLVM::IntegerOverflowFlags maybeNSW =
        op.getNoSignedWrap() ? mlir::LLVM::IntegerOverflowFlags::nsw
                             : mlir::LLVM::IntegerOverflowFlags::none;
    switch (op.getKind()) {
    case cir::UnaryOpKind::Inc: {
      assert(!isVector && "++ not allowed on vector types");
      auto one = rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, 1);
      rewriter.replaceOpWithNewOp<mlir::LLVM::AddOp>(
          op, llvmType, adaptor.getInput(), one, maybeNSW);
      return mlir::success();
    }
    case cir::UnaryOpKind::Dec: {
      assert(!isVector && "-- not allowed on vector types");
      auto one = rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, 1);
      rewriter.replaceOpWithNewOp<mlir::LLVM::SubOp>(op, adaptor.getInput(),
                                                     one, maybeNSW);
      return mlir::success();
    }
    case cir::UnaryOpKind::Plus:
      rewriter.replaceOp(op, adaptor.getInput());
      return mlir::success();
    case cir::UnaryOpKind::Minus: {
      mlir::Value zero;
      if (isVector)
        zero = rewriter.create<mlir::LLVM::ZeroOp>(loc, llvmType);
      else
        zero = rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, 0);
      rewriter.replaceOpWithNewOp<mlir::LLVM::SubOp>(
          op, zero, adaptor.getInput(), maybeNSW);
      return mlir::success();
    }
    case cir::UnaryOpKind::Not: {
      // bit-wise compliment operator, implemented as an XOR with -1.
      mlir::Value minusOne;
      if (isVector) {
        const uint64_t numElements =
            mlir::dyn_cast<cir::VectorType>(type).getSize();
        std::vector<int32_t> values(numElements, -1);
        mlir::DenseIntElementsAttr denseVec = rewriter.getI32VectorAttr(values);
        minusOne =
            rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, denseVec);
      } else {
        minusOne = rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, -1);
      }
      rewriter.replaceOpWithNewOp<mlir::LLVM::XOrOp>(op, adaptor.getInput(),
                                                     minusOne);
      return mlir::success();
    }
    }
    llvm_unreachable("Unexpected unary op for int");
  }

  // Floating point unary operations: + - ++ --
  if (mlir::isa<cir::FPTypeInterface>(elementType)) {
    switch (op.getKind()) {
    case cir::UnaryOpKind::Inc: {
      assert(!isVector && "++ not allowed on vector types");
      mlir::LLVM::ConstantOp one = rewriter.create<mlir::LLVM::ConstantOp>(
          loc, llvmType, rewriter.getFloatAttr(llvmType, 1.0));
      rewriter.replaceOpWithNewOp<mlir::LLVM::FAddOp>(op, llvmType, one,
                                                      adaptor.getInput());
      return mlir::success();
    }
    case cir::UnaryOpKind::Dec: {
      assert(!isVector && "-- not allowed on vector types");
      mlir::LLVM::ConstantOp minusOne = rewriter.create<mlir::LLVM::ConstantOp>(
          loc, llvmType, rewriter.getFloatAttr(llvmType, -1.0));
      rewriter.replaceOpWithNewOp<mlir::LLVM::FAddOp>(op, llvmType, minusOne,
                                                      adaptor.getInput());
      return mlir::success();
    }
    case cir::UnaryOpKind::Plus:
      rewriter.replaceOp(op, adaptor.getInput());
      return mlir::success();
    case cir::UnaryOpKind::Minus:
      rewriter.replaceOpWithNewOp<mlir::LLVM::FNegOp>(op, llvmType,
                                                      adaptor.getInput());
      return mlir::success();
    case cir::UnaryOpKind::Not:
      return op.emitError() << "Unary not is invalid for floating-point types";
    }
    llvm_unreachable("Unexpected unary op for float");
  }

  // Boolean unary operations: ! only. (For all others, the operand has
  // already been promoted to int.)
  if (mlir::isa<cir::BoolType>(elementType)) {
    switch (op.getKind()) {
    case cir::UnaryOpKind::Inc:
    case cir::UnaryOpKind::Dec:
    case cir::UnaryOpKind::Plus:
    case cir::UnaryOpKind::Minus:
      // Some of these are allowed in source code, but we shouldn't get here
      // with a boolean type.
      return op.emitError() << "Unsupported unary operation on boolean type";
    case cir::UnaryOpKind::Not: {
      assert(!isVector && "NYI: op! on vector mask");
      auto one = rewriter.create<mlir::LLVM::ConstantOp>(loc, llvmType, 1);
      rewriter.replaceOpWithNewOp<mlir::LLVM::XOrOp>(op, adaptor.getInput(),
                                                     one);
      return mlir::success();
    }
    }
    llvm_unreachable("Unexpected unary op for bool");
  }

  // Pointer unary operations: + only.  (++ and -- of pointers are implemented
  // with cir.ptr_stride, not cir.unary.)
  if (mlir::isa<cir::PointerType>(elementType)) {
    return op.emitError()
           << "Unary operation on pointer types is not yet implemented";
  }

  return op.emitError() << "Unary operation has unsupported type: "
                        << elementType;
}

mlir::LLVM::IntegerOverflowFlags
CIRToLLVMBinOpLowering::getIntOverflowFlag(cir::BinOp op) const {
  if (op.getNoUnsignedWrap())
    return mlir::LLVM::IntegerOverflowFlags::nuw;

  if (op.getNoSignedWrap())
    return mlir::LLVM::IntegerOverflowFlags::nsw;

  return mlir::LLVM::IntegerOverflowFlags::none;
}

static bool isIntTypeUnsigned(mlir::Type type) {
  // TODO: Ideally, we should only need to check cir::IntType here.
  return mlir::isa<cir::IntType>(type)
             ? mlir::cast<cir::IntType>(type).isUnsigned()
             : mlir::cast<mlir::IntegerType>(type).isUnsigned();
}

mlir::LogicalResult CIRToLLVMBinOpLowering::matchAndRewrite(
    cir::BinOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  if (adaptor.getLhs().getType() != adaptor.getRhs().getType())
    return op.emitError() << "inconsistent operands' types not supported yet";

  mlir::Type type = op.getRhs().getType();
  if (!mlir::isa<cir::IntType, cir::BoolType, cir::FPTypeInterface,
                 mlir::IntegerType, cir::VectorType>(type))
    return op.emitError() << "operand type not supported yet";

  const mlir::Type llvmTy = getTypeConverter()->convertType(op.getType());
  const mlir::Type llvmEltTy = elementTypeIfVector(llvmTy);

  const mlir::Value rhs = adaptor.getRhs();
  const mlir::Value lhs = adaptor.getLhs();
  type = elementTypeIfVector(type);

  switch (op.getKind()) {
  case cir::BinOpKind::Add:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy)) {
      if (op.getSaturated()) {
        if (isIntTypeUnsigned(type)) {
          rewriter.replaceOpWithNewOp<mlir::LLVM::UAddSat>(op, lhs, rhs);
          break;
        }
        rewriter.replaceOpWithNewOp<mlir::LLVM::SAddSat>(op, lhs, rhs);
        break;
      }
      rewriter.replaceOpWithNewOp<mlir::LLVM::AddOp>(op, llvmTy, lhs, rhs,
                                                     getIntOverflowFlag(op));
    } else {
      rewriter.replaceOpWithNewOp<mlir::LLVM::FAddOp>(op, lhs, rhs);
    }
    break;
  case cir::BinOpKind::Sub:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy)) {
      if (op.getSaturated()) {
        if (isIntTypeUnsigned(type)) {
          rewriter.replaceOpWithNewOp<mlir::LLVM::USubSat>(op, lhs, rhs);
          break;
        }
        rewriter.replaceOpWithNewOp<mlir::LLVM::SSubSat>(op, lhs, rhs);
        break;
      }
      rewriter.replaceOpWithNewOp<mlir::LLVM::SubOp>(op, llvmTy, lhs, rhs,
                                                     getIntOverflowFlag(op));
    } else {
      rewriter.replaceOpWithNewOp<mlir::LLVM::FSubOp>(op, lhs, rhs);
    }
    break;
  case cir::BinOpKind::Mul:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy))
      rewriter.replaceOpWithNewOp<mlir::LLVM::MulOp>(op, llvmTy, lhs, rhs,
                                                     getIntOverflowFlag(op));
    else
      rewriter.replaceOpWithNewOp<mlir::LLVM::FMulOp>(op, lhs, rhs);
    break;
  case cir::BinOpKind::Div:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy)) {
      auto isUnsigned = isIntTypeUnsigned(type);
      if (isUnsigned)
        rewriter.replaceOpWithNewOp<mlir::LLVM::UDivOp>(op, lhs, rhs);
      else
        rewriter.replaceOpWithNewOp<mlir::LLVM::SDivOp>(op, lhs, rhs);
    } else {
      rewriter.replaceOpWithNewOp<mlir::LLVM::FDivOp>(op, lhs, rhs);
    }
    break;
  case cir::BinOpKind::Rem:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy)) {
      auto isUnsigned = isIntTypeUnsigned(type);
      if (isUnsigned)
        rewriter.replaceOpWithNewOp<mlir::LLVM::URemOp>(op, lhs, rhs);
      else
        rewriter.replaceOpWithNewOp<mlir::LLVM::SRemOp>(op, lhs, rhs);
    } else {
      rewriter.replaceOpWithNewOp<mlir::LLVM::FRemOp>(op, lhs, rhs);
    }
    break;
  case cir::BinOpKind::And:
    rewriter.replaceOpWithNewOp<mlir::LLVM::AndOp>(op, lhs, rhs);
    break;
  case cir::BinOpKind::Or:
    rewriter.replaceOpWithNewOp<mlir::LLVM::OrOp>(op, lhs, rhs);
    break;
  case cir::BinOpKind::Xor:
    rewriter.replaceOpWithNewOp<mlir::LLVM::XOrOp>(op, lhs, rhs);
    break;
  case cir::BinOpKind::Max:
    if (mlir::isa<mlir::IntegerType>(llvmEltTy)) {
      auto isUnsigned = isIntTypeUnsigned(type);
      if (isUnsigned)
        rewriter.replaceOpWithNewOp<mlir::LLVM::UMaxOp>(op, llvmTy, lhs, rhs);
      else
        rewriter.replaceOpWithNewOp<mlir::LLVM::SMaxOp>(op, llvmTy, lhs, rhs);
    }
    break;
  }
  return mlir::LogicalResult::success();
}

/// Convert from a CIR comparison kind to an LLVM IR integral comparison kind.
static mlir::LLVM::ICmpPredicate
convertCmpKindToICmpPredicate(cir::CmpOpKind kind, bool isSigned) {
  using CIR = cir::CmpOpKind;
  using LLVMICmp = mlir::LLVM::ICmpPredicate;
  switch (kind) {
  case CIR::eq:
    return LLVMICmp::eq;
  case CIR::ne:
    return LLVMICmp::ne;
  case CIR::lt:
    return (isSigned ? LLVMICmp::slt : LLVMICmp::ult);
  case CIR::le:
    return (isSigned ? LLVMICmp::sle : LLVMICmp::ule);
  case CIR::gt:
    return (isSigned ? LLVMICmp::sgt : LLVMICmp::ugt);
  case CIR::ge:
    return (isSigned ? LLVMICmp::sge : LLVMICmp::uge);
  }
  llvm_unreachable("Unknown CmpOpKind");
}

/// Convert from a CIR comparison kind to an LLVM IR floating-point comparison
/// kind.
static mlir::LLVM::FCmpPredicate
convertCmpKindToFCmpPredicate(cir::CmpOpKind kind) {
  using CIR = cir::CmpOpKind;
  using LLVMFCmp = mlir::LLVM::FCmpPredicate;
  switch (kind) {
  case CIR::eq:
    return LLVMFCmp::oeq;
  case CIR::ne:
    return LLVMFCmp::une;
  case CIR::lt:
    return LLVMFCmp::olt;
  case CIR::le:
    return LLVMFCmp::ole;
  case CIR::gt:
    return LLVMFCmp::ogt;
  case CIR::ge:
    return LLVMFCmp::oge;
  }
  llvm_unreachable("Unknown CmpOpKind");
}

mlir::LogicalResult CIRToLLVMCmpOpLowering::matchAndRewrite(
    cir::CmpOp cmpOp, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type type = cmpOp.getLhs().getType();

  assert(!cir::MissingFeatures::dataMemberType());
  assert(!cir::MissingFeatures::methodType());

  if (mlir::isa<cir::IntType, mlir::IntegerType>(type)) {
    bool isSigned = mlir::isa<cir::IntType>(type)
                        ? mlir::cast<cir::IntType>(type).isSigned()
                        : mlir::cast<mlir::IntegerType>(type).isSigned();
    mlir::LLVM::ICmpPredicate kind =
        convertCmpKindToICmpPredicate(cmpOp.getKind(), isSigned);
    rewriter.replaceOpWithNewOp<mlir::LLVM::ICmpOp>(
        cmpOp, kind, adaptor.getLhs(), adaptor.getRhs());
    return mlir::success();
  }

  if (auto ptrTy = mlir::dyn_cast<cir::PointerType>(type)) {
    mlir::LLVM::ICmpPredicate kind =
        convertCmpKindToICmpPredicate(cmpOp.getKind(),
                                      /* isSigned=*/false);
    rewriter.replaceOpWithNewOp<mlir::LLVM::ICmpOp>(
        cmpOp, kind, adaptor.getLhs(), adaptor.getRhs());
    return mlir::success();
  }

  if (mlir::isa<cir::FPTypeInterface>(type)) {
    mlir::LLVM::FCmpPredicate kind =
        convertCmpKindToFCmpPredicate(cmpOp.getKind());
    rewriter.replaceOpWithNewOp<mlir::LLVM::FCmpOp>(
        cmpOp, kind, adaptor.getLhs(), adaptor.getRhs());
    return mlir::success();
  }

  if (mlir::isa<cir::ComplexType>(type)) {
    mlir::Value lhs = adaptor.getLhs();
    mlir::Value rhs = adaptor.getRhs();
    mlir::Location loc = cmpOp.getLoc();

    auto complexType = mlir::cast<cir::ComplexType>(cmpOp.getLhs().getType());
    mlir::Type complexElemTy =
        getTypeConverter()->convertType(complexType.getElementType());

    auto lhsReal =
        rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 0);
    auto lhsImag =
        rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 1);
    auto rhsReal =
        rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 0);
    auto rhsImag =
        rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 1);

    if (cmpOp.getKind() == cir::CmpOpKind::eq) {
      if (complexElemTy.isInteger()) {
        auto realCmp = rewriter.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::eq, lhsReal, rhsReal);
        auto imagCmp = rewriter.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::eq, lhsImag, rhsImag);
        rewriter.replaceOpWithNewOp<mlir::LLVM::AndOp>(cmpOp, realCmp, imagCmp);
        return mlir::success();
      }

      auto realCmp = rewriter.create<mlir::LLVM::FCmpOp>(
          loc, mlir::LLVM::FCmpPredicate::oeq, lhsReal, rhsReal);
      auto imagCmp = rewriter.create<mlir::LLVM::FCmpOp>(
          loc, mlir::LLVM::FCmpPredicate::oeq, lhsImag, rhsImag);
      rewriter.replaceOpWithNewOp<mlir::LLVM::AndOp>(cmpOp, realCmp, imagCmp);
      return mlir::success();
    }

    if (cmpOp.getKind() == cir::CmpOpKind::ne) {
      if (complexElemTy.isInteger()) {
        auto realCmp = rewriter.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::ne, lhsReal, rhsReal);
        auto imagCmp = rewriter.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::ne, lhsImag, rhsImag);
        rewriter.replaceOpWithNewOp<mlir::LLVM::OrOp>(cmpOp, realCmp, imagCmp);
        return mlir::success();
      }

      auto realCmp = rewriter.create<mlir::LLVM::FCmpOp>(
          loc, mlir::LLVM::FCmpPredicate::une, lhsReal, rhsReal);
      auto imagCmp = rewriter.create<mlir::LLVM::FCmpOp>(
          loc, mlir::LLVM::FCmpPredicate::une, lhsImag, rhsImag);
      rewriter.replaceOpWithNewOp<mlir::LLVM::OrOp>(cmpOp, realCmp, imagCmp);
      return mlir::success();
    }
  }

  return cmpOp.emitError() << "unsupported type for CmpOp: " << type;
}

mlir::LogicalResult CIRToLLVMShiftOpLowering::matchAndRewrite(
    cir::ShiftOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  assert((op.getValue().getType() == op.getType()) &&
         "inconsistent operands' types NYI");

  const mlir::Type llvmTy = getTypeConverter()->convertType(op.getType());
  mlir::Value amt = adaptor.getAmount();
  mlir::Value val = adaptor.getValue();

  auto cirAmtTy = mlir::dyn_cast<cir::IntType>(op.getAmount().getType());
  bool isUnsigned;
  if (cirAmtTy) {
    auto cirValTy = mlir::cast<cir::IntType>(op.getValue().getType());
    isUnsigned = cirValTy.isUnsigned();

    // Ensure shift amount is the same type as the value. Some undefined
    // behavior might occur in the casts below as per [C99 6.5.7.3].
    // Vector type shift amount needs no cast as type consistency is expected to
    // be already be enforced at CIRGen.
    if (cirAmtTy)
      amt = getLLVMIntCast(rewriter, amt, llvmTy, true, cirAmtTy.getWidth(),
                           cirValTy.getWidth());
  } else {
    auto cirValVTy = mlir::cast<cir::VectorType>(op.getValue().getType());
    isUnsigned =
        mlir::cast<cir::IntType>(cirValVTy.getElementType()).isUnsigned();
  }

  // Lower to the proper LLVM shift operation.
  if (op.getIsShiftleft()) {
    rewriter.replaceOpWithNewOp<mlir::LLVM::ShlOp>(op, llvmTy, val, amt);
    return mlir::success();
  }

  if (isUnsigned)
    rewriter.replaceOpWithNewOp<mlir::LLVM::LShrOp>(op, llvmTy, val, amt);
  else
    rewriter.replaceOpWithNewOp<mlir::LLVM::AShrOp>(op, llvmTy, val, amt);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMSelectOpLowering::matchAndRewrite(
    cir::SelectOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto getConstantBool = [](mlir::Value value) -> cir::BoolAttr {
    auto definingOp =
        mlir::dyn_cast_if_present<cir::ConstantOp>(value.getDefiningOp());
    if (!definingOp)
      return {};

    auto constValue = mlir::dyn_cast<cir::BoolAttr>(definingOp.getValue());
    if (!constValue)
      return {};

    return constValue;
  };

  // Two special cases in the LLVMIR codegen of select op:
  // - select %0, %1, false => and %0, %1
  // - select %0, true, %1 => or %0, %1
  if (mlir::isa<cir::BoolType>(op.getTrueValue().getType())) {
    cir::BoolAttr trueValue = getConstantBool(op.getTrueValue());
    cir::BoolAttr falseValue = getConstantBool(op.getFalseValue());
    if (falseValue && !falseValue.getValue()) {
      // select %0, %1, false => and %0, %1
      rewriter.replaceOpWithNewOp<mlir::LLVM::AndOp>(op, adaptor.getCondition(),
                                                     adaptor.getTrueValue());
      return mlir::success();
    }
    if (trueValue && trueValue.getValue()) {
      // select %0, true, %1 => or %0, %1
      rewriter.replaceOpWithNewOp<mlir::LLVM::OrOp>(op, adaptor.getCondition(),
                                                    adaptor.getFalseValue());
      return mlir::success();
    }
  }

  mlir::Value llvmCondition = adaptor.getCondition();
  rewriter.replaceOpWithNewOp<mlir::LLVM::SelectOp>(
      op, llvmCondition, adaptor.getTrueValue(), adaptor.getFalseValue());

  return mlir::success();
}

static void prepareTypeConverter(mlir::LLVMTypeConverter &converter,
                                 mlir::DataLayout &dataLayout) {
  converter.addConversion([&](cir::PointerType type) -> mlir::Type {
    // Drop pointee type since LLVM dialect only allows opaque pointers.
    assert(!cir::MissingFeatures::addressSpace());
    unsigned targetAS = 0;

    return mlir::LLVM::LLVMPointerType::get(type.getContext(), targetAS);
  });
  converter.addConversion([&](cir::ArrayType type) -> mlir::Type {
    mlir::Type ty =
        convertTypeForMemory(converter, dataLayout, type.getElementType());
    return mlir::LLVM::LLVMArrayType::get(ty, type.getSize());
  });
  converter.addConversion([&](cir::VectorType type) -> mlir::Type {
    const mlir::Type ty = converter.convertType(type.getElementType());
    return mlir::VectorType::get(type.getSize(), ty);
  });
  converter.addConversion([&](cir::BoolType type) -> mlir::Type {
    return mlir::IntegerType::get(type.getContext(), 1,
                                  mlir::IntegerType::Signless);
  });
  converter.addConversion([&](cir::IntType type) -> mlir::Type {
    // LLVM doesn't work with signed types, so we drop the CIR signs here.
    return mlir::IntegerType::get(type.getContext(), type.getWidth());
  });
  converter.addConversion([&](cir::SingleType type) -> mlir::Type {
    return mlir::Float32Type::get(type.getContext());
  });
  converter.addConversion([&](cir::DoubleType type) -> mlir::Type {
    return mlir::Float64Type::get(type.getContext());
  });
  converter.addConversion([&](cir::FP80Type type) -> mlir::Type {
    return mlir::Float80Type::get(type.getContext());
  });
  converter.addConversion([&](cir::FP128Type type) -> mlir::Type {
    return mlir::Float128Type::get(type.getContext());
  });
  converter.addConversion([&](cir::LongDoubleType type) -> mlir::Type {
    return converter.convertType(type.getUnderlying());
  });
  converter.addConversion([&](cir::FP16Type type) -> mlir::Type {
    return mlir::Float16Type::get(type.getContext());
  });
  converter.addConversion([&](cir::BF16Type type) -> mlir::Type {
    return mlir::BFloat16Type::get(type.getContext());
  });
  converter.addConversion([&](cir::ComplexType type) -> mlir::Type {
    // A complex type is lowered to an LLVM struct that contains the real and
    // imaginary part as data fields.
    mlir::Type elementTy = converter.convertType(type.getElementType());
    mlir::Type structFields[2] = {elementTy, elementTy};
    return mlir::LLVM::LLVMStructType::getLiteral(type.getContext(),
                                                  structFields);
  });
  converter.addConversion([&](cir::FuncType type) -> std::optional<mlir::Type> {
    auto result = converter.convertType(type.getReturnType());
    llvm::SmallVector<mlir::Type> arguments;
    arguments.reserve(type.getNumInputs());
    if (converter.convertTypes(type.getInputs(), arguments).failed())
      return std::nullopt;
    auto varArg = type.isVarArg();
    return mlir::LLVM::LLVMFunctionType::get(result, arguments, varArg);
  });
  converter.addConversion([&](cir::RecordType type) -> mlir::Type {
    // Convert struct members.
    llvm::SmallVector<mlir::Type> llvmMembers;
    switch (type.getKind()) {
    case cir::RecordType::Class:
    case cir::RecordType::Struct:
      for (mlir::Type ty : type.getMembers())
        llvmMembers.push_back(convertTypeForMemory(converter, dataLayout, ty));
      break;
    // Unions are lowered as only the largest member.
    case cir::RecordType::Union:
      if (auto largestMember = type.getLargestMember(dataLayout))
        llvmMembers.push_back(
            convertTypeForMemory(converter, dataLayout, largestMember));
      if (type.getPadded()) {
        auto last = *type.getMembers().rbegin();
        llvmMembers.push_back(
            convertTypeForMemory(converter, dataLayout, last));
      }
      break;
    }

    // Record has a name: lower as an identified record.
    mlir::LLVM::LLVMStructType llvmStruct;
    if (type.getName()) {
      llvmStruct = mlir::LLVM::LLVMStructType::getIdentified(
          type.getContext(), type.getPrefixedName());
      if (llvmStruct.setBody(llvmMembers, type.getPacked()).failed())
        llvm_unreachable("Failed to set body of record");
    } else { // Record has no name: lower as literal record.
      llvmStruct = mlir::LLVM::LLVMStructType::getLiteral(
          type.getContext(), llvmMembers, type.getPacked());
    }

    return llvmStruct;
  });
}

// The applyPartialConversion function traverses blocks in the dominance order,
// so it does not lower and operations that are not reachachable from the
// operations passed in as arguments. Since we do need to lower such code in
// order to avoid verification errors occur, we cannot just pass the module op
// to applyPartialConversion. We must build a set of unreachable ops and
// explicitly add them, along with the module, to the vector we pass to
// applyPartialConversion.
//
// For instance, this CIR code:
//
//    cir.func @foo(%arg0: !s32i) -> !s32i {
//      %4 = cir.cast(int_to_bool, %arg0 : !s32i), !cir.bool
//      cir.if %4 {
//        %5 = cir.const #cir.int<1> : !s32i
//        cir.return %5 : !s32i
//      } else {
//        %5 = cir.const #cir.int<0> : !s32i
//       cir.return %5 : !s32i
//      }
//      cir.return %arg0 : !s32i
//    }
//
// contains an unreachable return operation (the last one). After the flattening
// pass it will be placed into the unreachable block. The possible error
// after the lowering pass is: error: 'cir.return' op expects parent op to be
// one of 'cir.func, cir.scope, cir.if ... The reason that this operation was
// not lowered and the new parent is llvm.func.
//
// In the future we may want to get rid of this function and use a DCE pass or
// something similar. But for now we need to guarantee the absence of the
// dialect verification errors.
static void collectUnreachable(mlir::Operation *parent,
                               llvm::SmallVector<mlir::Operation *> &ops) {

  llvm::SmallVector<mlir::Block *> unreachableBlocks;
  parent->walk([&](mlir::Block *blk) { // check
    if (blk->hasNoPredecessors() && !blk->isEntryBlock())
      unreachableBlocks.push_back(blk);
  });

  std::set<mlir::Block *> visited;
  for (mlir::Block *root : unreachableBlocks) {
    // We create a work list for each unreachable block.
    // Thus we traverse operations in some order.
    std::deque<mlir::Block *> workList;
    workList.push_back(root);

    while (!workList.empty()) {
      mlir::Block *blk = workList.back();
      workList.pop_back();
      if (visited.count(blk))
        continue;
      visited.emplace(blk);

      for (mlir::Operation &op : *blk)
        ops.push_back(&op);

      for (mlir::Block *succ : blk->getSuccessors())
        workList.push_back(succ);
    }
  }
}

void ConvertCIRToLLVMPass::processCIRAttrs(mlir::ModuleOp module) {
  // Lower the module attributes to LLVM equivalents.
  if (mlir::Attribute tripleAttr =
          module->getAttr(cir::CIRDialect::getTripleAttrName()))
    module->setAttr(mlir::LLVM::LLVMDialect::getTargetTripleAttrName(),
                    tripleAttr);
}

void ConvertCIRToLLVMPass::runOnOperation() {
  llvm::TimeTraceScope scope("Convert CIR to LLVM Pass");

  mlir::ModuleOp module = getOperation();
  mlir::DataLayout dl(module);
  mlir::LLVMTypeConverter converter(&getContext());
  prepareTypeConverter(converter, dl);

  mlir::RewritePatternSet patterns(&getContext());

  patterns.add<CIRToLLVMReturnOpLowering>(patterns.getContext());
  // This could currently be merged with the group below, but it will get more
  // arguments later, so we'll keep it separate for now.
  patterns.add<CIRToLLVMAllocaOpLowering>(converter, patterns.getContext(), dl);
  patterns.add<CIRToLLVMLoadOpLowering>(converter, patterns.getContext(), dl);
  patterns.add<CIRToLLVMStoreOpLowering>(converter, patterns.getContext(), dl);
  patterns.add<CIRToLLVMGlobalOpLowering>(converter, patterns.getContext(), dl);
  patterns.add<CIRToLLVMCastOpLowering>(converter, patterns.getContext(), dl);
  patterns.add<CIRToLLVMPtrStrideOpLowering>(converter, patterns.getContext(),
                                             dl);
  patterns.add<
      // clang-format off
               CIRToLLVMAssumeOpLowering,
               CIRToLLVMBaseClassAddrOpLowering,
               CIRToLLVMBinOpLowering,
               CIRToLLVMBitClrsbOpLowering,
               CIRToLLVMBitClzOpLowering,
               CIRToLLVMBitCtzOpLowering,
               CIRToLLVMBitParityOpLowering,
               CIRToLLVMBitPopcountOpLowering,
               CIRToLLVMBitReverseOpLowering,
               CIRToLLVMBrCondOpLowering,
               CIRToLLVMBrOpLowering,
               CIRToLLVMByteSwapOpLowering,
               CIRToLLVMCallOpLowering,
               CIRToLLVMCmpOpLowering,
               CIRToLLVMComplexAddOpLowering,
               CIRToLLVMComplexCreateOpLowering,
               CIRToLLVMComplexImagOpLowering,
               CIRToLLVMComplexRealOpLowering,
               CIRToLLVMComplexRealPtrOpLowering,
               CIRToLLVMComplexSubOpLowering,
               CIRToLLVMConstantOpLowering,
               CIRToLLVMExpectOpLowering,
               CIRToLLVMFuncOpLowering,
               CIRToLLVMGetBitfieldOpLowering,
               CIRToLLVMGetGlobalOpLowering,
               CIRToLLVMGetMemberOpLowering,
               CIRToLLVMSelectOpLowering,
               CIRToLLVMSetBitfieldOpLowering,
               CIRToLLVMShiftOpLowering,
               CIRToLLVMStackRestoreOpLowering,
               CIRToLLVMStackSaveOpLowering,
               CIRToLLVMSwitchFlatOpLowering,
               CIRToLLVMTrapOpLowering,
               CIRToLLVMUnaryOpLowering,
               CIRToLLVMVecCmpOpLowering,
               CIRToLLVMVecCreateOpLowering,
               CIRToLLVMVecExtractOpLowering,
               CIRToLLVMVecInsertOpLowering,
               CIRToLLVMVecShuffleDynamicOpLowering,
               CIRToLLVMVecShuffleOpLowering,
               CIRToLLVMVecSplatOpLowering,
               CIRToLLVMVecTernaryOpLowering
      // clang-format on
      >(converter, patterns.getContext());

  processCIRAttrs(module);

  mlir::ConversionTarget target(getContext());
  target.addLegalOp<mlir::ModuleOp>();
  target.addLegalDialect<mlir::LLVM::LLVMDialect>();
  target.addIllegalDialect<mlir::BuiltinDialect, cir::CIRDialect,
                           mlir::func::FuncDialect>();

  llvm::SmallVector<mlir::Operation *> ops;
  ops.push_back(module);
  collectUnreachable(module, ops);

  if (failed(applyPartialConversion(ops, target, std::move(patterns))))
    signalPassFailure();
}

mlir::LogicalResult CIRToLLVMBrOpLowering::matchAndRewrite(
    cir::BrOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::BrOp>(op, adaptor.getOperands(),
                                                op.getDest());
  return mlir::LogicalResult::success();
}

mlir::LogicalResult CIRToLLVMGetMemberOpLowering::matchAndRewrite(
    cir::GetMemberOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type llResTy = getTypeConverter()->convertType(op.getType());
  const auto recordTy =
      mlir::cast<cir::RecordType>(op.getAddrTy().getPointee());
  assert(recordTy && "expected record type");

  switch (recordTy.getKind()) {
  case cir::RecordType::Class:
  case cir::RecordType::Struct: {
    // Since the base address is a pointer to an aggregate, the first offset
    // is always zero. The second offset tell us which member it will access.
    llvm::SmallVector<mlir::LLVM::GEPArg, 2> offset{0, op.getIndex()};
    const mlir::Type elementTy = getTypeConverter()->convertType(recordTy);
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(op, llResTy, elementTy,
                                                   adaptor.getAddr(), offset);
    return mlir::success();
  }
  case cir::RecordType::Union:
    // Union members share the address space, so we just need a bitcast to
    // conform to type-checking.
    rewriter.replaceOpWithNewOp<mlir::LLVM::BitcastOp>(op, llResTy,
                                                       adaptor.getAddr());
    return mlir::success();
  }
}

mlir::LogicalResult CIRToLLVMTrapOpLowering::matchAndRewrite(
    cir::TrapOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Location loc = op->getLoc();
  rewriter.eraseOp(op);

  rewriter.create<mlir::LLVM::Trap>(loc);

  // Note that the call to llvm.trap is not a terminator in LLVM dialect.
  // So we must emit an additional llvm.unreachable to terminate the current
  // block.
  rewriter.create<mlir::LLVM::UnreachableOp>(loc);

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMStackSaveOpLowering::matchAndRewrite(
    cir::StackSaveOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  const mlir::Type ptrTy = getTypeConverter()->convertType(op.getType());
  rewriter.replaceOpWithNewOp<mlir::LLVM::StackSaveOp>(op, ptrTy);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMStackRestoreOpLowering::matchAndRewrite(
    cir::StackRestoreOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::StackRestoreOp>(op, adaptor.getPtr());
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecCreateOpLowering::matchAndRewrite(
    cir::VecCreateOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // Start with an 'undef' value for the vector.  Then 'insertelement' for
  // each of the vector elements.
  const auto vecTy = mlir::cast<cir::VectorType>(op.getType());
  const mlir::Type llvmTy = typeConverter->convertType(vecTy);
  const mlir::Location loc = op.getLoc();
  mlir::Value result = rewriter.create<mlir::LLVM::PoisonOp>(loc, llvmTy);
  assert(vecTy.getSize() == op.getElements().size() &&
         "cir.vec.create op count doesn't match vector type elements count");

  for (uint64_t i = 0; i < vecTy.getSize(); ++i) {
    const mlir::Value indexValue =
        rewriter.create<mlir::LLVM::ConstantOp>(loc, rewriter.getI64Type(), i);
    result = rewriter.create<mlir::LLVM::InsertElementOp>(
        loc, result, adaptor.getElements()[i], indexValue);
  }

  rewriter.replaceOp(op, result);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecExtractOpLowering::matchAndRewrite(
    cir::VecExtractOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::ExtractElementOp>(
      op, adaptor.getVec(), adaptor.getIndex());
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecInsertOpLowering::matchAndRewrite(
    cir::VecInsertOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::LLVM::InsertElementOp>(
      op, adaptor.getVec(), adaptor.getValue(), adaptor.getIndex());
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecCmpOpLowering::matchAndRewrite(
    cir::VecCmpOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type elementType = elementTypeIfVector(op.getLhs().getType());
  mlir::Value bitResult;
  if (auto intType = mlir::dyn_cast<cir::IntType>(elementType)) {
    bitResult = rewriter.create<mlir::LLVM::ICmpOp>(
        op.getLoc(),
        convertCmpKindToICmpPredicate(op.getKind(), intType.isSigned()),
        adaptor.getLhs(), adaptor.getRhs());
  } else if (mlir::isa<cir::FPTypeInterface>(elementType)) {
    bitResult = rewriter.create<mlir::LLVM::FCmpOp>(
        op.getLoc(), convertCmpKindToFCmpPredicate(op.getKind()),
        adaptor.getLhs(), adaptor.getRhs());
  } else {
    return op.emitError() << "unsupported type for VecCmpOp: " << elementType;
  }

  // LLVM IR vector comparison returns a vector of i1. This one-bit vector
  // must be sign-extended to the correct result type.
  rewriter.replaceOpWithNewOp<mlir::LLVM::SExtOp>(
      op, typeConverter->convertType(op.getType()), bitResult);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecSplatOpLowering::matchAndRewrite(
    cir::VecSplatOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // Vector splat can be implemented with an `insertelement` and a
  // `shufflevector`, which is better than an `insertelement` for each
  // element in the vector. Start with an undef vector. Insert the value into
  // the first element. Then use a `shufflevector` with a mask of all 0 to
  // fill out the entire vector with that value.
  cir::VectorType vecTy = op.getType();
  mlir::Type llvmTy = typeConverter->convertType(vecTy);
  mlir::Location loc = op.getLoc();
  mlir::Value poison = rewriter.create<mlir::LLVM::PoisonOp>(loc, llvmTy);

  mlir::Value elementValue = adaptor.getValue();
  if (mlir::isa<mlir::LLVM::PoisonOp>(elementValue.getDefiningOp())) {
    // If the splat value is poison, then we can just use poison value
    // for the entire vector.
    rewriter.replaceOp(op, poison);
    return mlir::success();
  }

  if (auto constValue =
          dyn_cast<mlir::LLVM::ConstantOp>(elementValue.getDefiningOp())) {
    if (auto intAttr = dyn_cast<mlir::IntegerAttr>(constValue.getValue())) {
      mlir::DenseIntElementsAttr denseVec = mlir::DenseIntElementsAttr::get(
          mlir::cast<mlir::ShapedType>(llvmTy), intAttr.getValue());
      rewriter.replaceOpWithNewOp<mlir::LLVM::ConstantOp>(
          op, denseVec.getType(), denseVec);
      return mlir::success();
    }

    if (auto fpAttr = dyn_cast<mlir::FloatAttr>(constValue.getValue())) {
      mlir::DenseFPElementsAttr denseVec = mlir::DenseFPElementsAttr::get(
          mlir::cast<mlir::ShapedType>(llvmTy), fpAttr.getValue());
      rewriter.replaceOpWithNewOp<mlir::LLVM::ConstantOp>(
          op, denseVec.getType(), denseVec);
      return mlir::success();
    }
  }

  mlir::Value indexValue =
      rewriter.create<mlir::LLVM::ConstantOp>(loc, rewriter.getI64Type(), 0);
  mlir::Value oneElement = rewriter.create<mlir::LLVM::InsertElementOp>(
      loc, poison, elementValue, indexValue);
  SmallVector<int32_t> zeroValues(vecTy.getSize(), 0);
  rewriter.replaceOpWithNewOp<mlir::LLVM::ShuffleVectorOp>(op, oneElement,
                                                           poison, zeroValues);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecShuffleOpLowering::matchAndRewrite(
    cir::VecShuffleOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // LLVM::ShuffleVectorOp takes an ArrayRef of int for the list of indices.
  // Convert the ClangIR ArrayAttr of IntAttr constants into a
  // SmallVector<int>.
  SmallVector<int, 8> indices;
  std::transform(
      op.getIndices().begin(), op.getIndices().end(),
      std::back_inserter(indices), [](mlir::Attribute intAttr) {
        return mlir::cast<cir::IntAttr>(intAttr).getValue().getSExtValue();
      });
  rewriter.replaceOpWithNewOp<mlir::LLVM::ShuffleVectorOp>(
      op, adaptor.getVec1(), adaptor.getVec2(), indices);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecShuffleDynamicOpLowering::matchAndRewrite(
    cir::VecShuffleDynamicOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // LLVM IR does not have an operation that corresponds to this form of
  // the built-in.
  //     __builtin_shufflevector(V, I)
  // is implemented as this pseudocode, where the for loop is unrolled
  // and N is the number of elements:
  //
  // result = undef
  // maskbits = NextPowerOf2(N - 1)
  // masked = I & maskbits
  // for (i in 0 <= i < N)
  //    result[i] = V[masked[i]]
  mlir::Location loc = op.getLoc();
  mlir::Value input = adaptor.getVec();
  mlir::Type llvmIndexVecType =
      getTypeConverter()->convertType(op.getIndices().getType());
  mlir::Type llvmIndexType = getTypeConverter()->convertType(
      elementTypeIfVector(op.getIndices().getType()));
  uint64_t numElements =
      mlir::cast<cir::VectorType>(op.getVec().getType()).getSize();

  uint64_t maskBits = llvm::NextPowerOf2(numElements - 1) - 1;
  mlir::Value maskValue = rewriter.create<mlir::LLVM::ConstantOp>(
      loc, llvmIndexType, rewriter.getIntegerAttr(llvmIndexType, maskBits));
  mlir::Value maskVector =
      rewriter.create<mlir::LLVM::UndefOp>(loc, llvmIndexVecType);

  for (uint64_t i = 0; i < numElements; ++i) {
    mlir::Value idxValue =
        rewriter.create<mlir::LLVM::ConstantOp>(loc, rewriter.getI64Type(), i);
    maskVector = rewriter.create<mlir::LLVM::InsertElementOp>(
        loc, maskVector, maskValue, idxValue);
  }

  mlir::Value maskedIndices = rewriter.create<mlir::LLVM::AndOp>(
      loc, llvmIndexVecType, adaptor.getIndices(), maskVector);
  mlir::Value result = rewriter.create<mlir::LLVM::UndefOp>(
      loc, getTypeConverter()->convertType(op.getVec().getType()));
  for (uint64_t i = 0; i < numElements; ++i) {
    mlir::Value iValue =
        rewriter.create<mlir::LLVM::ConstantOp>(loc, rewriter.getI64Type(), i);
    mlir::Value indexValue = rewriter.create<mlir::LLVM::ExtractElementOp>(
        loc, maskedIndices, iValue);
    mlir::Value valueAtIndex =
        rewriter.create<mlir::LLVM::ExtractElementOp>(loc, input, indexValue);
    result = rewriter.create<mlir::LLVM::InsertElementOp>(loc, result,
                                                          valueAtIndex, iValue);
  }
  rewriter.replaceOp(op, result);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMVecTernaryOpLowering::matchAndRewrite(
    cir::VecTernaryOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  // Convert `cond` into a vector of i1, then use that in a `select` op.
  mlir::Value bitVec = rewriter.create<mlir::LLVM::ICmpOp>(
      op.getLoc(), mlir::LLVM::ICmpPredicate::ne, adaptor.getCond(),
      rewriter.create<mlir::LLVM::ZeroOp>(
          op.getCond().getLoc(),
          typeConverter->convertType(op.getCond().getType())));
  rewriter.replaceOpWithNewOp<mlir::LLVM::SelectOp>(
      op, bitVec, adaptor.getLhs(), adaptor.getRhs());
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexAddOpLowering::matchAndRewrite(
    cir::ComplexAddOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Value lhs = adaptor.getLhs();
  mlir::Value rhs = adaptor.getRhs();
  mlir::Location loc = op.getLoc();

  auto complexType = mlir::cast<cir::ComplexType>(op.getLhs().getType());
  mlir::Type complexElemTy =
      getTypeConverter()->convertType(complexType.getElementType());
  auto lhsReal =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 0);
  auto lhsImag =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 1);
  auto rhsReal =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 0);
  auto rhsImag =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 1);

  mlir::Value newReal;
  mlir::Value newImag;
  if (complexElemTy.isInteger()) {
    newReal = rewriter.create<mlir::LLVM::AddOp>(loc, complexElemTy, lhsReal,
                                                 rhsReal);
    newImag = rewriter.create<mlir::LLVM::AddOp>(loc, complexElemTy, lhsImag,
                                                 rhsImag);
  } else {
    assert(!cir::MissingFeatures::fastMathFlags());
    assert(!cir::MissingFeatures::fpConstraints());
    newReal = rewriter.create<mlir::LLVM::FAddOp>(loc, complexElemTy, lhsReal,
                                                  rhsReal);
    newImag = rewriter.create<mlir::LLVM::FAddOp>(loc, complexElemTy, lhsImag,
                                                  rhsImag);
  }

  mlir::Type complexLLVMTy =
      getTypeConverter()->convertType(op.getResult().getType());
  auto initialComplex =
      rewriter.create<mlir::LLVM::PoisonOp>(op->getLoc(), complexLLVMTy);

  auto realComplex = rewriter.create<mlir::LLVM::InsertValueOp>(
      op->getLoc(), initialComplex, newReal, 0);

  rewriter.replaceOpWithNewOp<mlir::LLVM::InsertValueOp>(op, realComplex,
                                                         newImag, 1);

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexCreateOpLowering::matchAndRewrite(
    cir::ComplexCreateOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type complexLLVMTy =
      getTypeConverter()->convertType(op.getResult().getType());
  auto initialComplex =
      rewriter.create<mlir::LLVM::UndefOp>(op->getLoc(), complexLLVMTy);

  auto realComplex = rewriter.create<mlir::LLVM::InsertValueOp>(
      op->getLoc(), initialComplex, adaptor.getReal(), 0);

  auto complex = rewriter.create<mlir::LLVM::InsertValueOp>(
      op->getLoc(), realComplex, adaptor.getImag(), 1);

  rewriter.replaceOp(op, complex);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexRealOpLowering::matchAndRewrite(
    cir::ComplexRealOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type resultLLVMTy = getTypeConverter()->convertType(op.getType());
  rewriter.replaceOpWithNewOp<mlir::LLVM::ExtractValueOp>(
      op, resultLLVMTy, adaptor.getOperand(), llvm::ArrayRef<std::int64_t>{0});
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexSubOpLowering::matchAndRewrite(
    cir::ComplexSubOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Value lhs = adaptor.getLhs();
  mlir::Value rhs = adaptor.getRhs();
  mlir::Location loc = op.getLoc();

  auto complexType = mlir::cast<cir::ComplexType>(op.getLhs().getType());
  mlir::Type complexElemTy =
      getTypeConverter()->convertType(complexType.getElementType());
  auto lhsReal =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 0);
  auto lhsImag =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, lhs, 1);
  auto rhsReal =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 0);
  auto rhsImag =
      rewriter.create<mlir::LLVM::ExtractValueOp>(loc, complexElemTy, rhs, 1);

  mlir::Value newReal;
  mlir::Value newImag;
  if (complexElemTy.isInteger()) {
    newReal = rewriter.create<mlir::LLVM::SubOp>(loc, complexElemTy, lhsReal,
                                                 rhsReal);
    newImag = rewriter.create<mlir::LLVM::SubOp>(loc, complexElemTy, lhsImag,
                                                 rhsImag);
  } else {
    assert(!cir::MissingFeatures::fastMathFlags());
    assert(!cir::MissingFeatures::fpConstraints());
    newReal = rewriter.create<mlir::LLVM::FSubOp>(loc, complexElemTy, lhsReal,
                                                  rhsReal);
    newImag = rewriter.create<mlir::LLVM::FSubOp>(loc, complexElemTy, lhsImag,
                                                  rhsImag);
  }

  mlir::Type complexLLVMTy =
      getTypeConverter()->convertType(op.getResult().getType());
  auto initialComplex =
      rewriter.create<mlir::LLVM::PoisonOp>(op->getLoc(), complexLLVMTy);

  auto realComplex = rewriter.create<mlir::LLVM::InsertValueOp>(
      op->getLoc(), initialComplex, newReal, 0);

  rewriter.replaceOpWithNewOp<mlir::LLVM::InsertValueOp>(op, realComplex,
                                                         newImag, 1);

  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexImagOpLowering::matchAndRewrite(
    cir::ComplexImagOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::Type resultLLVMTy = getTypeConverter()->convertType(op.getType());
  rewriter.replaceOpWithNewOp<mlir::LLVM::ExtractValueOp>(
      op, resultLLVMTy, adaptor.getOperand(), llvm::ArrayRef<std::int64_t>{1});
  return mlir::success();
}

mlir::IntegerType computeBitfieldIntType(mlir::Type storageType,
                                         mlir::MLIRContext *context,
                                         unsigned &storageSize) {
  return TypeSwitch<mlir::Type, mlir::IntegerType>(storageType)
      .Case<cir::ArrayType>([&](cir::ArrayType atTy) {
        storageSize = atTy.getSize() * 8;
        return mlir::IntegerType::get(context, storageSize);
      })
      .Case<cir::IntType>([&](cir::IntType intTy) {
        storageSize = intTy.getWidth();
        return mlir::IntegerType::get(context, storageSize);
      })
      .Default([](mlir::Type) -> mlir::IntegerType {
        llvm_unreachable(
            "Either ArrayType or IntType expected for bitfields storage");
      });
}

mlir::LogicalResult CIRToLLVMSetBitfieldOpLowering::matchAndRewrite(
    cir::SetBitfieldOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  mlir::OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(op);

  cir::BitfieldInfoAttr info = op.getBitfieldInfo();
  uint64_t size = info.getSize();
  uint64_t offset = info.getOffset();
  mlir::Type storageType = info.getStorageType();
  mlir::MLIRContext *context = storageType.getContext();

  unsigned storageSize = 0;

  mlir::IntegerType intType =
      computeBitfieldIntType(storageType, context, storageSize);

  mlir::Value srcVal = createIntCast(rewriter, adaptor.getSrc(), intType);
  unsigned srcWidth = storageSize;
  mlir::Value resultVal = srcVal;

  if (storageSize != size) {
    assert(storageSize > size && "Invalid bitfield size.");

    mlir::Value val = rewriter.create<mlir::LLVM::LoadOp>(
        op.getLoc(), intType, adaptor.getAddr(), /* alignment */ 0,
        op.getIsVolatile());

    srcVal =
        createAnd(rewriter, srcVal, llvm::APInt::getLowBitsSet(srcWidth, size));
    resultVal = srcVal;
    srcVal = createShL(rewriter, srcVal, offset);

    // Mask out the original value.
    val = createAnd(rewriter, val,
                    ~llvm::APInt::getBitsSet(srcWidth, offset, offset + size));

    // Or together the unchanged values and the source value.
    srcVal = rewriter.create<mlir::LLVM::OrOp>(op.getLoc(), val, srcVal);
  }

  rewriter.create<mlir::LLVM::StoreOp>(op.getLoc(), srcVal, adaptor.getAddr(),
                                       /* alignment */ 0, op.getIsVolatile());

  mlir::Type resultTy = getTypeConverter()->convertType(op.getType());

  if (info.getIsSigned()) {
    assert(size <= storageSize);
    unsigned highBits = storageSize - size;

    if (highBits) {
      resultVal = createShL(rewriter, resultVal, highBits);
      resultVal = createAShR(rewriter, resultVal, highBits);
    }
  }

  resultVal = createIntCast(rewriter, resultVal,
                            mlir::cast<mlir::IntegerType>(resultTy),
                            info.getIsSigned());

  rewriter.replaceOp(op, resultVal);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMComplexRealPtrOpLowering::matchAndRewrite(
    cir::ComplexRealPtrOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {
  cir::PointerType operandTy = op.getOperand().getType();
  mlir::Type resultLLVMTy = getTypeConverter()->convertType(op.getType());
  mlir::Type elementLLVMTy =
      getTypeConverter()->convertType(operandTy.getPointee());

  mlir::LLVM::GEPArg gepIndices[2] = {0, 0};
  mlir::LLVM::GEPNoWrapFlags inboundsNuw =
      mlir::LLVM::GEPNoWrapFlags::inbounds | mlir::LLVM::GEPNoWrapFlags::nuw;
  rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
      op, resultLLVMTy, elementLLVMTy, adaptor.getOperand(), gepIndices,
      inboundsNuw);
  return mlir::success();
}

mlir::LogicalResult CIRToLLVMGetBitfieldOpLowering::matchAndRewrite(
    cir::GetBitfieldOp op, OpAdaptor adaptor,
    mlir::ConversionPatternRewriter &rewriter) const {

  mlir::OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(op);

  cir::BitfieldInfoAttr info = op.getBitfieldInfo();
  uint64_t size = info.getSize();
  uint64_t offset = info.getOffset();
  mlir::Type storageType = info.getStorageType();
  mlir::MLIRContext *context = storageType.getContext();
  unsigned storageSize = 0;

  mlir::IntegerType intType =
      computeBitfieldIntType(storageType, context, storageSize);

  mlir::Value val = rewriter.create<mlir::LLVM::LoadOp>(
      op.getLoc(), intType, adaptor.getAddr(), 0, op.getIsVolatile());
  val = rewriter.create<mlir::LLVM::BitcastOp>(op.getLoc(), intType, val);

  if (info.getIsSigned()) {
    assert(static_cast<unsigned>(offset + size) <= storageSize);
    unsigned highBits = storageSize - offset - size;
    val = createShL(rewriter, val, highBits);
    val = createAShR(rewriter, val, offset + highBits);
  } else {
    val = createLShR(rewriter, val, offset);

    if (static_cast<unsigned>(offset) + size < storageSize)
      val = createAnd(rewriter, val,
                      llvm::APInt::getLowBitsSet(storageSize, size));
  }

  mlir::Type resTy = getTypeConverter()->convertType(op.getType());
  mlir::Value newOp = createIntCast(
      rewriter, val, mlir::cast<mlir::IntegerType>(resTy), info.getIsSigned());
  rewriter.replaceOp(op, newOp);
  return mlir::success();
}

std::unique_ptr<mlir::Pass> createConvertCIRToLLVMPass() {
  return std::make_unique<ConvertCIRToLLVMPass>();
}

void populateCIRToLLVMPasses(mlir::OpPassManager &pm) {
  mlir::populateCIRPreLoweringPasses(pm);
  pm.addPass(createConvertCIRToLLVMPass());
}

std::unique_ptr<llvm::Module>
lowerDirectlyFromCIRToLLVMIR(mlir::ModuleOp mlirModule, LLVMContext &llvmCtx) {
  llvm::TimeTraceScope scope("lower from CIR to LLVM directly");

  mlir::MLIRContext *mlirCtx = mlirModule.getContext();

  mlir::PassManager pm(mlirCtx);
  populateCIRToLLVMPasses(pm);

  (void)mlir::applyPassManagerCLOptions(pm);

  if (mlir::failed(pm.run(mlirModule))) {
    // FIXME: Handle any errors where they occurs and return a nullptr here.
    report_fatal_error(
        "The pass manager failed to lower CIR to LLVMIR dialect!");
  }

  mlir::registerBuiltinDialectTranslation(*mlirCtx);
  mlir::registerLLVMDialectTranslation(*mlirCtx);
  mlir::registerCIRDialectTranslation(*mlirCtx);

  llvm::TimeTraceScope translateScope("translateModuleToLLVMIR");

  StringRef moduleName = mlirModule.getName().value_or("CIRToLLVMModule");
  std::unique_ptr<llvm::Module> llvmModule =
      mlir::translateModuleToLLVMIR(mlirModule, llvmCtx, moduleName);

  if (!llvmModule) {
    // FIXME: Handle any errors where they occurs and return a nullptr here.
    report_fatal_error("Lowering from LLVMIR dialect to llvm IR failed!");
  }

  return llvmModule;
}
} // namespace direct
} // namespace cir
