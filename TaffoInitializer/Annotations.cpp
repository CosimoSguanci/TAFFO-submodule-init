#include <sstream>
#include <iostream>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#include "TaffoInitializerPass.h"
#include "AnnotationParser.h"
#include "Metadata.h"
#include "TypeUtils.h"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace llvm;
using namespace taffo;
using namespace std;
using namespace mdutils;

llvm::cl::opt<int> FracThreshold2("minfractbits2", llvm::cl::value_desc("bits"),
    llvm::cl::desc("Threshold of fractional bits in fixed point numbers"), llvm::cl::init(3));
llvm::cl::opt<int> TotalBits2("totalbits2", llvm::cl::value_desc("bits"),
    llvm::cl::desc("Total amount of bits in fixed point numbers"), llvm::cl::init(32));


void TaffoInitializer::readGlobalAnnotations(Module &m,
    MultiValueMap<Value *, ValueInfo>& variables,
		bool functionAnnotation)
{
  GlobalVariable *globAnnos = m.getGlobalVariable("llvm.global.annotations");

  if (globAnnos != NULL)
  {
    if (ConstantArray *annos = dyn_cast<ConstantArray>(globAnnos->getInitializer()))
    {
      for (unsigned i = 0, n = annos->getNumOperands(); i < n; i++)
      {
        if (ConstantStruct *anno = dyn_cast<ConstantStruct>(annos->getOperand(i)))
        {
          /* Structure of the expression (ConstantStruct operand #0 is the expression):
           * [OpType] operand:
           *   [BitCast] *function, [GetElementPtr] *annotation,
           *   [GetElementPtr] *filename, [Int] source code line */
          if (ConstantExpr *expr = dyn_cast<ConstantExpr>(anno->getOperand(0)))
          {
            if (expr->getOpcode() == Instruction::BitCast && (functionAnnotation ^ !isa<Function>(expr->getOperand(0))) )
            {
              parseAnnotation(variables, cast<ConstantExpr>(anno->getOperand(1)), expr->getOperand(0));
            }
          }
        }
      }
    }
  }
  if (functionAnnotation)
    removeNoFloatTy(variables);
}


void TaffoInitializer::readLocalAnnotations(llvm::Function &f, MultiValueMap<Value *, ValueInfo>& variables)
{

  bool found = false;
  for (inst_iterator iIt = inst_begin(&f), iItEnd = inst_end(&f); iIt != iItEnd; iIt++) {
    if (CallInst *call = dyn_cast<CallInst>(&(*iIt))) {
      if (!call->getCalledFunction())
        continue;

      if (call->getCalledFunction()->getName() == "llvm.var.annotation") {
        bool startingPoint = false;
        parseAnnotation(variables, cast<ConstantExpr>(iIt->getOperand(1)), iIt->getOperand(0), &startingPoint);
        found |= startingPoint;
      }
    }
  }
  if (found) {
    mdutils::MetadataManager::setStartingPoint(f);
  }
}


void TaffoInitializer::readAllLocalAnnotations(llvm::Module &m, MultiValueMap<Value *, ValueInfo>& res)
{
  for (Function &f: m.functions()) {
    MultiValueMap<Value *, ValueInfo> t;
    readLocalAnnotations(f, t);
    res.insert(res.end(), t.begin(), t.end());

    /* Otherwise dce pass ignores the function
     * (removed also where it's not required) */
    f.removeFnAttr(Attribute::OptimizeNone);
  }
}

// Return true on success, false on error
bool TaffoInitializer::parseAnnotation(MultiValueMap<Value *, ValueInfo>& variables,
				       ConstantExpr *annoPtrInst, Value *instr,
				       bool *startingPoint)
{
  ofstream declarationsFile;
  declarationsFile.open ("declarations", std::ios_base::app);
  ValueInfo vi;

  if (!(annoPtrInst->getOpcode() == Instruction::GetElementPtr))
    return false;
  GlobalVariable *annoContent = dyn_cast<GlobalVariable>(annoPtrInst->getOperand(0));
  if (!annoContent)
    return false;
  ConstantDataSequential *annoStr = dyn_cast<ConstantDataSequential>(annoContent->getInitializer());
  if (!annoStr)
    return false;
  if (!(annoStr->isString()))
    return false;

  StringRef annstr = annoStr->getAsString();
  AnnotationParser parser;
  if (!parser.parseAnnotationString(annstr)) {
    errs() << "TAFFO annnotation parser syntax error: \n";
    errs() << "  In annotation: \"" << annstr << "\"\n";
    errs() << "  " << parser.lastError() << "\n";
    return false;
  }
  vi.fixpTypeRootDistance = 0;
  if (!parser.backtracking)
    vi.backtrackingDepthLeft = 0;
  else
    vi.backtrackingDepthLeft = parser.backtrackingDepth;
  vi.metadata = parser.metadata;
  if (startingPoint)
    *startingPoint = parser.startingPoint;
  vi.target = parser.target;



  if (Instruction *toconv = dyn_cast<Instruction>(instr)) {
    variables.push_back(toconv->getOperand(0), vi);

    if(vi.metadata->isDeclaration()) {
      //printf("DECL #1");

      if (InputInfo *II = dyn_cast<InputInfo>(vi.metadata.get())) {
              Range* rng = II->IRange.get();

              FixedPointTypeGenError fpgerr;
              FPType fixedPoint = fixedPointTypeFromRange(*rng, &fpgerr, TotalBits2, FracThreshold2, 64, TotalBits2);

              if (fpgerr != FixedPointTypeGenError::InvalidRange) {
                int width = fixedPoint.getWidth();
                unsigned pointPos = fixedPoint.getPointPos();

                int integerPart = std::abs(width)-pointPos;
                unsigned fractionalPart = pointPos;

                const DebugLoc &location = toconv->getDebugLoc();

                if(location) {
                  declarationsFile << vi.target.getValue() + " " + to_string(location.getLine()) + " " + to_string(integerPart) + " " + to_string(fractionalPart) << endl;
                }
                else {
                  declarationsFile << vi.target.getValue() + " " + to_string(vi.metadata->getLocation()) + " " + to_string(integerPart) + " " + to_string(fractionalPart) + " function" << endl;
                }

              }

      }



    }

    
  } else if (Function *fun = dyn_cast<Function>(instr)) {
    enabledFunctions.insert(fun);
    for (auto user: fun->users()) {
      if (!(isa<CallInst>(user) || isa<InvokeInst>(user)))
        continue;
      variables.push_back(user, vi);
    }

    if(vi.metadata->isDeclaration()) {
        printf("DECL #2");

    }
    
  } else {

    // global variables declarations here
    if(vi.metadata->isDeclaration()) {

            if (InputInfo *II = dyn_cast<InputInfo>(vi.metadata.get())) {
              Range* rng = II->IRange.get();

              FixedPointTypeGenError fpgerr;
              FPType fixedPoint = fixedPointTypeFromRange(*rng, &fpgerr, TotalBits2, FracThreshold2, 64, TotalBits2);

              if (fpgerr != FixedPointTypeGenError::InvalidRange) {
                int width = fixedPoint.getWidth();
                unsigned pointPos = fixedPoint.getPointPos();

                int integerPart = std::abs(width)-pointPos;
                unsigned fractionalPart = pointPos;

                
                  declarationsFile << vi.target.getValue() + " " + to_string(vi.metadata->getLocation()) + " " + to_string(integerPart) + " " + to_string(fractionalPart) << endl;
                

              }

      }

    }
    variables.push_back(instr, vi);
  }

  declarationsFile.close();

  return true;
}


void TaffoInitializer::removeNoFloatTy(MultiValueMap<Value *, ValueInfo>& res)
{
  for (auto PIt: res) {
    Type *ty;
    Value *it = PIt->first;

    if (AllocaInst *alloca = dyn_cast<AllocaInst>(it)) {
      ty = alloca->getAllocatedType();
    } else if (GlobalVariable *global = dyn_cast<GlobalVariable>(it)) {
      ty = global->getType();
    } else if (isa<CallInst>(it) || isa<InvokeInst>(it)) {
      ty = it->getType();
      if (ty->isVoidTy())
        continue;
    } else {
      LLVM_DEBUG(dbgs() << "annotated instruction " << *it <<
        " not an alloca or a global, ignored\n");
      res.erase(it);
      continue;
    }

    while (ty->isArrayTy() || ty->isPointerTy()) {
      if (ty->isPointerTy())
        ty = ty->getPointerElementType();
      else
        ty = ty->getArrayElementType();
    }
    if (!ty->isFloatingPointTy()) {
      LLVM_DEBUG(dbgs() << "annotated instruction " << *it << " does not allocate a"
        " kind of float; ignored\n");
      res.erase(it);
    }
  }
}

void TaffoInitializer::printAnnotatedObj(Module &m)
{
  MultiValueMap<Value *, ValueInfo> res;

  readGlobalAnnotations(m, res, true);
  errs() << "Annotated Function: \n";
  if(!res.empty())
  {
    for (auto it : res)
    {
      errs() << " -> " << *it->first << "\n";
    }
    errs() << "\n";
  }

  res.clear();
  readGlobalAnnotations(m, res);
  errs() << "Global Set: \n";
  if(!res.empty())
  {
    for (auto it : res)
    {
      errs() << " -> " << *it->first << "\n";
    }
    errs() << "\n";
  }

  for (auto fIt=m.begin() , fItEnd=m.end() ; fIt!=fItEnd ; fIt++)
  {
    Function &f = *fIt;
    errs().write_escaped(f.getName()) << " : ";
    res.clear();
    readLocalAnnotations(f, res);
    if(!res.empty())
    {
      errs() << "\nLocal Set: \n";
      for (auto it : res)
      {
        errs() << " -> " << *it->first << "\n";
      }
    }
    errs() << "\n";
  }

}

