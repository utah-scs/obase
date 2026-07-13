#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Path.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Demangle/Demangle.h"
#include <set>
#include <map>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

using namespace llvm;

namespace
{
    struct GuideInstrumentationPass : public ModulePass
    {
        static char ID;

        // Function analysis data
        std::map<const Function *, bool> directGuideUse;    // Functions directly using Guide
        std::map<const Function *, bool> indirectGuideUse;  // Functions indirectly using Guide
        std::map<const Function *, bool> publicFunctionCache; // Cache of function visibility results
        std::map<std::string, bool> functionVisibility;       // Visibility data from files
        std::map<std::string, bool> cachedVisibility;         // Cache for demangled names

        // Map of functions to Guide operations they contain
        // (both CallInst and InvokeInst, hence generic Instruction*)
        std::map<const Function *, std::vector<Instruction *>> echoPtrOperations;

        // Existing data structures
        std::map<Value *, bool> echoPtrAllocs; // Track Guide allocations
        std::string visibilityDataDir;         // Visibility data directory

        GuideInstrumentationPass() : ModulePass(ID)
        {
            // Get visibility data directory from environment variable
            visibilityDataDir = getenv("VISIBILITY_DATA_DIR") ? getenv("VISIBILITY_DATA_DIR") : "./visibility_data";
        }

        void getAnalysisUsage(AnalysisUsage &AU) const override
        {
            // Require the CallGraph analysis
            AU.addRequired<CallGraphWrapperPass>();
            AU.addPreserved<CallGraphWrapperPass>();
        }

        // Helper to check if a string ends with a certain suffix
        bool endsWith(const std::string &str, const std::string &suffix)
        {
            if (str.length() < suffix.length())
            {
                return false;
            }
            return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
        }

        // Load visibility data for all modules
        void loadVisibilityData(Module &M)
        {
            if (!functionVisibility.empty())
            {
                // Already loaded
                return;
            }

            // Create a directory iterator to find any .visibility files
            std::error_code EC;
            sys::fs::directory_iterator DirIt(visibilityDataDir, EC);
            sys::fs::directory_iterator DirEnd;

            if (EC)
            {
                errs() << "Error opening directory " << visibilityDataDir << ": " << EC.message() << "\n";
                return;
            }

            bool foundAnyFile = false;
            for (; DirIt != DirEnd && !EC; DirIt.increment(EC))
            {
                std::string Path = DirIt->path();
                if (endsWith(Path, ".visibility"))
                {
                    std::ifstream VisFile(Path);
                    std::string Line;

                    while (std::getline(VisFile, Line))
                    {
                        auto CommaPos = Line.find(',');
                        if (CommaPos != std::string::npos)
                        {
                            std::string FuncName = Line.substr(0, CommaPos);
                            std::string Visibility = Line.substr(CommaPos + 1);
                            functionVisibility[FuncName] = (Visibility == "public");
                        }
                    }
                    foundAnyFile = true;
                    errs() << "Loaded visibility data from " << Path << "\n";
                }
            }

            if (!foundAnyFile)
            {
                errs() << "Warning: No visibility data found in " << visibilityDataDir
                       << ". Falling back to heuristics.\n";
            }
        }

        bool isInStringContext(Value *V)
        {
            if (!V)
                return false;

            // Check immediate users first
            for (User *U : V->users())
            {
                if (CallInst *CI = dyn_cast<CallInst>(U))
                {
                    if (Function *F = CI->getCalledFunction())
                    {
                        if (F->getName().contains("basic_string"))
                        {
                            return true;
                        }
                    }
                }
            }

            // Check parent block for string operations
            if (Instruction *I = dyn_cast<Instruction>(V))
            {
                BasicBlock *BB = I->getParent();
                for (Instruction &Inst : *BB)
                {
                    if (CallInst *Call = dyn_cast<CallInst>(&Inst))
                    {
                        if (Function *F = Call->getCalledFunction())
                        {
                            if (F->getName().contains("basic_string"))
                            {
                                return true;
                            }
                        }
                    }
                }
            }

            return false;
        }

        bool isConstructorCall(CallInst *CI)
        {
            if (Function *F = CI->getCalledFunction())
            {
                StringRef Name = F->getName();
                return Name.contains("C1E") || Name.contains("C2E");
            }
            return false;
        }

        bool isTemporaryGuide(Value *V)
        {
            // Check if we've already determined this
            if (echoPtrAllocs.count(V))
            {
                return echoPtrAllocs[V];
            }

            // Check if the value is an alloca instruction
            if (AllocaInst *AI = dyn_cast<AllocaInst>(V))
            {
                // Look through all uses of this alloca
                for (Use &U : AI->uses())
                {
                    if (StoreInst *SI = dyn_cast<StoreInst>(U.getUser()))
                    {
                        // Check if the stored value is from string operations
                        if (Value *StoredVal = SI->getValueOperand())
                        {
                            if (isInStringContext(StoredVal))
                            {
                                echoPtrAllocs[V] = true;
                                return true;
                            }
                        }
                        // If there's a store immediately after the alloca, likely temporary
                        if (SI->getValueOperand() == AI)
                        {
                            echoPtrAllocs[V] = true;
                            return true;
                        }
                    }
                }
            }

            echoPtrAllocs[V] = false;
            return false;
        }

        bool isInConstructionContext(Value *V)
        {
            if (Instruction *I = dyn_cast<Instruction>(V))
            {
                BasicBlock *BB = I->getParent();
                // Look for recent allocations
                for (auto it = I->getIterator(); it != BB->begin(); --it)
                {
                    if (CallInst *Call = dyn_cast<CallInst>(&*it))
                    {
                        if (Function *F = Call->getCalledFunction())
                        {
                            if (F->getName().contains("_Znwm") ||
                                F->getName().contains("Guide") ||
                                F->getName().contains("C1E") ||
                                F->getName().contains("C2E"))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        }

        bool shouldInstrumentOperation(Instruction *I)
        {
            // Handle both Call and Invoke instructions
            Function *calledFunc = nullptr;
            if (auto *CI = dyn_cast<CallInst>(I))
            {
                calledFunc = CI->getCalledFunction();
            }
            else if (auto *II = dyn_cast<InvokeInst>(I))
            {
                calledFunc = II->getCalledFunction();
            }

            if (!calledFunc)
                return false;

            StringRef Name = calledFunc->getName();

            if (!Name.startswith("_ZN5Guide"))
                return false;

            // Skip constructor calls
            if (Name.contains("C1E") || Name.contains("C2E"))
            {
                errs() << "Skipping constructor: " << Name << "\n";
                return false;
            }

            // Get the Guide object being operated on
            Value *echoPtrObj = nullptr;
            if (auto *CI = dyn_cast<CallInst>(I))
            {
                echoPtrObj = CI->getArgOperand(0);
            }
            else if (auto *II = dyn_cast<InvokeInst>(I))
            {
                echoPtrObj = II->getArgOperand(0);
            }

            if (isInConstructionContext(echoPtrObj))
            {
                errs() << "Skipping construction context: " << Name << "\n";
                return false;
            }

            // Match by mangled-name fragments, covering every Guide<T>
            // instantiation: cvPv/cvPc/... are operator T*() conversions,
            // aSEPv/aSEPc/... are operator=(T*). deEv (operator*) and
            // ptEv (operator->) exist on the generic template; no current
            // structure calls them, but a converted one may.
            if (Name.contains("cvP") ||       // operator T*()
                Name.contains("deEv") ||      // operator*()
                Name.contains("ptEv") ||      // operator->()
                Name.contains("destroyEv") || // destroy()
                Name.contains("aSEP"))        // operator=(T*)
            {
                errs() << "Including operation: " << Name << "\n";
                return true;
            }

            return false;
        }

        // Updated isPublicFunction method
        bool isPublicFunction(const Function &F)
        {
            // Check if we've already determined this
            if (publicFunctionCache.count(&F))
            {
                return publicFunctionCache[&F];
            }

            std::string MangledName = F.getName().str();

            // Check cache first
            if (cachedVisibility.count(MangledName))
            {
                bool result = cachedVisibility[MangledName];
                publicFunctionCache[&F] = result;
                return result;
            }

            // Demangle the name
            std::string DemangledName = demangle(MangledName.c_str());

            // Strip parameters for lookup
            size_t ParamStart = DemangledName.find('(');
            if (ParamStart != std::string::npos)
            {
                DemangledName = DemangledName.substr(0, ParamStart);
            }

            errs() << "Looking up visibility for: " << DemangledName << " (from " << MangledName << ")\n";

            // Look for exact match
            if (functionVisibility.count(DemangledName))
            {
                bool IsPublic = functionVisibility[DemangledName];
                cachedVisibility[MangledName] = IsPublic;
                publicFunctionCache[&F] = IsPublic;
                errs() << "Function determined to be " << (IsPublic ? "public" : "private")
                       << " from visibility data\n";
                return IsPublic;
            }

            // Try class::method format
            for (const auto &Entry : functionVisibility)
            {
                // Split into class::method
                size_t MethodStart = Entry.first.rfind("::");
                if (MethodStart != std::string::npos && MethodStart + 2 < Entry.first.length())
                {
                    std::string MethodName = Entry.first.substr(MethodStart + 2);
                    // Strip parameters from method name too
                    size_t MethodParamStart = MethodName.find('(');
                    if (MethodParamStart != std::string::npos)
                    {
                        MethodName = MethodName.substr(0, MethodParamStart);
                    }
                    // Strip constructor/destructor annotation
                    size_t MethodAnnotation = MethodName.find(" [");
                    if (MethodAnnotation != std::string::npos)
                    {
                        MethodName = MethodName.substr(0, MethodAnnotation);
                    }

                    // Check if method name is in demangled name
                    if (DemangledName.find(MethodName) != std::string::npos)
                    {
                        bool IsPublic = Entry.second;
                        cachedVisibility[MangledName] = IsPublic;
                        publicFunctionCache[&F] = IsPublic;
                        errs() << "Function determined to be " << (IsPublic ? "public" : "private")
                               << " from method name match with " << Entry.first << "\n";
                        return IsPublic;
                    }
                }
            }

            // Fall back to heuristics
            errs() << "No visibility data for " << DemangledName << ", using heuristics.\n";

            // Skip internal and private functions
            if (F.hasLocalLinkage())
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            StringRef Name = F.getName();

            // Skip compiler-generated functions and standard library functions
            if (Name.startswith("_ZN5Guide") ||
                Name.startswith("_ZNK5Guide") ||
                Name.startswith("_ZSt") ||
                Name.startswith("_ZNSs") ||
                Name.contains("basic_string"))
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            // Skip C++ language internal functions
            if (Name.contains("__cxx") ||
                Name.contains("__gnu") ||
                Name.contains("__clang"))
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            // Consider functions with implementation details as non-public
            if (Name.contains("internal") ||
                Name.contains("detail") ||
                Name.contains("impl") ||
                Name.contains("private"))
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            // Consider standard destructors and constructors as non-public
            if (Name.contains("D0Ev") ||
                Name.contains("D1Ev") ||
                Name.contains("D2Ev"))
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            // Skip logging functions
            if (Name.startswith("spdlog"))
            {
                publicFunctionCache[&F] = false;
                cachedVisibility[MangledName] = false;
                return false;
            }

            // Consider it a public function by default
            publicFunctionCache[&F] = true;
            cachedVisibility[MangledName] = true;
            return true;
        }

        // Identify direct Guide operations in functions
        void identifyDirectGuideOperations(Module &M)
        {
            for (Function &F : M)
            {
                if (F.isDeclaration() || shouldSkipFunction(F))
                    continue;

                directGuideUse[&F] = false;

                // Identify Guide operations in the function
                for (BasicBlock &BB : F)
                {
                    for (Instruction &I : BB)
                    {
                        // Track Guide allocations
                        if (auto *AI = dyn_cast<AllocaInst>(&I))
                        {
                            if (AI->getAllocatedType()->isStructTy() &&
                                AI->getAllocatedType()->getStructName().contains("Guide"))
                            {
                                isTemporaryGuide(AI);
                            }
                        }

                        // Find Guide operations in both CallInst and InvokeInst
                        if (isa<CallInst>(&I) || isa<InvokeInst>(&I))
                        {
                            if (shouldInstrumentOperation(&I))
                            {
                                directGuideUse[&F] = true;
                                // Store the instruction for later instrumentation
                                // (invokes are instrumented too: the addToTAG
                                // call is inserted just before the terminator)
                                echoPtrOperations[&F].push_back(&I);
                            }
                        }

                        // This is for trie_art in specific -- can remove this
                        // Also check for GetElementPtr instructions that access Guide fields
                        if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
                        {
                            if (GEP->getSourceElementType()->isStructTy())
                            {
                                StructType *ST = dyn_cast<StructType>(GEP->getSourceElementType());
                                if (ST && ST->getStructName().contains("KeyValuePair"))
                                {
                                    // KeyValuePair contains Guide fields
                                    directGuideUse[&F] = true;
                                    errs() << "Found KeyValuePair access in function: " << F.getName() << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }

        // Propagate Guide usage up the call chain using LLVM's CallGraph
        void propagateGuideUsage(Module &M)
        {
            // Get LLVM's call graph
            CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

            // Initialize based on direct usage
            for (Function &F : M)
            {
                if (!F.isDeclaration() && !shouldSkipFunction(F))
                {
                    indirectGuideUse[&F] = directGuideUse[&F];
                }
            }

            // Fixed-point iteration to propagate UP the call chain
            bool changed;
            do
            {
                changed = false;

                // Iterate through all call graph nodes
                for (auto &CGPair : CG)
                {
                    // Skip external calling node
                    if (!CGPair.first)
                        continue;

                    const Function *F = CGPair.first;
                    if (F->isDeclaration() || shouldSkipFunction(*F))
                        continue;

                    if (indirectGuideUse[F])
                        continue; // Already marked

                    // Check callee nodes
                    CallGraphNode *CGNode = CGPair.second.get();
                    for (auto &CallRecord : *CGNode)
                    {
                        CallGraphNode *CalleeNode = CallRecord.second;
                        Function *Callee = CalleeNode->getFunction();
                        if (Callee && !Callee->isDeclaration())
                        {
                            if (directGuideUse[Callee] || indirectGuideUse[Callee])
                            {
                                // This function indirectly uses Guide via a callee
                                indirectGuideUse[F] = true;
                                changed = true;
                                errs() << "Function " << F->getName()
                                       << " indirectly uses Guide via callee "
                                       << Callee->getName() << "\n";
                                break;
                            }
                        }
                    }
                }
            } while (changed);
        }

        // Decides if a function should be instrumented with scope guard
        bool shouldInstrumentFunction(const Function *F)
        {
            if (F->isDeclaration() || shouldSkipFunction(*F))
                return false;

            // Only instrument public functions that use Guide (directly or indirectly)
            bool isPublic = isPublicFunction(*F);
            bool usesGuide = directGuideUse[F] || indirectGuideUse[F];

            if (isPublic && usesGuide)
            {
                errs() << "Will instrument function " << F->getName()
                       << " (Public: " << isPublic
                       << ", Direct Guide Use: " << directGuideUse[F]
                       << ", Indirect Guide Use: " << indirectGuideUse[F] << ")\n";
                return true;
            }
            return false;
        }

        // Instrument a function with appropriate scope guards
        bool instrumentFunction(Function &F)
        {
            Module *M = F.getParent();
            LLVMContext &Ctx = M->getContext();
            bool modified = false;

            // Create scope guard type and functions
            StructType *scopeGuardTy = StructType::getTypeByName(Ctx, "struct.ActiveScopeGuard");
            if (!scopeGuardTy)
            {
                scopeGuardTy = StructType::create(Ctx, "struct.ActiveScopeGuard");
            }
            PointerType *scopeGuardPtrTy = PointerType::get(scopeGuardTy, 0);

            Function *createScopeGuardFunc = getOrCreateFunction(M, "createTAG", scopeGuardPtrTy, {});
            Function *destroyScopeGuardFunc = getOrCreateFunction(M, "destroyTAG", Type::getVoidTy(Ctx), {scopeGuardPtrTy});
            Function *addToScopeGuardFunc = getOrCreateFunction(M, "addToTAG", Type::getVoidTy(Ctx), {scopeGuardPtrTy, Type::getInt8PtrTy(Ctx)});

            // For public functions that use Guide, add scope guard
            IRBuilder<> entryBuilder(&F.getEntryBlock(), F.getEntryBlock().begin());
            Value *scopeGuard = entryBuilder.CreateCall(createScopeGuardFunc);
            errs() << "Created scope guard for public function: " << F.getName() << "\n";
            modified = true;

            // Add cleanup at all function exits: normal returns AND unwind
            // exits (resume). Missing the unwind path would leak the guard
            // and leave the thread's nesting level permanently skewed, so
            // ATC decrements for later operations would never fire.
            for (BasicBlock &BB : F)
            {
                Instruction *term = BB.getTerminator();
                if (isa<ReturnInst>(term) || isa<ResumeInst>(term))
                {
                    IRBuilder<> exitBuilder(term);
                    exitBuilder.CreateCall(destroyScopeGuardFunc, {scopeGuard});
                    errs() << "Inserted destroyTAG at "
                           << (isa<ReturnInst>(term) ? "return" : "resume")
                           << " in function: " << F.getName() << "\n";
                }
            }

            // If the function directly uses Guide, add reference tracking.
            // Insert addToTAG before EVERY Guide operation: deduplicating
            // by SSA value and instrumenting only the first occurrence is
            // unsound under control flow (a path can reach a later use
            // without executing the first one, leaving the dereference
            // untracked). The runtime pointer set deduplicates increments,
            // so redundant calls cost only a set lookup.
            if (directGuideUse[&F])
            {
                for (Instruction *OpInst : echoPtrOperations[&F])
                {
                    Value *echoPtrAddr = cast<CallBase>(OpInst)->getArgOperand(0);
                    IRBuilder<> builder(OpInst);
                    Value *castedPtr = builder.CreateBitCast(echoPtrAddr, Type::getInt8PtrTy(Ctx));
                    builder.CreateCall(addToScopeGuardFunc, {scopeGuard, castedPtr});
                    errs() << "Inserting addToTAG for function: " << F.getName() << "\n";
                }
            }

            return modified;
        }

        // Instrument private functions without creating guards
        bool instrumentPrivateGuideUse(Function &F)
        {
            if (!directGuideUse[&F])
            {
                return false;
            }

            Module *M = F.getParent();
            LLVMContext &Ctx = M->getContext();
            bool modified = false;

            // Create type and function references
            StructType *scopeGuardTy = StructType::getTypeByName(Ctx, "struct.ActiveScopeGuard");
            if (!scopeGuardTy)
            {
                scopeGuardTy = StructType::create(Ctx, "struct.ActiveScopeGuard");
            }
            PointerType *scopeGuardPtrTy = PointerType::get(scopeGuardTy, 0);
            Function *addToScopeGuardFunc = getOrCreateFunction(M, "addToTAG", Type::getVoidTy(Ctx), {scopeGuardPtrTy, Type::getInt8PtrTy(Ctx)});

            // NULL guard for private functions
            Value *nullGuard = ConstantPointerNull::get(scopeGuardPtrTy);

            // Add reference tracking with NULL guard, before EVERY operation
            // (see the comment in instrumentFunction for why deduplication
            // by SSA value is unsound)
            for (Instruction *OpInst : echoPtrOperations[&F])
            {
                Value *echoPtrAddr = cast<CallBase>(OpInst)->getArgOperand(0);
                IRBuilder<> builder(OpInst);
                Value *castedPtr = builder.CreateBitCast(echoPtrAddr, Type::getInt8PtrTy(Ctx));
                builder.CreateCall(addToScopeGuardFunc, {nullGuard, castedPtr});
                errs() << "Inserting addToTAG with NULL guard for private function: " << F.getName() << "\n";
                modified = true;
            }

            return modified;
        }

        bool runOnModule(Module &M) override
        {
            errs() << "Running GuideInstrumentationPass on module: " << M.getName() << "\n";

            // Clear previous data
            directGuideUse.clear();
            indirectGuideUse.clear();
            publicFunctionCache.clear();
            cachedVisibility.clear();
            echoPtrOperations.clear();
            echoPtrAllocs.clear();

            // Step 1: Load visibility data
            loadVisibilityData(M);

            // Step 2: Identify direct Guide operations in all functions
            identifyDirectGuideOperations(M);

            // Step 3: Propagate Guide usage up the call chain
            propagateGuideUsage(M);

            // Step 4: Instrument functions
            bool modified = false;

            // First, instrument public functions with scope guards
            for (Function &F : M)
            {
                if (shouldInstrumentFunction(&F))
                {
                    modified |= instrumentFunction(F);
                }
            }

            // Then, add addToTAG calls to private functions that directly use Guide
            for (Function &F : M)
            {
                if (!F.isDeclaration() && !isPublicFunction(F) && directGuideUse[&F])
                {
                    modified |= instrumentPrivateGuideUse(F);
                }
            }

            return modified;
        }

    private:
        bool shouldSkipFunction(const Function &F)
        {
            StringRef Name = F.getName();
            if (Name.startswith("_ZN5Guide") &&
                (Name.contains("destroy") ||
                 Name.contains("operator=") ||
                 Name.contains("Guide")))
            {
                return true;
            }
            if (Name.startswith("_ZNK5Guide") ||
                Name.startswith("_ZSt") ||
                Name.startswith("_ZNSs"))
            {
                return true;
            }
            if (Name.startswith("spdlog"))
            {
                return true;
            }
            if (Name.contains("basic_string"))
            {
                return true;
            }
            return false;
        }

        Function *getOrCreateFunction(Module *M, StringRef Name, Type *RetTy, ArrayRef<Type *> ArgTypes)
        {
            Function *F = M->getFunction(Name);
            if (!F)
            {
                FunctionType *FTy = FunctionType::get(RetTy, ArgTypes, false);
                F = Function::Create(FTy, Function::ExternalLinkage, Name, M);
            }
            return F;
        }
    };
}

char GuideInstrumentationPass::ID = 0;
static RegisterPass<GuideInstrumentationPass> X("guide-instrumentation", "OBASE guide/TAG instrumentation pass");

static RegisterStandardPasses Y(
    PassManagerBuilder::EP_EarlyAsPossible,
    [](const PassManagerBuilder &Builder, legacy::PassManagerBase &PM)
    {
        PM.add(new GuideInstrumentationPass());
    });