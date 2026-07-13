#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include <fstream>
#include <map>
#include <string>
#include <sstream>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Command line options
static cl::OptionCategory VisibilityCategory("Visibility Extractor Options");
static cl::opt<std::string> OutputFile("o", cl::desc("Output file for visibility data"),
                                       cl::value_desc("filename"), cl::cat(VisibilityCategory));

class VisibilityVisitor : public RecursiveASTVisitor<VisibilityVisitor>
{
private:
    ASTContext *Context;
    std::map<std::string, bool> MethodVisibility;

    // Helper to create a unique identifier for a method without using mangling
    std::string getMethodIdentifier(const CXXMethodDecl *MD)
    {
        std::string Result;
        llvm::raw_string_ostream OS(Result);

        // Include class name
        if (const CXXRecordDecl *RD = MD->getParent())
        {
            OS << RD->getQualifiedNameAsString() << "::";
        }

        // Include method name
        OS << MD->getNameAsString();

        // Include parameter types to differentiate overloads
        OS << "(";
        bool First = true;
        for (const ParmVarDecl *Param : MD->parameters())
        {
            if (!First)
                OS << ", ";
            First = false;

            // Get type string
            QualType Type = Param->getType();
            OS << Type.getAsString();
        }
        OS << ")";

        // For constructors/destructors, add a tag
        if (isa<CXXConstructorDecl>(MD))
        {
            OS << " [constructor]";
        }
        else if (isa<CXXDestructorDecl>(MD))
        {
            OS << " [destructor]";
        }

        OS.flush();
        return Result;
    }

public:
    explicit VisibilityVisitor(ASTContext *Context) : Context(Context) {}

    bool VisitCXXMethodDecl(CXXMethodDecl *MD)
    {
        // Skip declarations without bodies, implicit methods, etc.
        if (MD->isImplicit() || !MD->hasBody())
            return true;

        std::string MethodID = getMethodIdentifier(MD);
        bool IsPublic = MD->getAccess() == AS_public;

        // Also record the raw demangled name for easier lookup
        MethodVisibility[MethodID] = IsPublic;

        // Record the method name pattern used in LLVM pass
        std::string DemangedName = MD->getQualifiedNameAsString();
        if (!DemangedName.empty())
        {
            MethodVisibility[DemangedName] = IsPublic;
        }

        return true;
    }

    void saveVisibilityData(const std::string &Filename)
    {
        std::ofstream OutFile(Filename);
        if (!OutFile)
        {
            llvm::errs() << "Error: Could not open output file: " << Filename << "\n";
            return;
        }

        for (const auto &Entry : MethodVisibility)
        {
            OutFile << Entry.first << "," << (Entry.second ? "public" : "private") << "\n";
        }
        OutFile.close();
    }
};

class VisibilityASTConsumer : public ASTConsumer
{
private:
    VisibilityVisitor Visitor;
    std::string OutputFilename;

public:
    explicit VisibilityASTConsumer(ASTContext *Context, StringRef Filename)
        : Visitor(Context), OutputFilename(Filename) {}

    void HandleTranslationUnit(ASTContext &Context) override
    {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
        Visitor.saveVisibilityData(OutputFilename);
    }
};

class VisibilityExtractionAction : public ASTFrontendAction
{
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef File) override
    {
        return std::make_unique<VisibilityASTConsumer>(&CI.getASTContext(), OutputFile);
    }
};

int main(int argc, const char **argv)
{
    CommonOptionsParser OptionsParser(argc, argv, VisibilityCategory);
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());

    if (OutputFile.empty())
    {
        llvm::errs() << "Error: Output file must be specified with -o\n";
        return 1;
    }

    return Tool.run(newFrontendActionFactory<VisibilityExtractionAction>().get());
}