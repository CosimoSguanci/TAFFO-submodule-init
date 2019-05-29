#include <cmath>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include "TaffoInitializerPass.h"
#include "TypeUtils.h"
#include "Metadata.h"


using namespace llvm;
using namespace taffo;


char TaffoInitializer::ID = 0;

static RegisterPass<TaffoInitializer> X(
  "taffoinit",
  "TAFFO Framework Initialization Stage",
  false /* does not affect the CFG */,
  true /* Optimization Pass (sorta) */);


llvm::cl::opt<bool> ManualFunctionCloning("manualclone",
    llvm::cl::desc("Enables function cloning only for annotated functions"), llvm::cl::init(false));


bool TaffoInitializer::runOnModule(Module &m)
{
  DEBUG_WITH_TYPE(DEBUG_ANNOTATION, printAnnotatedObj(m));

  llvm::SmallPtrSet<llvm::Value *, 32> local;
  llvm::SmallPtrSet<llvm::Value *, 32> global;
  readAllLocalAnnotations(m, local);
  readGlobalAnnotations(m, global, true);
  readGlobalAnnotations(m, global, false);

  std::vector<Value*> rootsa(local.begin(), local.end());
  rootsa.insert(rootsa.begin(), global.begin(), global.end());
  AnnotationCount = rootsa.size();

  std::vector<Value*> vals;
  buildConversionQueueForRootValues(rootsa, vals);
  for (Value *v: vals) {
    setMetadataOfValue(v);
  }
  removeAnnotationCalls(vals);

  SmallPtrSet<Function*, 10> callTrace;
  generateFunctionSpace(vals, global, callTrace);

  LLVM_DEBUG(printConversionQueue(vals));
  setFunctionArgsMetadata(m);

  return true;
}


void TaffoInitializer::removeAnnotationCalls(std::vector<Value*>& q)
{
  for (auto i = q.begin(); i != q.end();) {
    Value *v = *i;
    
    if (CallInst *anno = dyn_cast<CallInst>(v)) {
      if (anno->getCalledFunction()) {
        if (anno->getCalledFunction()->getName() == "llvm.var.annotation") {
          anno->eraseFromParent();
          i = q.erase(i);
          continue;
        }
      }
    }
    
    // TODO: remove global annotations
    
    i++;
  }
}


void removeRangeErrorFromMetadata(std::shared_ptr<mdutils::MDInfo> mdinfo)
{
  SmallVector<std::shared_ptr<mdutils::MDInfo>, 1> list({mdinfo});
  while (list.size() > 0) {
    std::shared_ptr<mdutils::MDInfo> cur = list.pop_back_val();
    if (!cur.get())
      continue;
    if (mdutils::InputInfo *ii = dyn_cast<mdutils::InputInfo>(cur.get())) {
      ii->IRange.reset();
      ii->IError.reset();
    } else if (mdutils::StructInfo *si = dyn_cast<mdutils::StructInfo>(cur.get())){
      list.append(si->begin(), si->end());
    }
  }
}


void TaffoInitializer::setMetadataOfValue(Value *v)
{
  ValueInfo& vi = *valueInfo(v);
  std::shared_ptr<mdutils::MDInfo> md = vi.metadata;
  
  if (isa<Instruction>(v) || isa<GlobalObject>(v)) {
    mdutils::MetadataManager::setInputInfoInitWeightMetadata(v, vi.fixpTypeRootDistance);
  }

  if (Instruction *inst = dyn_cast<Instruction>(v)) {
    if (vi.target.hasValue())
      mdutils::MetadataManager::setTargetMetadata(*inst, vi.target.getValue());

    if (mdutils::InputInfo *ii = dyn_cast<mdutils::InputInfo>(md.get())) {
      mdutils::MetadataManager::setInputInfoMetadata(*inst, *ii);
    } else if (mdutils::StructInfo *si = dyn_cast<mdutils::StructInfo>(md.get())) {
      mdutils::MetadataManager::setStructInfoMetadata(*inst, *si);
    }
  } else if (GlobalObject *con = dyn_cast<GlobalObject>(v)) {
    if (vi.target.hasValue())
      mdutils::MetadataManager::setTargetMetadata(*con, vi.target.getValue());

    if (mdutils::InputInfo *ii = dyn_cast<mdutils::InputInfo>(md.get())) {
      mdutils::MetadataManager::setInputInfoMetadata(*con, *ii);
    } else if (mdutils::StructInfo *si = dyn_cast<mdutils::StructInfo>(md.get())) {
      mdutils::MetadataManager::setStructInfoMetadata(*con, *si);
    }
  }
}


void TaffoInitializer::setFunctionArgsMetadata(Module &m)
{
  std::vector<mdutils::MDInfo *> iiPVec;
  std::vector<int> wPVec;
  for (Function &f : m.functions()) {
    LLVM_DEBUG(dbgs() << "Processing function " << f.getName() << "\n");
    iiPVec.reserve(f.arg_size());
    wPVec.reserve(f.arg_size());

    for (const Argument &a : f.args()) {
      LLVM_DEBUG(dbgs() << "Processing arg " << a << "\n");
      mdutils::MDInfo *ii = nullptr;
      int weight = -1;
      for (const Use &u : a.uses()) {
        Value *sv = u.getUser();
        LLVM_DEBUG(dbgs() << "Processing use " << *sv << "\n");
        if (isa<StoreInst>(sv)) {
          if (hasInfo(sv)) {
            LLVM_DEBUG(dbgs() << "Info found.\n");
            ValueInfo &vi = *valueInfo(sv);
            ii = vi.metadata.get();
	    weight = vi.fixpTypeRootDistance;
            break;
          }
        }
      }
      iiPVec.push_back(ii);
      wPVec.push_back(weight);
    }

    mdutils::MetadataManager::setArgumentInputInfoMetadata(f, iiPVec);
    mdutils::MetadataManager::setInputInfoInitWeightMetadata(&f, wPVec);

    iiPVec.clear();
    wPVec.clear();
  }
}


void TaffoInitializer::buildConversionQueueForRootValues(
  const ArrayRef<Value*>& val,
  std::vector<Value*>& queue)
{
  queue.insert(queue.begin(), val.begin(), val.end());
  for (auto i = queue.begin(); i != queue.end(); i++) {
    valueInfo(*i)->isRoot = true;
  }

  SmallPtrSet<Value *, 8U> visited;
  size_t prevQueueSize = 0;
  while (prevQueueSize < queue.size()) {
    LLVM_DEBUG(dbgs() << "***** buildConversionQueueForRootValues iter " << prevQueueSize << " < " << queue.size() << "\n";);
    prevQueueSize = queue.size();

    size_t next = 0;
    while (next < queue.size()) {
      Value *v = queue.at(next);
      visited.insert(v);

      for (auto *u: v->users()) {
        /* ignore u if it is the global annotation array */
        if (GlobalObject *ugo = dyn_cast<GlobalObject>(u)) {
          if (ugo->hasSection() && ugo->getSection() == "llvm.metadata")
            continue;
        }

	if (isa<PHINode>(u) && visited.count(u)) {
	  continue;
	}

        /* Insert u at the end of the queue.
         * If u exists already in the queue, *move* it to the end instead. */
        for (int i=0; i<queue.size();) {
          if (queue[i] == u) {
            queue.erase(queue.begin() + i);
            if (i < next)
              next--;
          } else {
            i++;
          }
        }
        queue.push_back(u);

        if (valueInfo(v)->isBacktrackingNode) {
          valueInfo(u)->isBacktrackingNode = true;
        }
        createInfoOfUser(v, u);
      }
      next++;
    }

    next = queue.size();
    for (next = queue.size(); next != 0; next--) {
      Value *v = queue.at(next-1);
      if (!(valueInfo(v)->isBacktrackingNode))
        continue;

      Instruction *inst = dyn_cast<Instruction>(v);
      if (!inst)
        continue;

      #ifdef LOG_BACKTRACK
      dbgs() << "BACKTRACK ";
      v->print(dbgs());
      dbgs() << "\n";
      #endif

      for (Value *u: inst->operands()) {
        if (!isa<User>(u) && !isa<Argument>(u)) {
          #ifdef LOG_BACKTRACK
          dbgs() << " - " ;
          u->printAsOperand(dbgs());
          dbgs() << " not a User or an Argument\n";
          #endif
          continue;
        }

        if (isa<Function>(u) || isa<BlockAddress>(u)) {
          #ifdef LOG_BACKTRACK
          dbgs() << " - " ;
          u->printAsOperand(dbgs());
          dbgs() << " is a function/block address\n";
          #endif
          continue;
        }

        #ifdef LOG_BACKTRACK
        dbgs() << " - " << *u;
        #endif

        if (!isFloatType(u->getType())) {
          #ifdef LOG_BACKTRACK
          dbgs() << " not a float\n";
          #endif
          continue;
        }
        valueInfo(v)->isRoot = false;

        valueInfo(u)->isBacktrackingNode = true;

        bool alreadyIn = false;
        for (int i=0; i<queue.size() && !alreadyIn;) {
          if (queue[i] == u) {
            if (i < next)
              alreadyIn = true;
            else
              queue.erase(queue.begin() + i);
          } else {
            i++;
          }
        }
        if (!alreadyIn) {
          valueInfo(u)->isRoot = true;
          #ifdef LOG_BACKTRACK
          dbgs() << "  enqueued\n";
          #endif
          queue.insert(queue.begin()+next-1, u);
          next++;
        } else {
          #ifdef LOG_BACKTRACK
          dbgs() << " already in\n";
          #endif
        }

        createInfoOfUser(v, u);
      }
    }
  }

  for (Value *v: queue) {
    if (valueInfo(v)->isRoot) {
      valueInfo(v)->roots = {v};
    }

    SmallPtrSet<Value*, 5> newroots = valueInfo(v)->roots;
    for (Value *u: v->users()) {
      if (!hasInfo(u))
        continue;

      auto oldroots = valueInfo(u)->roots;
      SmallPtrSet<Value*, 5> merge(newroots);
      merge.insert(oldroots.begin(), oldroots.end());
      valueInfo(u)->roots = merge;
    }
  }
}


void TaffoInitializer::createInfoOfUser(Value *used, Value *user)
{
  ValueInfo vinfo = *valueInfo(used);
  ValueInfo &uinfo = *valueInfo(user);
  
  /* Copy metadata from the closest instruction to a root */
  if (!(uinfo.fixpTypeRootDistance <= std::max(vinfo.fixpTypeRootDistance, vinfo.fixpTypeRootDistance+1))) {
    /* Do not copy metadata in case of type conversions from struct to
     * non-struct and vice-versa.
     * We could check the instruction type and copy the correct type
     * contained in the struct type or create a struct type with the
     * correct type in the correct place, but is'a huge mess */
    Type *usedt = fullyUnwrapPointerOrArrayType(used->getType());
    Type *usert = fullyUnwrapPointerOrArrayType(user->getType());
    bool copyok = (usedt == usert);
    copyok |= (!usedt->isStructTy() && !usert->isStructTy()) || isa<StoreInst>(user);
    if (copyok) {
      uinfo.metadata.reset(vinfo.metadata->clone());
    } else if (std::shared_ptr<mdutils::MDInfo> gepimdi = extractGEPIMetadata(user, used, vinfo.metadata)) {
      uinfo.metadata = gepimdi;
    } else {
      uinfo.metadata = mdutils::StructInfo::constructFromLLVMType(usert);
      if (uinfo.metadata.get() == nullptr) {
        uinfo.metadata.reset(new mdutils::InputInfo(nullptr, nullptr, nullptr, true));
      }
    }
  
    uinfo.target = vinfo.target;
    uinfo.fixpTypeRootDistance = std::max(vinfo.fixpTypeRootDistance, vinfo.fixpTypeRootDistance+1);
  }

  /* The conversion enabling flag shall be true if at least one of the parents
   * of the children has it enabled */
  mdutils::InputInfo *iiu = dyn_cast_or_null<mdutils::InputInfo>(uinfo.metadata.get());
  mdutils::InputInfo *iiv = dyn_cast_or_null<mdutils::InputInfo>(vinfo.metadata.get());
  if (iiu && iiv && iiv->IEnableConversion) {
    iiu->IEnableConversion = true;
  }
}

std::shared_ptr<mdutils::MDInfo>
TaffoInitializer::extractGEPIMetadata(const llvm::Value *v_gepi,
				      const llvm::Value *pop,
				      std::shared_ptr<mdutils::MDInfo> mdi)
{
  using namespace mdutils;
  assert(v_gepi && pop && mdi);
  const GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(v_gepi);
  if (!gepi || gepi->getPointerOperand() != pop)
    return nullptr;

  Type* source_element_type = gepi->getSourceElementType();
  for (auto idx_it = gepi->idx_begin() + 1; // skip first index
       idx_it != gepi->idx_end(); ++idx_it) {
    if (isa<SequentialType>(source_element_type))
      continue;

    if (const llvm::ConstantInt* int_i = dyn_cast<llvm::ConstantInt>(*idx_it)) {
      int n = static_cast<int>(int_i->getSExtValue());
      mdi = cast<StructInfo>(mdi.get())->getField(n);
      source_element_type =
	cast<StructType>(source_element_type)->getTypeAtIndex(n);
    } else {
      return nullptr;
    }
  }
  return (mdi) ? std::shared_ptr<mdutils::MDInfo>(mdi.get()->clone()) : nullptr;
}


void TaffoInitializer::generateFunctionSpace(std::vector<Value *> &vals, SmallPtrSetImpl<llvm::Value *> &global,
                                             SmallPtrSet<Function *, 10> &callTrace)
{
  for (Value *v: vals) {
    if (!(isa<CallInst>(v) || isa<InvokeInst>(v)))
      continue;
    CallSite *call = new CallSite(v);
    
    Function *oldF = call->getCalledFunction();
    if (!oldF) {
      LLVM_DEBUG(dbgs() << "found bitcasted funcptr in " << *v << "\n");
      assert(0 && "bitcasted function pointers and such not handled atm");
    }
    if(isSpecialFunction(oldF))
      continue;
    if (ManualFunctionCloning) {
      if (enabledFunctions.count(oldF) == 0) {
        LLVM_DEBUG(dbgs() << "skipped cloning of function from call " << *v << ": function disabled\n");
        continue;
      }
    }

    std::vector<Value*> newVals;
    Function *newF = createFunctionAndQueue(call, global, newVals);
    call->setCalledFunction(newF);
    enabledFunctions.insert(newF);

    //Attach metadata
    MDNode *newFRef = MDNode::get(call->getInstruction()->getContext(),ValueAsMetadata::get(newF));
    MDNode *oldFRef = MDNode::get(call->getInstruction()->getContext(),ValueAsMetadata::get(oldF));

    call->getInstruction()->setMetadata(ORIGINAL_FUN_METADATA, oldFRef);
    if (MDNode *cloned = oldF->getMetadata(CLONED_FUN_METADATA)) {
      cloned = cloned->concatenate(cloned, newFRef);
      oldF->setMetadata(CLONED_FUN_METADATA, cloned);
    } else {
      oldF->setMetadata(CLONED_FUN_METADATA, newFRef);
    }
    newF->setMetadata(CLONED_FUN_METADATA, NULL);
    newF->setMetadata(SOURCE_FUN_METADATA, oldFRef);
    for (Value *v : newVals) {
      setMetadataOfValue(v);
    }
    
    /* Reconstruct the value info for the values which are in the top-level
     * conversion queue and in the oldF
     * Allows us to properly process call functions */
    for (BasicBlock& bb: *newF) {
      for (Instruction& i: bb) {
        if (mdutils::MDInfo *mdi = mdutils::MetadataManager::getMetadataManager().retrieveMDInfo(&i)) {
          newVals.push_back(&i);
          valueInfo(&i)->metadata.reset(mdi->clone());
        }
      }
    }

    if (callTrace.count(oldF)) {
      continue;
    }
    callTrace.insert(oldF);
    callTrace.insert(newF);
    generateFunctionSpace(newVals, global, callTrace);
    callTrace.erase(newF);
    callTrace.erase(oldF);
  }
}


Function* TaffoInitializer::createFunctionAndQueue(CallSite *call, SmallPtrSetImpl<Value *> &global,
    std::vector<Value *> &convQueue)
{
  Function *oldF = call->getCalledFunction();
  Function *newF = Function::Create(
      oldF->getFunctionType(), oldF->getLinkage(),
      oldF->getName(), oldF->getParent());

  ValueToValueMapTy mapArgs; // Create Val2Val mapping and clone function
  Function::arg_iterator newArgumentI = newF->arg_begin();
  Function::arg_iterator oldArgumentI = oldF->arg_begin();
  for (; oldArgumentI != oldF->arg_end() ; oldArgumentI++, newArgumentI++) {
    newArgumentI->setName(oldArgumentI->getName());
    mapArgs.insert(std::make_pair(oldArgumentI, newArgumentI));
  }
  SmallVector<ReturnInst*,100> returns;
  CloneFunctionInto(newF, oldF, mapArgs, true, returns);
  FunctionCloned++;

  std::vector<Value*> roots; //propagate fixp conversion
  oldArgumentI = oldF->arg_begin();
  newArgumentI = newF->arg_begin();
  LLVM_DEBUG(dbgs() << "Create function from " << oldF->getName() << " to " << newF->getName() << "\n";);
  for (int i=0; oldArgumentI != oldF->arg_end() ; oldArgumentI++, newArgumentI++, i++) {
    Value *callOperand = call->getInstruction()->getOperand(i);
    Value *allocaOfArgument = newArgumentI->user_begin()->getOperand(1);
    
    if (!hasInfo(callOperand))
      continue;
  
    ValueInfo& callVi = *valueInfo(callOperand);
    ValueInfo& allocaVi = *valueInfo(allocaOfArgument);
    ValueInfo& argumentVi = *valueInfo(newArgumentI);
    
    // Mark the alloca used for the argument (in O0 opt lvl)
    // let it be a root in VRA-less mode
    allocaVi.metadata.reset(callVi.metadata->clone());
    allocaVi.fixpTypeRootDistance = callVi.fixpTypeRootDistance+2;
    roots.push_back(allocaOfArgument);
    
    LLVM_DEBUG(dbgs() << "  Arg nr. " << i << " processed, isRoot = " << allocaVi.isRoot << "\n");
    LLVM_DEBUG(dbgs() << "    md = " << allocaVi.metadata->toString() << "\n");
    
    // Mark the argument itself (set it as a new root as well in VRA-less mode)
    argumentVi.metadata.reset(callVi.metadata->clone());
    argumentVi.fixpTypeRootDistance = callVi.fixpTypeRootDistance+1;
  }

  std::vector<Value*> tmpVals;
  roots.insert(roots.begin(), global.begin(), global.end());
  SmallPtrSet<Value*, 32> localFix;
  readLocalAnnotations(*newF, localFix);
  roots.insert(roots.begin(), localFix.begin(), localFix.end());
  buildConversionQueueForRootValues(roots, tmpVals);
  for (Value *val : tmpVals){
    if (Instruction *inst = dyn_cast<Instruction>(val)) {
      if (inst->getFunction()==newF){
        convQueue.push_back(val);
      }
    }
  }
  return newF;
}


void TaffoInitializer::printConversionQueue(std::vector<Value*> vals)
{
  if (vals.size() < 1000) {
    errs() << "conversion queue:\n";
    for (Value *val: vals) {
      errs() << "bt=" << valueInfo(val)->isBacktrackingNode << " ";
      errs() << "md=" << valueInfo(val)->metadata->toString() << " ";
      errs() << "[";
      for (Value *rootv: valueInfo(val)->roots) {
        rootv->print(errs());
        errs() << ' ';
      }
      errs() << "] ";
      val->print(errs());
      errs() << "\n";
    }
    errs() << "\n\n";
  } else {
    errs() << "not printing the conversion queue because it exceeds 1000 items";
  }
}

