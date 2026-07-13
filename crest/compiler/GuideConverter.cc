/*
 * OBASE Pass 2: pointer-to-guide conversion.
 *
 * Rewrites developer-annotated pointer declarations into guides and repairs
 * the usages that stop compiling after the type change. Developers mark
 * fields with OBASE_GUIDED (runtime/GuideAnnotations.h):
 *
 *     struct Node {
 *         OBASE_GUIDED void *key;     -->   Guide<void> key;
 *         OBASE_GUIDED void *val;     -->   Guide<void> val;
 *         Node *next;                       (untouched)
 *     };
 *
 * Rewrites performed:
 *   1. Annotated pointer field/variable declarations become Guide<T>.
 *   2. Explicit casts of a guided expression to a non-void pointer or an
 *      integer are repaired by materializing the raw address first:
 *          (char *)node->val   -->   (char *)static_cast<void *>(node->val)
 *      (Guide's conversion operator yields T*; a further conversion to an
 *      unrelated pointer type needs the intermediate step spelled out.)
 *   3. The GuideAnnotations.h include in each converted file is replaced
 *      with Guide.hpp.
 *
 * Everything else compiles unchanged through Guide's operator overloads;
 * Pass 3 later instruments the resulting Guide method calls with TAG/ATC
 * hooks. Lifecycle rules (jemalloc allocation of guided objects, never
 * annotating routing metadata) remain the developer's contract -- see
 * GuideAnnotations.h.
 *
 * Usage:
 *     guide-converter [-inplace] [-suffix=.converted] <file.cc> -- <cflags>
 *
 * By default converted copies are written next to the originals with the
 * suffix appended; -inplace overwrites the sources.
 */

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

static llvm::cl::OptionCategory ConverterCategory("Guide Converter Options");
static llvm::cl::opt<bool> InPlace("inplace",
                                   llvm::cl::desc("Overwrite the source files"),
                                   llvm::cl::init(false), llvm::cl::cat(ConverterCategory));
static llvm::cl::opt<std::string> Suffix("suffix",
                                         llvm::cl::desc("Suffix for converted copies (default .converted)"),
                                         llvm::cl::init(".converted"), llvm::cl::cat(ConverterCategory));

static const char *GUIDED_ANNOTATION = "obase::guided";

namespace
{

struct IncludeSite
{
    FileID file;
    CharSourceRange range; // "#include ".../GuideAnnotations.h"" directive
};

// Records where GuideAnnotations.h is included so the directive can be
// swapped for Guide.hpp in converted files.
class IncludeRecorder : public PPCallbacks
{
    SourceManager &SM;
    std::vector<IncludeSite> &sites;

public:
    IncludeRecorder(SourceManager &SM, std::vector<IncludeSite> &sites)
        : SM(SM), sites(sites) {}

    void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef FileName,
                            bool, CharSourceRange FilenameRange, const FileEntry *,
                            StringRef, StringRef, const Module *,
                            SrcMgr::CharacteristicKind) override
    {
        if (FileName.endswith("GuideAnnotations.h"))
        {
            sites.push_back({SM.getFileID(HashLoc),
                             CharSourceRange::getCharRange(HashLoc, FilenameRange.getEnd())});
        }
    }
};

class ConverterVisitor : public RecursiveASTVisitor<ConverterVisitor>
{
    ASTContext &Ctx;
    Rewriter &R;
    std::set<const ValueDecl *> guidedDecls;
    unsigned declsConverted = 0;
    unsigned castsRepaired = 0;
    unsigned errors = 0;

    bool hasGuidedAnnotation(const Decl *D) const
    {
        for (const auto *A : D->specific_attrs<AnnotateAttr>())
        {
            if (A->getAnnotation() == GUIDED_ANNOTATION)
                return true;
        }
        return false;
    }

    void report(SourceLocation Loc, const std::string &kind, const std::string &msg)
    {
        const SourceManager &SM = Ctx.getSourceManager();
        llvm::errs() << SM.getExpansionLoc(Loc).printToString(SM)
                     << ": " << kind << ": " << msg << "\n";
    }

    // Rewrites "T *name" of an annotated declarator to "Guide<T> name" and
    // deletes the annotation token.
    void convertDecl(DeclaratorDecl *D)
    {
        const SourceManager &SM = Ctx.getSourceManager();
        QualType QT = D->getType();

        if (!QT->isPointerType())
        {
            report(D->getLocation(), "error",
                   "OBASE_GUIDED requires a pointer type (got '" + QT.getAsString() + "')");
            errors++;
            return;
        }
        QualType Pointee = QT->getPointeeType();
        if (Pointee->isPointerType())
        {
            report(D->getLocation(), "error",
                   "multi-level pointers cannot be annotated; guide the leaf "
                   "objects or declare Guide<...>* by hand");
            errors++;
            return;
        }
        if (isa<ParmVarDecl>(D))
        {
            report(D->getLocation(), "error",
                   "parameters cannot be annotated; guides live in fields, "
                   "callers pass raw pointers");
            errors++;
            return;
        }

        TypeSourceInfo *TSI = D->getTypeSourceInfo();
        if (!TSI)
            return;
        TypeLoc TL = TSI->getTypeLoc();
        auto PTL = TL.getAsAdjusted<PointerTypeLoc>();
        if (PTL.isNull())
        {
            report(D->getLocation(), "error", "unsupported declarator shape");
            errors++;
            return;
        }

        // Replace "OBASE_GUIDED T *" (annotation through the star) with
        // "Guide<T> " in one edit. When the annotation is not directly in
        // front of the type, delete it separately.
        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressTagKeyword = true;
        std::string guideType = "Guide<" + Pointee.getAsString(PP) + "> ";

        SourceLocation typeBegin = SM.getExpansionLoc(TL.getBeginLoc());
        SourceLocation sigil = SM.getExpansionLoc(PTL.getSigilLoc());
        SourceLocation replaceBegin = typeBegin;
        for (const auto *A : D->specific_attrs<AnnotateAttr>())
        {
            if (A->getAnnotation() != GUIDED_ANNOTATION)
                continue;
            CharSourceRange attrRange = SM.getExpansionRange(A->getRange());
            if (SM.isBeforeInTranslationUnit(attrRange.getBegin(), typeBegin))
                replaceBegin = attrRange.getBegin();
            else
                R.ReplaceText(attrRange, "");
        }
        R.ReplaceText(CharSourceRange::getTokenRange(replaceBegin, sigil), guideType);
        declsConverted++;
    }

public:
    ConverterVisitor(ASTContext &Ctx, Rewriter &R) : Ctx(Ctx), R(R) {}

    unsigned errorCount() const { return errors; }
    unsigned convertedCount() const { return declsConverted; }
    unsigned repairedCount() const { return castsRepaired; }

    bool VisitFieldDecl(FieldDecl *D)
    {
        if (hasGuidedAnnotation(D))
            convertDecl(D);
        return true;
    }

    bool VisitVarDecl(VarDecl *D)
    {
        if (hasGuidedAnnotation(D))
            convertDecl(D);
        return true;
    }

    // Repair explicit casts whose operand is (directly) an annotated decl.
    // After conversion the operand is a Guide; its conversion operator
    // produces T*, so a cast to an unrelated pointer type or an integer
    // needs the raw address materialized first.
    bool VisitExplicitCastExpr(ExplicitCastExpr *CE)
    {
        const Expr *Sub = CE->getSubExpr()->IgnoreParenImpCasts();
        const ValueDecl *VD = nullptr;
        if (const auto *ME = dyn_cast<MemberExpr>(Sub))
            VD = ME->getMemberDecl();
        else if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub))
            VD = DRE->getDecl();
        if (!VD || !guidedDecls.count(VD))
            return true;

        QualType Target = CE->getTypeAsWritten();
        bool needsRepair = false;
        if (Target->isPointerType() && !Target->getPointeeType()->isVoidType())
            needsRepair = true; // (char *)guide
        else if (Target->isIntegerType())
            needsRepair = true; // (uintptr_t)guide
        if (!needsRepair)
            return true;

        const SourceManager &SM = Ctx.getSourceManager();
        SourceLocation Begin = SM.getExpansionLoc(Sub->getBeginLoc());
        SourceLocation End = SM.getExpansionLoc(Sub->getEndLoc());
        R.InsertTextBefore(Begin, "static_cast<void *>(");
        R.InsertTextAfterToken(End, ")");
        castsRepaired++;
        return true;
    }

    // First pass over the TU: collect the annotated decls so cast repair can
    // recognize references to them regardless of visitation order.
    void collectGuidedDecls(TranslationUnitDecl *TU)
    {
        struct Collector : RecursiveASTVisitor<Collector>
        {
            ConverterVisitor &Outer;
            Collector(ConverterVisitor &O) : Outer(O) {}
            bool VisitFieldDecl(FieldDecl *D)
            {
                if (Outer.hasGuidedAnnotation(D))
                    Outer.guidedDecls.insert(D);
                return true;
            }
            bool VisitVarDecl(VarDecl *D)
            {
                if (Outer.hasGuidedAnnotation(D))
                    Outer.guidedDecls.insert(D);
                return true;
            }
        } C(*this);
        C.TraverseDecl(TU);
    }
};

class ConverterConsumer : public ASTConsumer
{
    Rewriter &R;
    std::vector<IncludeSite> &includeSites;

public:
    ConverterConsumer(Rewriter &R, std::vector<IncludeSite> &sites)
        : R(R), includeSites(sites) {}

    void HandleTranslationUnit(ASTContext &Ctx) override
    {
        ConverterVisitor V(Ctx, R);
        V.collectGuidedDecls(Ctx.getTranslationUnitDecl());
        V.TraverseDecl(Ctx.getTranslationUnitDecl());

        if (V.errorCount() > 0)
        {
            llvm::errs() << "guide-converter: " << V.errorCount()
                         << " error(s); no output written\n";
            exit(1);
        }

        // Swap the annotations include for the runtime header in every file
        // the rewrite touched.
        const SourceManager &SM = Ctx.getSourceManager();
        std::set<FileID> touched;
        for (auto it = R.buffer_begin(); it != R.buffer_end(); ++it)
            touched.insert(it->first);
        for (const auto &site : includeSites)
        {
            if (touched.count(site.file))
                R.ReplaceText(site.range, "#include \"Guide.hpp\"");
        }

        // Emit converted files.
        unsigned filesWritten = 0;
        for (auto it = R.buffer_begin(); it != R.buffer_end(); ++it)
        {
            const FileEntry *FE = SM.getFileEntryForID(it->first);
            if (!FE)
                continue;
            std::string outPath = FE->getName().str();
            if (!InPlace)
                outPath += Suffix;
            std::error_code EC;
            llvm::raw_fd_ostream out(outPath, EC, llvm::sys::fs::OF_Text);
            if (EC)
            {
                llvm::errs() << "guide-converter: cannot write " << outPath
                             << ": " << EC.message() << "\n";
                exit(1);
            }
            it->second.write(out);
            filesWritten++;
            llvm::errs() << "guide-converter: wrote " << outPath << "\n";
        }

        llvm::errs() << "guide-converter: " << V.convertedCount()
                     << " declaration(s) converted, " << V.repairedCount()
                     << " cast(s) repaired, " << filesWritten << " file(s) written\n";
    }
};

class ConverterAction : public ASTFrontendAction
{
    Rewriter R;
    std::vector<IncludeSite> includeSites;

public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef) override
    {
        R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        CI.getPreprocessor().addPPCallbacks(
            std::make_unique<IncludeRecorder>(CI.getSourceManager(), includeSites));
        return std::make_unique<ConverterConsumer>(R, includeSites);
    }
};

} // namespace

int main(int argc, const char **argv)
{
    CommonOptionsParser OptionsParser(argc, argv, ConverterCategory);
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<ConverterAction>().get());
}
