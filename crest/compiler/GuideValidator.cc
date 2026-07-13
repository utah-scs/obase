/*
 * OBASE validation pass
 *
 * Runs on every data-structure translation unit before IR emission and
 * rejects uses of managed objects the runtime cannot support, so they fail
 * the build instead of silently miscompiling:
 *
 *   [arithmetic]   Pointer arithmetic or indexing on an address obtained
 *                  from a guide dereference: `(char *)(void *)g->val + n`,
 *                  `((char *)(void *)g->val)[i]`. Migration relocates
 *                  objects independently; adjacent objects are not
 *                  contiguous and offsets past the object are meaningless.
 *                  (memcpy/memcmp of the object through its raw address is
 *                  fine -- the address itself is pinned for the operation.)
 *
 *   [slot-alias]   Casting a guide slot's address (`Guide<T> *`, `&field`)
 *                  to a raw pointer or integer type. The 64-bit slot holds
 *                  tagged metadata, not an address; raw aliases bypass the
 *                  migration protocol.
 *
 *   [varargs]      Passing a Guide object through a variadic argument
 *                  (undefined behavior for non-trivial classes; the intent
 *                  is almost always the raw address).
 *
 * Diagnostics are limited to the data-structure sources themselves; the
 * runtime headers legitimately manipulate slots and are exempt.
 *
 * Usage:
 *     guide-validator <file.cc> -- <cflags>
 *
 * Exits nonzero if any violation is found.
 */

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include <string>

using namespace clang;
using namespace clang::tooling;

static llvm::cl::OptionCategory ValidatorCategory("Guide Validator Options");

namespace
{

class ValidatorVisitor : public RecursiveASTVisitor<ValidatorVisitor>
{
    ASTContext &Ctx;
    unsigned violations = 0;

    static bool isGuideRecord(const CXXRecordDecl *RD)
    {
        return RD && RD->getIdentifier() && RD->getName() == "Guide";
    }

    static bool isGuideType(QualType QT)
    {
        return isGuideRecord(QT->getAsCXXRecordDecl());
    }

    static bool isGuidePointerType(QualType QT)
    {
        return QT->isPointerType() && isGuideType(QT->getPointeeType());
    }

    // True if E, after stripping parentheses and every implicit/explicit
    // cast, bottoms out at a guide dereference: either a call to one of
    // Guide's conversion/deref operators, or a guide-typed member access.
    static bool derivesFromGuideDeref(const Expr *E)
    {
        E = E->IgnoreParenCasts();
        if (const auto *MC = dyn_cast<CXXMemberCallExpr>(E))
        {
            const CXXMethodDecl *MD = MC->getMethodDecl();
            if (MD && isGuideRecord(MC->getRecordDecl()) &&
                (isa<CXXConversionDecl>(MD) ||
                 MD->getOverloadedOperator() == OO_Star ||
                 MD->getOverloadedOperator() == OO_Arrow))
                return true;
            return false;
        }
        if (const auto *OC = dyn_cast<CXXOperatorCallExpr>(E))
        {
            if (OC->getNumArgs() > 0)
                return isGuideType(OC->getArg(0)->IgnoreParenImpCasts()->getType());
            return false;
        }
        return isGuideType(E->getType());
    }

    // Diagnostics only for the data-structure sources; the runtime headers
    // (Guide.hpp and friends) legitimately manipulate slot words.
    bool inScope(SourceLocation Loc) const
    {
        const SourceManager &SM = Ctx.getSourceManager();
        SourceLocation ELoc = SM.getExpansionLoc(Loc);
        if (SM.isInSystemHeader(ELoc))
            return false;
        StringRef file = SM.getFilename(ELoc);
        if (file.contains("/runtime/") || file.endswith("Guide.hpp"))
            return false;
        return true;
    }

    void report(SourceLocation Loc, const char *rule, const std::string &msg)
    {
        const SourceManager &SM = Ctx.getSourceManager();
        llvm::errs() << SM.getExpansionLoc(Loc).printToString(SM)
                     << ": error: [obase-validate:" << rule << "] " << msg << "\n";
        violations++;
    }

public:
    explicit ValidatorVisitor(ASTContext &Ctx) : Ctx(Ctx) {}

    unsigned violationCount() const { return violations; }

    bool VisitBinaryOperator(BinaryOperator *BO)
    {
        if (!inScope(BO->getOperatorLoc()))
            return true;
        switch (BO->getOpcode())
        {
        case BO_Add:
        case BO_Sub:
        case BO_AddAssign:
        case BO_SubAssign:
            break;
        default:
            return true;
        }
        // Only pointer/address arithmetic is a contiguity assumption;
        // integer arithmetic on lengths etc. is fine.
        if (!BO->getType()->isPointerType() &&
            !BO->getLHS()->getType()->isPointerType() &&
            !BO->getRHS()->getType()->isPointerType())
            return true;
        if (derivesFromGuideDeref(BO->getLHS()) || derivesFromGuideDeref(BO->getRHS()))
        {
            report(BO->getOperatorLoc(), "arithmetic",
                   "pointer arithmetic on a managed object's address; objects "
                   "relocate independently and are not contiguous");
        }
        return true;
    }

    bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE)
    {
        if (!inScope(ASE->getRBracketLoc()))
            return true;
        if (derivesFromGuideDeref(ASE->getBase()))
        {
            report(ASE->getRBracketLoc(), "arithmetic",
                   "indexing into a managed object's address; objects "
                   "relocate independently and are not contiguous");
        }
        return true;
    }

    bool VisitExplicitCastExpr(ExplicitCastExpr *CE)
    {
        if (!inScope(CE->getBeginLoc()))
            return true;

        const Expr *Sub = CE->getSubExpr()->IgnoreParenImpCasts();
        QualType SubTy = Sub->getType();
        QualType Target = CE->getTypeAsWritten();

        // Address of a guide slot escaping to a raw POINTER type: creates a
        // store path that bypasses the migration protocol. Integer casts of
        // the address are allowed -- that is how SODA slot indices are
        // computed, both in the runtime and in protocol-level structure
        // code (e.g. the lock-free update path in ht_harris).
        bool subIsSlotAddr = isGuidePointerType(SubTy);
        if (const auto *UO = dyn_cast<UnaryOperator>(Sub))
        {
            if (UO->getOpcode() == UO_AddrOf &&
                isGuideType(UO->getSubExpr()->getType()))
                subIsSlotAddr = true;
        }
        if (subIsSlotAddr && Target->isPointerType() && !isGuidePointerType(Target))
        {
            report(CE->getBeginLoc(), "slot-alias",
                   "casting a guide slot's address to '" + Target.getAsString() +
                   "'; the slot holds tagged metadata and must only be "
                   "updated through Guide methods");
        }
        return true;
    }

    bool VisitCallExpr(CallExpr *CE)
    {
        if (!inScope(CE->getBeginLoc()))
            return true;
        const FunctionDecl *FD = CE->getDirectCallee();
        if (!FD || !FD->isVariadic())
            return true;
        for (unsigned i = FD->getNumParams(); i < CE->getNumArgs(); ++i)
        {
            const Expr *Arg = CE->getArg(i)->IgnoreParenImpCasts();
            if (isGuideType(Arg->getType()))
            {
                report(Arg->getBeginLoc(), "varargs",
                       "Guide object passed through a variadic argument; "
                       "cast to void* to pass the raw address");
            }
        }
        return true;
    }
};

class ValidatorConsumer : public ASTConsumer
{
public:
    void HandleTranslationUnit(ASTContext &Ctx) override
    {
        ValidatorVisitor V(Ctx);
        V.TraverseDecl(Ctx.getTranslationUnitDecl());
        if (V.violationCount() > 0)
        {
            llvm::errs() << "guide-validator: " << V.violationCount()
                         << " violation(s)\n";
            exit(1);
        }
    }
};

class ValidatorAction : public ASTFrontendAction
{
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &, StringRef) override
    {
        return std::make_unique<ValidatorConsumer>();
    }
};

} // namespace

int main(int argc, const char **argv)
{
    CommonOptionsParser OptionsParser(argc, argv, ValidatorCategory);
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<ValidatorAction>().get());
}
